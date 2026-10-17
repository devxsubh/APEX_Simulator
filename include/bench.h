#ifndef BENCH_H
#define BENCH_H

#define BENCH_TRIALS 21
#define BENCH_REPS 2000

/* Median ns per call of fn over `trials` timed trials (after one warm-up),
 * each calling fn on every input `reps` times. Calls go through a volatile
 * function pointer so every variant pays the same call overhead; measure
 * bench_identity the same way and subtract it to get net cost. */
double bench_ns_per_op(double (*fn)(double), const double *inputs, int n_inputs, int reps, int trials);

#endif
