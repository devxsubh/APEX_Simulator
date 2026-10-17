#ifndef SIM_H
#define SIM_H

#include "apex_types.h"

void init_equal(Task *tasks, int n);
void init_mixed(Task *tasks, int *n);
void init_20_mixed(Task *tasks, int *n);
void sim_set_load_coupling(double coupling);
double sim_get_load_coupling(void);
/* chosen_trace may be NULL; otherwise it receives `ticks` entries (task index or -1). */
Result run_simulation(Task *tasks, int n, int ticks, ApproxMode mode, bool phased,
                      ControllerPhaseResult *phase_out, int *chosen_trace);

#endif
