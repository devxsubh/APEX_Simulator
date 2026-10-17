#include "experiments.h"

#include "approx.h"
#include "bench.h"
#include "sim.h"

#include <math.h>
#include <stdio.h>

#define EXP2_TICKS 2000
#define NICE0_SLICE (32.0 * 1024.0 / 1024.0) /* vruntime advance of one nice-0 tick */

void print_experiment_1(void) {
    printf("=== Experiment 1: Equal-Weight Fairness ===\n");
    printf("| Mode    | N=10 vdev%% | N=50 vdev%% | N=100 vdev%% | Max vruntime gap | Starvation |\n");
    printf("|---------|-----------|-----------|------------|------------------|------------|\n");
    ApproxMode modes[] = {MODE_EXACT, MODE_LINEAR, MODE_LUT256, MODE_POLY2, MODE_ADAPTIVE};
    for (int m = 0; m < 5; m++) {
        int ns[] = {10, 50, 100};
        double vd[3] = {0};
        double gap = 0.0;
        int starvation = 0;
        for (int i = 0; i < 3; i++) {
            Task tasks[MAX_TASKS];
            init_equal(tasks, ns[i]);
            Result r = run_simulation(tasks, ns[i], 1000, modes[m], false, NULL, NULL);
            vd[i] = r.max_vruntime_dev_pct;
            if (r.max_vruntime_gap > gap) gap = r.max_vruntime_gap;
            starvation += r.starvation_count;
        }
        printf("| %-8s| %9.4f | %9.4f | %10.4f | %16.4f | %10d |\n", mode_name(modes[m]), vd[0], vd[1], vd[2], gap, starvation);
    }
    printf("\n*Max vruntime gap = largest max-pairwise vruntime gap seen at any tick (vruntime units; one nice-0 tick = %.0f).\n\n", NICE0_SLICE);
}

typedef struct {
    double share_pct;
    int max_wait;
    int starvation;
    double diverged_pct;
    int first_divergence;
    double max_gap_slices;
} PairedStats;

static PairedStats run_paired(ApproxMode mode, const int *trace_exact, const Task *exact_tasks) {
    PairedStats s = {0};
    Task tasks[MAX_TASKS];
    int trace[EXP2_TICKS];
    int n = 0;
    init_mixed(tasks, &n);
    Result r = run_simulation(tasks, n, EXP2_TICKS, mode, false, NULL, trace);
    int total = 0;
    for (int i = 0; i < n; i++) total += tasks[i].exec_ticks;
    s.share_pct = total > 0 ? (double)tasks[0].exec_ticks * 100.0 / total : 0.0;
    s.max_wait = r.max_wait_ticks;
    s.starvation = r.starvation_count;
    s.first_divergence = -1;
    int differ = 0;
    for (int t = 0; t < EXP2_TICKS; t++) {
        if (trace[t] != trace_exact[t]) {
            differ++;
            if (s.first_divergence < 0) s.first_divergence = t;
        }
    }
    s.diverged_pct = 100.0 * differ / EXP2_TICKS;
    for (int i = 0; i < n; i++) {
        double g = fabs(tasks[i].vruntime - exact_tasks[i].vruntime) / NICE0_SLICE;
        if (g > s.max_gap_slices) s.max_gap_slices = g;
    }
    return s;
}

