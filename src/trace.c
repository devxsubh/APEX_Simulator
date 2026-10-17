#include "trace.h"

#include "pelt.h"
#include "rng.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NICE0_WEIGHT 1024UL

typedef struct {
    TaskGen gen;
    PeltAvg avg;
    bool on_rq;
    double vruntime;
    unsigned long weight;
} SimTask;

typedef struct {
    SimTask tasks[WL_MAX_TASKS];
    int n;
    int curr;               /* running task, -1 when idle */
    double min_vruntime;
    PeltAvg cfs;            /* CPU-level (cfs_rq) signal, the one schedutil reads */
    PeltClock clk;
    uint64_t pelt_updates;
    uint64_t switches;
} Cpu;

void trace_defaults(TraceConfig *cfg) {
    memset(cfg, 0, sizeof *cfg);
    workload_defaults(&cfg->wl);
    cfg->sample_ns = PELT_PERIOD_NS;
    cfg->hz = 250;
    cfg->cpu_scale = SCHED_CAPACITY_SCALE;
    cfg->freq_scale = SCHED_CAPACITY_SCALE;
}

static unsigned long cfs_load(const Cpu *c) {
    unsigned long w = 0;
    for (int i = 0; i < c->n; i++) if (c->tasks[i].on_rq) w += c->tasks[i].weight;
    return w;
}

static unsigned long cfs_nr_running(const Cpu *c) {
    unsigned long n = 0;
    for (int i = 0; i < c->n; i++) n += c->tasks[i].on_rq;
    return n;
}

/* update_load_avg() for one task and its cfs_rq, using the state that held
 * since their last update (the kernel calls it before changing state). */
static void update_task(Cpu *c, int i) {
    uint64_t now = pelt_clock_now(&c->clk);
    SimTask *t = &c->tasks[i];
    pelt_update(now, &t->avg, t->on_rq, t->on_rq, c->curr == i, t->weight);
    pelt_update(now, &c->cfs, cfs_load(c), cfs_nr_running(c), c->curr >= 0, 1);
    c->pelt_updates++;
}

static void update_cfs(Cpu *c) {
    pelt_update(pelt_clock_now(&c->clk), &c->cfs, cfs_load(c), cfs_nr_running(c), c->curr >= 0, 1);
    c->pelt_updates++;
}

static int pick_next(const Cpu *c) {
    int best = -1;
    for (int i = 0; i < c->n; i++) {
        if (!c->tasks[i].on_rq) continue;
        if (best < 0 || c->tasks[i].vruntime < c->tasks[best].vruntime) best = i;
    }
    return best;
}

static void switch_to(Cpu *c, int next) {
    if (next == c->curr) return;
    if (c->curr >= 0) update_task(c, c->curr); /* put_prev */
    if (next >= 0) update_task(c, next);       /* set_next */
    else update_cfs(c);
    c->curr = next;
    c->switches++;
}

/* Non-mutating read of a signal as it would look if updated now (for sampling). */
static PeltAvg peek(const PeltAvg *a, uint64_t now, unsigned long sum_load, unsigned long runnable,
                    int running, unsigned long avg_load) {
    PeltAvg copy = *a;
    pelt_update(now, &copy, sum_load, runnable, running, avg_load);
    return copy;
}

