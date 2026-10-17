#include "tcp_cubic.h"

#include "approx.h"
#include "bench.h"

#include <math.h>
#include <stdio.h>

#define CUBIC_INPUTS 1024

static double cubic_inputs[CUBIC_INPUTS];

CubicResult run_cubic_experiment(void) {
    CubicResult out = {0};
    double sum_ce = 0.0, sum_nr = 0.0;
    /* Evenly cover the table domain [CUBIC_A_BASE, CUBIC_A_BASE + 64*64). */
    for (int i = 0; i < CUBIC_INPUTS; i++) {
        cubic_inputs[i] = (double)CUBIC_A_BASE + (i + 0.5) * (64.0 * CUBIC_LUT64_SIZE / CUBIC_INPUTS);
    }
    for (int i = 0; i < CUBIC_INPUTS; i++) {
        double a = cubic_inputs[i];
        double x_true = cbrt(a);
        double x_e = cubic_exact(a);
        double x_a = cubic_approx(a);
        double ce = fabs(x_e - x_a) / x_e * 100.0;
        double nr = fabs(x_e - x_true) / x_true * 100.0;
        sum_ce += ce;
        sum_nr += nr;
        if (ce > out.max_cube_error_pct) out.max_cube_error_pct = ce;
        if (nr > out.max_nr_error_pct) out.max_nr_error_pct = nr;
    }
    out.mean_cube_error_pct = sum_ce / CUBIC_INPUTS;
    out.mean_nr_error_pct = sum_nr / CUBIC_INPUTS;

    out.baseline_ns = bench_ns_per_op(bench_identity, cubic_inputs, CUBIC_INPUTS, BENCH_REPS, BENCH_TRIALS);
    out.exact_ns = bench_ns_per_op(cubic_exact, cubic_inputs, CUBIC_INPUTS, BENCH_REPS, BENCH_TRIALS);
    out.approx_ns = bench_ns_per_op(cubic_approx, cubic_inputs, CUBIC_INPUTS, BENCH_REPS, BENCH_TRIALS);
    double net_e = out.exact_ns - out.baseline_ns;
    double net_a = out.approx_ns - out.baseline_ns;
    out.speedup = net_a > 0.05 ? net_e / net_a : 0.0;
    return out;
}

void print_experiment_5(void) {
    printf("=== Experiment 5: TCP CUBIC Approximation ===\n");
    CubicResult r = run_cubic_experiment();
    printf("| Metric | LUT+NR | LUT-only | Notes |\n");
    printf("|--------|--------|----------|-------|\n");
    printf("| ns/op | %.2f | %.2f | measured; call-only baseline %.2f ns |\n", r.exact_ns, r.approx_ns, r.baseline_ns);
    printf("| Net ns/op | %.2f | %.2f | baseline subtracted |\n", r.exact_ns - r.baseline_ns, r.approx_ns - r.baseline_ns);
    if (r.speedup > 0.0) printf("| Speedup (net) | - | %.2fx | this machine |\n", r.speedup);
    else printf("| Speedup (net) | - | n/a | LUT-only below timer resolution |\n");
    printf("| Mean error vs cbrt()%% | %.6f | - | LUT+NR is itself approximate |\n", r.mean_nr_error_pct);
    printf("| Max error vs cbrt()%% | %.6f | - | |\n", r.max_nr_error_pct);
    printf("| Mean error vs LUT+NR%% | - | %.4f | cost of dropping the NR step |\n", r.mean_cube_error_pct);
    printf("| Max error vs LUT+NR%% | - | %.4f | |\n", r.max_cube_error_pct);
    printf("\n*%d inputs evenly covering the table domain [%d, %d). Median of %d trials.\n",
           CUBIC_INPUTS, CUBIC_A_BASE, CUBIC_A_BASE + 64 * CUBIC_LUT64_SIZE, BENCH_TRIALS);
    printf("*Not the Linux cubic_root(): that uses a 64-entry table over a bit-scaled u64 input plus one NR step.\n");
    printf("*Linux calls cubic_root() about once per congestion epoch, not per ACK, so per-call savings barely reach throughput.\n\n");
}