void print_experiment_2(void) {
    printf("=== Experiment 2: Mixed-Weight Starvation Test ===\n");
    ApproxMode modes[] = {MODE_EXACT, MODE_LINEAR, MODE_LUT256, MODE_POLY2, MODE_ADAPTIVE};
    Task exact_tasks[MAX_TASKS];
    int trace_exact[EXP2_TICKS];
    int n = 0;

    init_mixed(exact_tasks, &n);
    run_simulation(exact_tasks, n, EXP2_TICKS, MODE_EXACT, false, NULL, trace_exact);

    printf("| Mode    | HiPrio Share%% | MaxWait | Starvation | Decisions differ%% | First divergence tick | Max vruntime gap vs EXACT (slices) |\n");
    printf("|---------|--------------|---------|------------|------------------|-----------------------|------------------------------------|\n");
    for (int m = 0; m < 5; m++) {
        PairedStats s = run_paired(modes[m], trace_exact, exact_tasks);
        printf("| %-8s| %12.2f | %7d | %10d | %16.2f | %21d | %34.4f |\n",
               mode_name(modes[m]), s.share_pct, s.max_wait, s.starvation,
               s.diverged_pct, s.first_divergence, s.max_gap_slices);
    }
    double w_hi = weight_from_nice(-10), w_lo = weight_from_nice(0);
    printf("\n*Ideal HiPrio share from weights alone: %.2f%%.\n", 100.0 * w_hi / (w_hi + 10.0 * w_lo));
    printf("*Each mode runs the same workload as EXACT; 'Decisions differ' = %% of ticks that picked a different task.\n");
    printf("*Approximation reaches scheduling only via the load-coupling assumption (coupling = %.2f); real CFS has no such path.\n\n",
           sim_get_load_coupling());

    /* Sensitivity: how much do the conclusions depend on the made-up coupling? */
    double couplings[] = {0.02, 0.2, 1.0};
    double saved = sim_get_load_coupling();
    printf("=== Experiment 2b: Load-Coupling Sensitivity ===\n");
    printf("| Coupling | LINEAR differ%% | LUT256 differ%% | POLY2 differ%% | ADAPTIVE differ%% |\n");
    printf("|----------|---------------|---------------|--------------|-----------------|\n");
    for (int k = 0; k < 3; k++) {
        sim_set_load_coupling(couplings[k]);
        init_mixed(exact_tasks, &n);
        run_simulation(exact_tasks, n, EXP2_TICKS, MODE_EXACT, false, NULL, trace_exact);
        double d[4];
        for (int m = 1; m < 5; m++) d[m - 1] = run_paired(modes[m], trace_exact, exact_tasks).diverged_pct;
        printf("| %8.2f | %13.2f | %13.2f | %12.2f | %15.2f |\n", couplings[k], d[0], d[1], d[2], d[3]);
    }
    sim_set_load_coupling(saved);
    printf("\n");
}

void print_experiment_3(void) {
    printf("=== Experiment 3: Safety Controller Dynamics ===\n");
    Task tasks[MAX_TASKS];
    for (int i = 0; i < 60; i++) {
        tasks[i] = (Task){0};
        tasks[i].tid = i;
        tasks[i].nice = 0;
        tasks[i].weight = weight_from_nice(0);
        tasks[i].effective_weight = (double)tasks[i].weight;
        tasks[i].runnable = true;
        tasks[i].load_current = 1.0;
    }
    ControllerPhaseResult p = {0};
    Result r = run_simulation(tasks, 60, 2000, MODE_ADAPTIVE, true, &p, NULL);
    printf("| Phase | Tasks | SAFE ticks | CAUTION ticks | STRICT ticks | Mean normalized gap%% |\n");
    printf("|-------|-------|------------|---------------|-------------|----------------------|\n");
    printf("| 1     | 5     | %10d | %13d | %11d | %19.2f%% |\n", p.safe_phase[0], p.caution_phase[0], p.strict_phase[0], p.fairness_phase[0]);
    printf("| 2     | 55    | %10d | %13d | %11d | %19.2f%% |\n", p.safe_phase[1], p.caution_phase[1], p.strict_phase[1], p.fairness_phase[1]);
    printf("| 3     | 15    | %10d | %13d | %11d | %19.2f%% |\n", p.safe_phase[2], p.caution_phase[2], p.strict_phase[2], p.fairness_phase[2]);
    printf("\n*Normalized gap = max pairwise vruntime gap / mean vruntime of runnable tasks, averaged over the phase.\n");
    printf("*Known limitation: controller thresholds (0.05/0.10 * w_min) are compared against this percentage;\n");
    printf(" the units do not match, and the signal reflects contention rather than approximation error.\n");

    double interval_ms = APAF_MONITOR_INTERVAL * TICK_MS;
    printf("\nController samples every %d ticks (%.0f ms at %.0f ms/tick).\n", APAF_MONITOR_INTERVAL, interval_ms, TICK_MS);
    if (r.reaction_ticks < 0) {
        printf("No state change after the spike at tick 500.\n");
    } else {
        printf("Reaction time after spike at tick 500: %d ticks (%.0f ms)\n", r.reaction_ticks, r.reaction_ticks * TICK_MS);
        printf("  %s\n", r.reaction_ticks <= APAF_MONITOR_INTERVAL ? "Reacted within one monitor interval"
                                                                   : "Took longer than one monitor interval");
    }
    printf("\nController state transitions:\n");
    printf("  SAFE->CAUTION:  %d times\n", r.safe_to_caution);
    printf("  CAUTION->STRICT: %d times\n", r.caution_to_strict);
    printf("  STRICT->CAUTION: %d times\n", r.strict_to_caution);
    printf("  CAUTION->SAFE:  %d times\n\n", r.caution_to_safe);
}

