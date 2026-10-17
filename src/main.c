#include "approx.h"
#include "experiments.h"
#include "tcp_cubic.h"
#include "trace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *bin) {
    fprintf(stderr,
            "Usage:\n"
            "  %s --experiment all|1|2|3|4|5|6     Phase 2 experiments (legacy)\n"
            "  %s trace [options]                  Phase 3 PELT trace engine\n"
            "\n"
            "trace options (defaults in brackets):\n"
            "  --workload step|periodic|bursty|phase [periodic]\n"
            "  --tasks N [1]          --seed S [1]          --seeds K [1] (runs seeds S..S+K-1)\n"
            "  --duration-ms D [1000] --period-ms P [16]    --duty U [0.25]   --duty-hi U [0.8]\n"
            "  --jitter J [0]         --start-ms T [100]    --stop-ms T [600]\n"
            "  --burst-rate-hz R [20] --burst-mean-ms M [5]\n"
            "  --halflife 32|16|8 [32] --hz HZ [250]        --freq-scale F [1024] --cpu-scale C [1024]\n"
            "  --sample-us US [1048.576, one PELT period]   --csv FILE (per-sample trace; seed S only)\n",
            bin, bin);
}

static int run_legacy(const char *exp) {
    init_luts();
    bool all = strcmp(exp, "all") == 0;
    int violations = 0;
    if (all || strcmp(exp, "1") == 0) print_experiment_1();
    if (all || strcmp(exp, "2") == 0) print_experiment_2();
    if (all || strcmp(exp, "3") == 0) print_experiment_3();
    if (all || strcmp(exp, "4") == 0) violations = print_experiment_4();
    if (all || strcmp(exp, "5") == 0) print_experiment_5();
    if (all || strcmp(exp, "6") == 0) print_experiment_6();
    if (violations > 0) {
        fprintf(stderr, "Experiment 4: %d analytic bound violation(s)\n", violations);
        return 2;
    }
    return 0;
}

