#include "approx.h"

#include <math.h>

static const int nice_to_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  2008,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15};

double decay_lut256[LUT256_SIZE];
double cubic_lut64[CUBIC_LUT64_SIZE];

/* LUT256 entries are rounded to 4 decimals in float arithmetic. Worst case:
 * 0.5e-4 from rounding, plus float error in q*10000 that can move the rounding
 * decision by at most half a float ulp at 1e4 (~4.9e-4 units -> 4.9e-8), plus
 * two float conversions (~6e-8 each). 5.1e-5 covers all of it. */
#define LUT256_QUANT_BOUND 5.1e-5

int weight_from_nice(int nice) {
    int idx = nice + 20;
    if (idx < 0) idx = 0;
    if (idx > 39) idx = 39;
    return nice_to_weight[idx];
}

void init_luts(void) {
    for (int i = 0; i < LUT256_SIZE; i++) {
        double t = (double)i * TICK_SECONDS;
        float q = (float)exp(-DECAY_LAMBDA * t);
        q = floorf(q * 10000.0f + 0.5f) / 10000.0f;
        decay_lut256[i] = (double)q;
    }
    for (int i = 0; i < CUBIC_LUT64_SIZE; i++) {
        double a = (double)(CUBIC_A_BASE + (i << 6));
        cubic_lut64[i] = cbrt(a);
    }
}

const char *mode_name(ApproxMode mode) {
    switch (mode) {
        case MODE_EXACT: return "EXACT";
        case MODE_LINEAR: return "LINEAR";
        case MODE_LUT256: return "LUT256";
        case MODE_POLY2: return "POLY2";
        case MODE_ADAPTIVE: return "ADAPTIVE";
        default: return "UNKNOWN";
    }
}

double exact_decay(double seconds) { return exp(-DECAY_LAMBDA * seconds); }

double linear_decay(double seconds) {
    double x = 1.0 - DECAY_LAMBDA * seconds;
    return x < 0.0 ? 0.0 : x;
}

double lut256_decay(double seconds) {
    int idx = (int)llround(seconds / TICK_SECONDS);
    if (idx < 0) idx = 0;
    if (idx >= LUT256_SIZE) idx = LUT256_SIZE - 1;
    return decay_lut256[idx];
}

double poly2_decay(double seconds) {
    double x = DECAY_LAMBDA * seconds;
    double p = 1.0 - x + 0.5 * x * x;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return p;
}

double bench_identity(double x) { return x; }

double mode_decay(ApproxMode mode, SafetyState state, double seconds) {
    if (mode == MODE_ADAPTIVE) {
        if (state == STATE_SAFE)    return poly2_decay(seconds);
        if (state == STATE_CAUTION) return lut256_decay(seconds);
        return exact_decay(seconds);
    }
    if (mode == MODE_LINEAR) return linear_decay(seconds);
    if (mode == MODE_LUT256) return lut256_decay(seconds);
    if (mode == MODE_POLY2)  return poly2_decay(seconds);
    return exact_decay(seconds);
}

/* Analytic sup over t in [0, t_max] of |d_mode(t) - e^{-lambda t}|.
 * LINEAR: Taylor remainder, 0 <= e^{-x} - (1 - x) <= x^2/2 for x >= 0.
 * POLY2:  alternating remainder, |e^{-x} - (1 - x + x^2/2)| <= x^3/6.
 *         Clamping to [0,1] only moves the value toward the true one.
 * LUT256: index rounding moves t by at most dt/2, so the error is at most
 *         lambda*dt/2 (Lipschitz constant of e^{-lambda t} is lambda) plus
 *         the table quantization. Beyond the table range the bound is 1.
 * ADAPTIVE: worst of the states it can be in (POLY2, LUT256, EXACT). */
double step_error_bound(ApproxMode mode, double t_max) {
    double x = DECAY_LAMBDA * t_max;
    switch (mode) {
        case MODE_LINEAR: return 0.5 * x * x;
        case MODE_POLY2:  return x * x * x / 6.0;
        case MODE_LUT256:
            if (t_max > (LUT256_SIZE - 0.5) * TICK_SECONDS) return 1.0;
            return DECAY_LAMBDA * TICK_SECONDS / 2.0 + LUT256_QUANT_BOUND;
        case MODE_ADAPTIVE: {
            double a = step_error_bound(MODE_POLY2, t_max);
            double b = step_error_bound(MODE_LUT256, t_max);
            return a > b ? a : b;
        }
        default: return 0.0;
    }
}

/* sup of d_mode(t) over t in [t_min, t_max]. Every approximation here is
 * either non-increasing in t (LINEAR, LUT256, EXACT) or convex on the
 * interval (POLY2), so the sup is attained at an endpoint. */
static double endpoint_max(double (*f)(double), double t_min, double t_max) {
    double a = f(t_min), b = f(t_max);
    return a > b ? a : b;
}

double decay_sup(ApproxMode mode, double t_min, double t_max) {
    switch (mode) {
        case MODE_LINEAR: return endpoint_max(linear_decay, t_min, t_max);
        case MODE_LUT256: return endpoint_max(lut256_decay, t_min, t_max);
        case MODE_POLY2:  return endpoint_max(poly2_decay, t_min, t_max);
        case MODE_ADAPTIVE: {
            double m = endpoint_max(poly2_decay, t_min, t_max);
            double l = endpoint_max(lut256_decay, t_min, t_max);
            double e = endpoint_max(exact_decay, t_min, t_max);
            if (l > m) m = l;
            return e > m ? e : m;
        }
        default: return endpoint_max(exact_decay, t_min, t_max);
    }
}

double cubic_exact(double a) {
    int idx = (int)((a - CUBIC_A_BASE) / 64.0);
    if (idx < 0) idx = 0;
    if (idx >= CUBIC_LUT64_SIZE) idx = CUBIC_LUT64_SIZE - 1;
    double x = cubic_lut64[idx];
    return (2.0 * x + a / (x * x)) / 3.0;
}

double cubic_approx(double a) {
    int idx = (int)((a - CUBIC_A_BASE) / 64.0);
    if (idx < 0) idx = 0;
    if (idx >= CUBIC_LUT64_SIZE) idx = CUBIC_LUT64_SIZE - 1;
    return cubic_lut64[idx];
}

void update_controller(Controller *c, double fairness_violation, double w_min, int tick) {
    if (c->requested_mode != MODE_ADAPTIVE) return;
    SafetyState prev = c->state;
    double t1 = 0.05 * w_min;
    double t2 = 0.10 * w_min;
    double t3 = 0.02 * w_min;
    double t4 = 0.04 * w_min;
    if (c->state == STATE_SAFE && fairness_violation > t1) c->state = STATE_CAUTION;
    else if (c->state == STATE_CAUTION && fairness_violation > t2) c->state = STATE_STRICT;
    else if (c->state == STATE_CAUTION && fairness_violation < t3) c->state = STATE_SAFE;
    else if (c->state == STATE_STRICT && fairness_violation < t4) c->state = STATE_CAUTION;
    if (prev == STATE_SAFE && c->state == STATE_CAUTION) c->safe_to_caution++;
    else if (prev == STATE_CAUTION && c->state == STATE_STRICT) c->caution_to_strict++;
    else if (prev == STATE_STRICT && c->state == STATE_CAUTION) c->strict_to_caution++;
    else if (prev == STATE_CAUTION && c->state == STATE_SAFE) c->caution_to_safe++;
    if (!c->reaction_seen && prev != c->state && tick >= 500) {
        c->reaction_ticks = tick - 500;
        c->reaction_seen = true;
    }
}
