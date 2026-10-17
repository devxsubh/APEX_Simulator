/* Unit tests for the PELT port. Each test states the kernel property it checks. */
#include "pelt.h"
#include "trace.h"

#include <math.h>
#include <stdio.h>

static int failures, checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* Documentation/scheduler/sched-pelt.c: table[i] = (long)(0xffffffff * y^i). */
static void test_table_regenerates(void) {
    double y = pow(0.5, 1.0 / PELT_LOAD_AVG_PERIOD);
    for (int i = 0; i < PELT_LOAD_AVG_PERIOD; i++) {
        uint32_t x = (uint32_t)(long)(((1UL << 32) - 1) * pow(y, i));
        CHECK(x == pelt_runnable_avg_yN_inv[i], "table[%d] = 0x%08x, generator gives 0x%08x", i,
              pelt_runnable_avg_yN_inv[i], x);
    }
}

/* Same generator: iterate max = max*y + 1024 in fixed point until it stops changing. */
static void test_load_avg_max_converges(void) {
    double y = pow(0.5, 1.0 / PELT_LOAD_AVG_PERIOD);
    long y_inv = (long)(((1UL << 32) - 1) * y);
    long max = 1024, last = 0;
    while (last != max) {
        last = max;
        max = ((max * y_inv) >> 32) + 1024;
    }
    CHECK(max == PELT_LOAD_AVG_MAX, "converged max %ld, expected %d", max, PELT_LOAD_AVG_MAX);
}

static void test_decay_load(void) {
    CHECK(pelt_decay_load(12345, 0) == 12344, "n=0 multiplies by 0xffffffff/2^32 and truncates");
    CHECK(pelt_decay_load(1 << 20, 32) == (1 << 19) - 1, "one half-life halves (minus truncation)");
    CHECK(pelt_decay_load(1 << 20, 64) == (1 << 18) - 1, "two half-lives quarter");
    CHECK(pelt_decay_load(UINT64_MAX, 32 * 63 + 1) == 0, "beyond 63 half-lives returns 0");
    CHECK(pelt_decay_load(PELT_LOAD_AVG_MAX, 1) == 46718, "LOAD_AVG_MAX * y");
    uint64_t prev = UINT64_MAX;
    for (uint64_t n = 0; n <= 32 * 63; n++) {
        uint64_t v = pelt_decay_load(1ULL << 40, n);
        CHECK(v <= prev, "decay_load must be non-increasing in n (n=%llu)", (unsigned long long)n);
        prev = v;
    }
}

/* An entity updated once per period while always running, from zero. */
static PeltAvg run_always(uint64_t periods, uint64_t start) {
    PeltAvg a = {0};
    a.last_update_time = start;
    for (uint64_t p = 1; p <= periods; p++) pelt_update(start + p * PELT_PERIOD_NS, &a, 1, 1, 1, 1024);
    return a;
}

static void test_saturation_and_halflife(void) {
    PeltAvg a = run_always(2000, 0);
    CHECK(a.util_avg >= 1020 && a.util_avg <= 1024, "always-running util saturates near 1024, got %lu", a.util_avg);
    CHECK(a.load_avg >= 1020 && a.load_avg <= 1024, "load_avg with weight 1024 saturates, got %lu", a.load_avg);

    PeltAvg r = run_always(32, 0);
    CHECK(fabs((double)r.util_avg - 512.0) <= 512.0 * 0.02, "one half-life from 0 reaches ~512, got %lu", r.util_avg);

    /* Then sleep for one half-life: util roughly halves. */
    uint64_t t = 2000 * PELT_PERIOD_NS;
    pelt_update(t + 32 * PELT_PERIOD_NS, &a, 0, 0, 0, 1024);
    CHECK(fabs((double)a.util_avg - 512.0) <= 512.0 * 0.02, "one half-life of sleep halves util, got %lu",
          a.util_avg);
}

/* Updating more often must not change the signal by more than integer rounding. */
static void test_update_cadence(void) {
    PeltAvg coarse = {0}, fine = {0};
    uint64_t end = 200 * PELT_PERIOD_NS;
    pelt_update(end, &coarse, 1, 1, 1, 1024);
    for (uint64_t t = 250000; t <= end; t += 250000) pelt_update(t, &fine, 1, 1, 1, 1024);
    pelt_update(end, &fine, 1, 1, 1, 1024);
    long d = (long)coarse.util_avg - (long)fine.util_avg;
    CHECK(d >= -2 && d <= 2, "update cadence changes util by %ld (coarse %lu, fine %lu)", d, coarse.util_avg,
          fine.util_avg);
}

