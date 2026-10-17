#ifndef APPROX_H
#define APPROX_H

#include "apex_types.h"

extern double decay_lut256[LUT256_SIZE];
extern double cubic_lut64[CUBIC_LUT64_SIZE];

void init_luts(void);
int weight_from_nice(int nice);
const char *mode_name(ApproxMode mode);
double exact_decay(double seconds);
double linear_decay(double seconds);
double lut256_decay(double seconds);
double poly2_decay(double seconds);
double bench_identity(double x);
double mode_decay(ApproxMode mode, SafetyState state, double seconds);
double step_error_bound(ApproxMode mode, double t_max);
double decay_sup(ApproxMode mode, double t_min, double t_max);
double cubic_exact(double a);
double cubic_approx(double a);
void update_controller(Controller *c, double fairness_violation, double w_min, int tick);

#endif
