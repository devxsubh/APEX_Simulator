#include "rng.h"

#include <math.h>

void rng_seed(Rng *r, uint64_t seed) { r->state = seed; }

uint64_t rng_u64(Rng *r) {
    uint64_t z = (r->state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

double rng_uniform(Rng *r) { return (double)(rng_u64(r) >> 11) * 0x1.0p-53; }

double rng_exp(Rng *r, double mean) { return -mean * log1p(-rng_uniform(r)); }