int run_trace(const TraceConfig *cfg, TraceSummary *out) {
    const WorkloadSpec *wl = &cfg->wl;
    if (wl->ntasks < 1 || wl->ntasks > WL_MAX_TASKS) {
        fprintf(stderr, "ntasks must be 1..%d\n", WL_MAX_TASKS);
        return 1;
    }
    FILE *csv = NULL;
    if (cfg->csv_path) {
        csv = fopen(cfg->csv_path, "w");
        if (!csv) {
            perror(cfg->csv_path);
            return 1;
        }
        fprintf(csv, "t_ms,task,on_rq,running,pending_us,util_avg,load_avg,runnable_avg\n");
    }

    Rng rng;
    rng_seed(&rng, wl->seed);
    Cpu c;
    memset(&c, 0, sizeof c);
    c.n = wl->ntasks;
    c.curr = -1;
    pelt_clock_init(&c.clk, cfg->pelt_shift);
    TaskGen gens[WL_MAX_TASKS];
    workload_init(wl, gens, &rng);
    for (int i = 0; i < c.n; i++) {
        c.tasks[i].gen = gens[i];
        c.tasks[i].weight = NICE0_WEIGHT;
    }

    memset(out, 0, sizeof *out);
    out->ramp80_ms = out->ramp95_ms = -1.0;
    double rate = (double)cfg->cpu_scale * (double)cfg->freq_scale / (double)(1UL << 20); /* work ns per ns */
    uint64_t end = (uint64_t)llround(wl->duration_ms * 1e6);
    uint64_t tick_ns = 1000000000ULL / cfg->hz;
    uint64_t now = 0, next_sample = 0, next_tick = tick_ns, busy_ns = 0;
    double util_total = 0.0;

    while (now < end) {
        /* Releases and wakeups due now. */
        for (int i = 0; i < c.n; i++) {
            SimTask *t = &c.tasks[i];
            workload_release(wl, &t->gen, (double)now, &rng);
            bool want = t->gen.pending_ns > 0.0;
            if (want && !t->on_rq) {
                update_task(&c, i); /* enqueue: decay the blocked period first */
                if (t->vruntime < c.min_vruntime) t->vruntime = c.min_vruntime;
                t->on_rq = true;
                if (c.curr >= 0 && t->vruntime < c.tasks[c.curr].vruntime) switch_to(&c, i);
            } else if (!want && t->on_rq) {
                if (c.curr == i) switch_to(&c, -1);
                update_task(&c, i); /* dequeue */
                t->on_rq = false;
            }
        }
        if (c.curr < 0) switch_to(&c, pick_next(&c));

        /* Sample. */
        if (now >= next_sample) {
            uint64_t pnow = pelt_clock_now(&c.clk);
            PeltAvg cpu = peek(&c.cfs, pnow, cfs_load(&c), cfs_nr_running(&c), c.curr >= 0, 1);
            if (csv) {
                fprintf(csv, "%.4f,cpu,%lu,%d,,%lu,%lu,%lu\n", now / 1e6, cfs_nr_running(&c), c.curr >= 0,
                        cpu.util_avg, cpu.load_avg, cpu.runnable_avg);
                for (int i = 0; i < c.n; i++) {
                    SimTask *t = &c.tasks[i];
                    PeltAvg a = peek(&t->avg, pnow, t->on_rq, t->on_rq, c.curr == i, t->weight);
                    fprintf(csv, "%.4f,%d,%d,%d,%.3f,%lu,%lu,%lu\n", now / 1e6, i, t->on_rq, c.curr == i,
                            t->gen.pending_ns / 1e3, a.util_avg, a.load_avg, a.runnable_avg);
                }
            }
            util_total += (double)cpu.util_avg;
            if (cpu.util_avg > out->max_cpu_util) out->max_cpu_util = cpu.util_avg;
            if (wl->kind == WL_STEP && now >= wl->start_ms * 1e6) {
                double since = now / 1e6 - wl->start_ms;
                if (out->ramp80_ms < 0 && cpu.util_avg >= 0.80 * SCHED_CAPACITY_SCALE) out->ramp80_ms = since;
                if (out->ramp95_ms < 0 && cpu.util_avg >= 0.95 * SCHED_CAPACITY_SCALE) out->ramp95_ms = since;
            }
            out->samples++;
            next_sample += cfg->sample_ns;
        }

        /* Next boundary: sample, tick, release, completion of curr, or end. */
        uint64_t next = end;
        if (next_sample < next) next = next_sample;
        if (next_tick < next) next = next_tick;
        for (int i = 0; i < c.n; i++) {
            double r = workload_next_event(wl, &c.tasks[i].gen, (double)now);
            if (isfinite(r) && r < (double)next) {
                uint64_t ri = (uint64_t)ceil(r);
                next = ri > now ? ri : now + 1;
            }
        }
        if (c.curr >= 0) {
            uint64_t done = now + (uint64_t)ceil(c.tasks[c.curr].gen.pending_ns / rate);
            if (done < next) next = done > now ? done : now + 1;
        }

        uint64_t dt = next - now;
        bool busy = c.curr >= 0;
        pelt_clock_advance(&c.clk, dt, busy, cfg->cpu_scale, cfg->freq_scale, c.cfs.util_sum);
        if (busy) {
            SimTask *t = &c.tasks[c.curr];
            t->gen.pending_ns -= (double)dt * rate;
            if (t->gen.pending_ns < 1e-6) t->gen.pending_ns = 0.0;
            t->vruntime += (double)dt * NICE0_WEIGHT / (double)t->weight;
            busy_ns += dt;
        }
        now = next;

        double mv = INFINITY;
        for (int i = 0; i < c.n; i++) if (c.tasks[i].on_rq && c.tasks[i].vruntime < mv) mv = c.tasks[i].vruntime;
        if (isfinite(mv) && mv > c.min_vruntime) c.min_vruntime = mv;

        if (now >= next_tick) {
            /* scheduler_tick(): update curr (or the idle cfs_rq), then maybe preempt. */
            if (c.curr >= 0) update_task(&c, c.curr);
            else update_cfs(&c);
            int best = pick_next(&c);
            if (best >= 0 && best != c.curr && c.curr >= 0 &&
                c.tasks[best].vruntime < c.tasks[c.curr].vruntime) {
                switch_to(&c, best);
            }
            next_tick += tick_ns;
        }
    }

    out->busy_pct = end > 0 ? 100.0 * (double)busy_ns / (double)end : 0.0;
    out->mean_cpu_util = out->samples ? util_total / (double)out->samples : 0.0;
    out->pelt_updates = c.pelt_updates;
    out->switches = c.switches;
    if (csv) fclose(csv);
    return 0;
}
