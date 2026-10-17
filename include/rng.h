#ifndef RNG_H
#define RNG_H

#include <stdint.h>

/* SplitMix64: tiny, fast, and identical on every platform, so a seed
 * reproduces the same workload everywhere (unlike rand()). */
typedef struct {
    uint64_t state;
} Rng;

void rng_seed(Rng *r, uint64_t seed);
uint64_t rng_u64(Rng *r);
double rng_uniform(Rng *r);              /* [0, 1) */
double rng_exp(Rng *r, double mean);     /* exponential with the given mean */

#endif
