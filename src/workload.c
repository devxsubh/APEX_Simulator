#include "workload.h"

#include <math.h>
#include <string.h>

#define NS_PER_MS 1e6

void workload_defaults(WorkloadSpec *spec) {
    memset(spec, 0, sizeof *spec);
    spec->kind = WL_PERIODIC;
    spec->ntasks = 1;
    spec->seed = 1;
    spec->duration_ms = 1000.0;
    spec->period_ms = 16.0;
    spec->duty = 0.25;
    spec->duty_hi = 0.8;
    spec->start_ms = 100.0;
    spec->stop_ms = 600.0;
    spec->burst_rate_hz = 20.0;
    spec->burst_mean_ms = 5.0;
}

static const char *kind_names[] = {"step", "periodic", "bursty", "phase"};

const char *workload_name(WorkloadKind kind) { return kind_names[kind]; }

int workload_parse_kind(const char *name, WorkloadKind *kind) {
    for (int k = 0; k < 4; k++) {
        if (strcmp(name, kind_names[k]) == 0) {
            *kind = (WorkloadKind)k;
            return 1;
        }
    }
    return 0;
}

static double phase_duty(const WorkloadSpec *spec, double now_ns) {
    double third = spec->duration_ms * NS_PER_MS / 3.0;
    return (now_ns >= third && now_ns < 2.0 * third) ? spec->duty_hi : spec->duty;
}

static double jittered_period_ns(const WorkloadSpec *spec, Rng *rng) {
    double p = spec->period_ms * NS_PER_MS;
    if (spec->jitter > 0.0) p *= 1.0 + spec->jitter * (2.0 * rng_uniform(rng) - 1.0);
    return p;
}

void workload_init(const WorkloadSpec *spec, TaskGen *gens, Rng *rng) {
    for (int i = 0; i < spec->ntasks; i++) {
        TaskGen *g = &gens[i];
        memset(g, 0, sizeof *g);
        switch (spec->kind) {
            case WL_STEP:
                g->next_release_ns = spec->start_ms * NS_PER_MS;
                break;
            case WL_PERIODIC:
            case WL_PHASE:
                /* Random phase per task so several tasks do not wake in lockstep. */
                g->next_release_ns = spec->ntasks > 1 ? rng_uniform(rng) * spec->period_ms * NS_PER_MS : 0.0;
                break;
            case WL_BURSTY:
                g->next_release_ns = rng_exp(rng, 1e9 / spec->burst_rate_hz);
                break;
        }
    }
}

void workload_release(const WorkloadSpec *spec, TaskGen *g, double now_ns, Rng *rng) {
    if (spec->kind == WL_STEP) {
        double start = spec->start_ms * NS_PER_MS, stop = spec->stop_ms * NS_PER_MS;
        /* Always busy inside the window: keep more work pending than can ever run. */
        if (now_ns >= start && now_ns < stop) {
            g->pending_ns = stop - now_ns;
            g->next_release_ns = stop;
        } else if (now_ns >= stop) {
            g->pending_ns = 0.0;
            g->next_release_ns = INFINITY;
        }
        return;
    }
    while (g->next_release_ns <= now_ns) {
        double t = g->next_release_ns;
        if (spec->kind == WL_BURSTY) {
            g->pending_ns += rng_exp(rng, spec->burst_mean_ms * NS_PER_MS);
            g->next_release_ns = t + rng_exp(rng, 1e9 / spec->burst_rate_hz);
        } else {
            double duty = spec->kind == WL_PHASE ? phase_duty(spec, t) : spec->duty;
            g->pending_ns += duty * spec->period_ms * NS_PER_MS;
            g->next_release_ns = t + jittered_period_ns(spec, rng);
        }
        g->released++;
    }
}

double workload_next_event(const WorkloadSpec *spec, const TaskGen *g, double now_ns) {
    (void)spec;
    (void)now_ns;
    return g->next_release_ns;
}
