#ifndef WORKLOAD_H
#define WORKLOAD_H

#include "rng.h"

#include <stdint.h>

#define WL_MAX_TASKS 16

typedef enum {
    WL_STEP = 0,  /* idle, then always busy in [start, stop), then idle */
    WL_PERIODIC,  /* every period, release duty * period of work */
    WL_BURSTY,    /* Poisson burst arrivals with exponential burst sizes */
    WL_PHASE      /* periodic at duty, duty_hi, duty for equal thirds of the run */
} WorkloadKind;

/* Work is measured in ns of execution at full capacity and full frequency,
 * so a task running at half frequency takes twice as long to finish it. */
typedef struct {
    WorkloadKind kind;
    int ntasks;
    uint64_t seed;
    double duration_ms;
    double period_ms;      /* PERIODIC, PHASE */
    double duty;           /* PERIODIC, PHASE (light phases) */
    double duty_hi;        /* PHASE heavy phase */
    double jitter;         /* PERIODIC, PHASE: release time jitter, fraction of period (uniform +-) */
    double start_ms;       /* STEP */
    double stop_ms;        /* STEP */
    double burst_rate_hz;  /* BURSTY */
    double burst_mean_ms;  /* BURSTY */
} WorkloadSpec;

typedef struct {
    double next_release_ns;
    double pending_ns;     /* remaining work, ns at full capacity */
    uint64_t released;     /* number of releases so far */
} TaskGen;

void workload_defaults(WorkloadSpec *spec);
const char *workload_name(WorkloadKind kind);
int workload_parse_kind(const char *name, WorkloadKind *kind);
void workload_init(const WorkloadSpec *spec, TaskGen *gens, Rng *rng);
/* Release all work due at or before now_ns. */
void workload_release(const WorkloadSpec *spec, TaskGen *g, double now_ns, Rng *rng);
/* Time of the next event that changes a task's work (release or step edge), or INFINITY. */
double workload_next_event(const WorkloadSpec *spec, const TaskGen *g, double now_ns);

#endif