/* Two-sided 95% Student t critical values for df = 1..30; normal beyond. */
static double t95(int df) {
    static const double t[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                               2.201,  2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
                               2.080,  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
    return df >= 1 && df <= 30 ? t[df - 1] : 1.960;
}

typedef struct {
    double sum, sumsq;
    int n;
} Stat;

static void stat_add(Stat *s, double x) {
    s->sum += x;
    s->sumsq += x * x;
    s->n++;
}

static void stat_print(const char *name, const Stat *s) {
    double mean = s->sum / s->n;
    double half = 0.0;
    if (s->n > 1) {
        double var = (s->sumsq - s->n * mean * mean) / (s->n - 1);
        half = t95(s->n - 1) * sqrt(var > 0 ? var : 0) / sqrt((double)s->n);
    }
    printf("| %-15s | %12.3f | %10.3f |\n", name, mean, half);
}

static int run_trace_cli(int argc, char **argv) {
    TraceConfig cfg;
    trace_defaults(&cfg);
    int seeds = 1;
    unsigned int halflife = 32;
    for (int i = 2; i < argc; i++) {
        const char *k = argv[i];
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", k);
            return 1;
        }
        const char *v = argv[++i];
        if (strcmp(k, "--workload") == 0) {
            if (!workload_parse_kind(v, &cfg.wl.kind)) {
                fprintf(stderr, "unknown workload %s\n", v);
                return 1;
            }
        } else if (strcmp(k, "--tasks") == 0) cfg.wl.ntasks = atoi(v);
        else if (strcmp(k, "--seed") == 0) cfg.wl.seed = strtoull(v, NULL, 10);
        else if (strcmp(k, "--seeds") == 0) seeds = atoi(v);
        else if (strcmp(k, "--duration-ms") == 0) cfg.wl.duration_ms = atof(v);
        else if (strcmp(k, "--period-ms") == 0) cfg.wl.period_ms = atof(v);
        else if (strcmp(k, "--duty") == 0) cfg.wl.duty = atof(v);
        else if (strcmp(k, "--duty-hi") == 0) cfg.wl.duty_hi = atof(v);
        else if (strcmp(k, "--jitter") == 0) cfg.wl.jitter = atof(v);
        else if (strcmp(k, "--start-ms") == 0) cfg.wl.start_ms = atof(v);
        else if (strcmp(k, "--stop-ms") == 0) cfg.wl.stop_ms = atof(v);
        else if (strcmp(k, "--burst-rate-hz") == 0) cfg.wl.burst_rate_hz = atof(v);
        else if (strcmp(k, "--burst-mean-ms") == 0) cfg.wl.burst_mean_ms = atof(v);
        else if (strcmp(k, "--halflife") == 0) halflife = (unsigned int)atoi(v);
        else if (strcmp(k, "--hz") == 0) cfg.hz = (unsigned int)atoi(v);
        else if (strcmp(k, "--freq-scale") == 0) cfg.freq_scale = strtoul(v, NULL, 10);
        else if (strcmp(k, "--cpu-scale") == 0) cfg.cpu_scale = strtoul(v, NULL, 10);
        else if (strcmp(k, "--sample-us") == 0) cfg.sample_ns = (uint64_t)llround(atof(v) * 1e3);
        else if (strcmp(k, "--csv") == 0) cfg.csv_path = v;
        else {
            fprintf(stderr, "unknown option %s\n", k);
            return 1;
        }
    }
    if (halflife == 32) cfg.pelt_shift = 0;
    else if (halflife == 16) cfg.pelt_shift = 1;
    else if (halflife == 8) cfg.pelt_shift = 2;
    else {
        fprintf(stderr, "--halflife must be 32, 16 or 8 (PELT periods)\n");
        return 1;
    }
    if (seeds < 1 || cfg.hz == 0 || cfg.sample_ns == 0 || cfg.freq_scale == 0 || cfg.freq_scale > 1024 ||
        cfg.cpu_scale == 0 || cfg.cpu_scale > 1024) {
        fprintf(stderr, "invalid --seeds, --hz, --sample-us, --freq-scale or --cpu-scale\n");
        return 1;
    }

    printf("=== Trace: %s, %d task(s), half-life %u periods, HZ %u, freq %lu/1024, cpu %lu/1024 ===\n",
           workload_name(cfg.wl.kind), cfg.wl.ntasks, halflife, cfg.hz, cfg.freq_scale, cfg.cpu_scale);
    printf("| Seed | Busy%% | Mean CPU util | Max CPU util | Ramp80 ms | Ramp95 ms | PELT updates | Switches |\n");
    printf("|------|-------|---------------|--------------|-----------|-----------|--------------|----------|\n");
    Stat busy = {0}, mean_util = {0}, max_util = {0};
    uint64_t first_seed = cfg.wl.seed;
    const char *csv = cfg.csv_path;
    for (int s = 0; s < seeds; s++) {
        cfg.wl.seed = first_seed + (uint64_t)s;
        cfg.csv_path = s == 0 ? csv : NULL;
        TraceSummary r;
        if (run_trace(&cfg, &r) != 0) return 1;
        printf("| %4llu | %5.1f | %13.1f | %12lu | %9.1f | %9.1f | %12llu | %8llu |\n",
               (unsigned long long)cfg.wl.seed, r.busy_pct, r.mean_cpu_util, r.max_cpu_util, r.ramp80_ms,
               r.ramp95_ms, (unsigned long long)r.pelt_updates, (unsigned long long)r.switches);
        stat_add(&busy, r.busy_pct);
        stat_add(&mean_util, r.mean_cpu_util);
        stat_add(&max_util, (double)r.max_cpu_util);
    }
    if (seeds > 1) {
        printf("\n| Metric          |         Mean | 95%% CI +- |\n");
        printf("|-----------------|--------------|------------|\n");
        stat_print("Busy%", &busy);
        stat_print("Mean CPU util", &mean_util);
        stat_print("Max CPU util", &max_util);
    }
    printf("\n*Util in [0, 1024]. Ramp = time after step start until CPU util_avg reaches 80%%/95%% (-1 = never).\n");
    if (csv) printf("*Per-sample trace for seed %llu written to %s\n", (unsigned long long)first_seed, csv);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "trace") == 0) return run_trace_cli(argc, argv);
    const char *exp = "all";
    if (argc == 3 && strcmp(argv[1], "--experiment") == 0) exp = argv[2];
    else if (argc != 1) {
        usage(argv[0]);
        return 1;
    }
    return run_legacy(exp);
}