int print_experiment_4(void) {
    printf("=== Experiment 4: Error Bound Verification (Theorem 2) ===\n");
    printf("| Workload | Mode    | Max window ms | Step bound | Step observed | Load bound | Load observed | Observed/Bound | Verified |\n");
    printf("|----------|---------|---------------|------------|---------------|------------|---------------|----------------|----------|\n");
    ApproxMode modes[] = {MODE_EXACT, MODE_LINEAR, MODE_LUT256, MODE_POLY2, MODE_ADAPTIVE};
    const char *workloads[] = {"mixed20", "equal100"};
    int violations = 0;
    for (int w = 0; w < 2; w++) {
        for (int m = 0; m < 5; m++) {
            Task tasks[MAX_TASKS];
            int n = 100;
            if (w == 0) init_20_mixed(tasks, &n);
            else init_equal(tasks, n);
            Result r = run_simulation(tasks, n, 1000, modes[m], false, NULL, NULL);

            double step_bound = step_error_bound(modes[m], r.max_window_s);
            double d_sup = decay_sup(modes[m], TICK_SECONDS, r.max_window_s);
            double load_bound = d_sup < 1.0 ? step_bound / (1.0 - d_sup) : INFINITY;
            int ok = r.max_step_abs_err <= step_bound + 1e-12 && r.max_load_abs_err <= load_bound + 1e-12;
            if (!ok) violations++;

            char ratio[32];
            if (load_bound > 0.0) snprintf(ratio, sizeof ratio, "%.3f", r.max_load_abs_err / load_bound);
            else snprintf(ratio, sizeof ratio, "-");
            printf("| %-8s | %-8s| %13.1f | %10.3e | %13.3e | %10.3e | %13.3e | %14s | %-8s |\n",
                   workloads[w], mode_name(modes[m]), r.max_window_s * 1000.0,
                   step_bound, r.max_step_abs_err, load_bound, r.max_load_abs_err,
                   ratio, ok ? "YES" : "NO");
        }
    }
    printf("\n*Bounds are analytic and fixed before the run (see step_error_bound in src/approx.c); errors are absolute.\n");
    printf("*Theorem 2: with L' = d L + (1-d) c and L~' = d~ L~ + (1-d~) c on the same inputs c in [0,1],\n");
    printf(" |d~ - d| <= delta and d~ <= D < 1 give |L~ - L| <= delta / (1 - D) at every step.\n");
    printf(" Proof: e' = d~ e + (d~ - d)(L - c) and |L - c| <= 1, so |e'| <= D|e| + delta; unroll from e_0 = 0.\n");
    printf("*Load observed comes from a shadow exact tracker fed identical inputs inside the simulation.\n\n");
    return violations;
}

static double decay_inputs[1000];

void print_experiment_6(void) {
    printf("=== Experiment 6: Decay Cost Microbenchmark ===\n");
    for (int i = 0; i < 1000; i++) {
        decay_inputs[i] = TICK_SECONDS * (1.0 + 0.05 * (double)(i % 100));
    }
    struct { const char *name; double (*fn)(double); } variants[] = {
        {"EXACT", exact_decay}, {"LINEAR", linear_decay}, {"LUT256", lut256_decay}, {"POLY2", poly2_decay},
    };
    double base = bench_ns_per_op(bench_identity, decay_inputs, 1000, BENCH_REPS, BENCH_TRIALS);
    double exact_net = 0.0;
    printf("| Variant | ns/op | Net ns/op | Speedup vs EXACT | Max abs error |\n");
    printf("|---------|-------|-----------|------------------|---------------|\n");
    for (int v = 0; v < 4; v++) {
        double ns = bench_ns_per_op(variants[v].fn, decay_inputs, 1000, BENCH_REPS, BENCH_TRIALS);
        double net = ns - base;
        if (v == 0) exact_net = net;
        double err = 0.0;
        for (int i = 0; i < 100; i++) {
            double e = fabs(variants[v].fn(decay_inputs[i]) - exact_decay(decay_inputs[i]));
            if (e > err) err = e;
        }
        char speed[32];
        if (net > 0.05) snprintf(speed, sizeof speed, "%.2fx", exact_net / net);
        else snprintf(speed, sizeof speed, "n/a");
        printf("| %-7s | %5.2f | %9.2f | %16s | %13.3e |\n", variants[v].name, ns, net, speed, err);
    }
    printf("\n*Median of %d trials x %d reps x 1000 inputs (windows %.0f-%.0f ms). Call-only baseline: %.2f ns/op, subtracted for Net.\n",
           BENCH_TRIALS, BENCH_REPS, TICK_MS, TICK_MS * (1.0 + 0.05 * 99.0), base);
    printf("*Timings are machine-dependent. 'n/a' = net cost below timer resolution.\n");
    printf("*Floating point, not kernel fixed point: Linux PELT already uses an integer table (runnable_avg_yN_inv).\n\n");
}
