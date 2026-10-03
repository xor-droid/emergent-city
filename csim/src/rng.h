/* rng.h — PCG32 deterministic PRNG (header-only).
 *
 * C's rand() won't reproduce Python's cities, so the port ships its own PRNG.
 * PCG32 is small, fast and well-distributed. Seed it explicitly for determinism.
 */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>
#include <math.h>

typedef struct { uint64_t state, inc; } Rng;

static inline uint32_t rng_u32(Rng *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static inline void rng_seed(Rng *r, uint64_t seed, uint64_t seq) {
    r->state = 0u;
    r->inc = (seq << 1u) | 1u;
    rng_u32(r);
    r->state += seed;
    rng_u32(r);
}

/* uniform double in [0,1) */
static inline double rng_double(Rng *r) {
    return rng_u32(r) / 4294967296.0;
}

static inline double rng_range(Rng *r, double lo, double hi) {
    return lo + (hi - lo) * rng_double(r);
}

/* integer in [0,n) */
static inline int rng_int(Rng *r, int n) {
    return (int)(rng_double(r) * n);
}

/* integer in [lo,hi] inclusive */
static inline int rng_int_incl(Rng *r, int lo, int hi) {
    return lo + rng_int(r, hi - lo + 1);
}

/* gaussian via Box-Muller */
static inline double rng_gauss(Rng *r, double mean, double sd) {
    double u1 = rng_double(r);
    if (u1 < 1e-12) u1 = 1e-12;
    double u2 = rng_double(r);
    double z = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
    return mean + sd * z;
}

#endif /* RNG_H */