/* LOAD_AVG_MAX is the fixed point of max*y + 1024, so one crossed period adds
 * nothing from the middle segment: only the decayed d1 (1024 * y = 1002). */
static void test_accumulate_segments(void) {
    CHECK(pelt_accumulate_segments(1, 1024, 0) == 1002, "one full period contributes 1002, got %u",
          pelt_accumulate_segments(1, 1024, 0));
    /* Via the c2 formula the same 1024 * y is 47742 - decay(47742, 2) - 1024 = 1001: one more truncation. */
    CHECK(pelt_accumulate_segments(2, 0, 0) == 1001, "an empty d1 plus one middle period gives 1001, got %u",
          pelt_accumulate_segments(2, 0, 0));
}

/* Ramp-time model from the research plan: n_a = P * log2(1/(1-a)) periods. */
static void test_ramp_matches_theory(void) {
    for (unsigned shift = 0; shift <= 2; shift++) {
        TraceConfig cfg;
        trace_defaults(&cfg);
        cfg.wl.kind = WL_STEP;
        cfg.wl.start_ms = 50;
        cfg.wl.stop_ms = 1050;
        cfg.wl.duration_ms = 1100;
        cfg.pelt_shift = shift;
        TraceSummary r;
        run_trace(&cfg, &r);
        double period_ms = PELT_PERIOD_NS / 1e6 / (double)(1u << shift);
        double t80 = 32.0 * log2(1.0 / 0.2) * period_ms;
        double t95 = 32.0 * log2(1.0 / 0.05) * period_ms;
        /* Tolerance: one sample (1.05 ms) plus 3% for integer rounding. */
        CHECK(fabs(r.ramp80_ms - t80) <= 1.05 + 0.03 * t80, "shift %u: ramp80 %.2f ms, model %.2f ms", shift,
              r.ramp80_ms, t80);
        CHECK(fabs(r.ramp95_ms - t95) <= 1.05 + 0.03 * t95, "shift %u: ramp95 %.2f ms, model %.2f ms", shift,
              r.ramp95_ms, t95);
    }
}

/* Frequency invariance: a 25%-duty task reads ~256 whether the CPU runs at full or half speed. */
static void test_frequency_invariance(void) {
    double util[2];
    unsigned long freqs[2] = {1024, 512};
    for (int k = 0; k < 2; k++) {
        TraceConfig cfg;
        trace_defaults(&cfg);
        cfg.wl.kind = WL_PERIODIC;
        cfg.wl.period_ms = 8;
        cfg.wl.duty = 0.25;
        cfg.wl.duration_ms = 2000;
        cfg.freq_scale = freqs[k];
        TraceSummary r;
        run_trace(&cfg, &r);
        util[k] = r.mean_cpu_util;
        CHECK(fabs(r.busy_pct - 25.0 * 1024.0 / freqs[k]) < 1.0, "freq %lu: busy %.1f%%", freqs[k], r.busy_pct);
    }
    CHECK(fabs(util[0] - 256.0) < 256.0 * 0.05, "full speed mean util %.1f, expected ~256", util[0]);
    CHECK(fabs(util[1] - util[0]) < 256.0 * 0.05, "half speed mean util %.1f vs full %.1f", util[1], util[0]);
}

/* Same seed, same result; different seed, different bursty workload. */
static void test_seed_reproducibility(void) {
    TraceConfig cfg;
    trace_defaults(&cfg);
    cfg.wl.kind = WL_BURSTY;
    cfg.wl.ntasks = 3;
    TraceSummary a, b, c;
    run_trace(&cfg, &a);
    run_trace(&cfg, &b);
    cfg.wl.seed = 2;
    run_trace(&cfg, &c);
    CHECK(a.mean_cpu_util == b.mean_cpu_util && a.switches == b.switches, "same seed must reproduce exactly");
    CHECK(a.mean_cpu_util != c.mean_cpu_util, "different seeds should differ");
}

int main(void) {
    test_table_regenerates();
    test_load_avg_max_converges();
    test_decay_load();
    test_saturation_and_halflife();
    test_update_cadence();
    test_accumulate_segments();
    test_ramp_matches_theory();
    test_frequency_invariance();
    test_seed_reproducibility();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
