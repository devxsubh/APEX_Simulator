#include "bench.h"

#include <stdlib.h>
#include <time.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static volatile double bench_sink;

double bench_ns_per_op(double (*fn)(double), const double *inputs, int n_inputs, int reps, int trials) {
    double (*volatile f)(double) = fn;
    double samples[64];
    if (trials > 64) trials = 64;
    for (int t = -1; t < trials; t++) {
        double acc = 0.0;
        double start = now_ns();
        for (int r = 0; r < reps; r++) {
            for (int i = 0; i < n_inputs; i++) acc += f(inputs[i]);
        }
        double elapsed = now_ns() - start;
        bench_sink += acc;
        if (t >= 0) samples[t] = elapsed / ((double)reps * (double)n_inputs);
    }
    qsort(samples, (size_t)trials, sizeof(double), cmp_double);
    return samples[trials / 2];
}
