#ifndef APEX_TYPES_H
#define APEX_TYPES_H

#include <stdbool.h>

#define MAX_TASKS 160
#define APAF_MONITOR_INTERVAL 8
#define SCHED_PERIOD 20
#define DECAY_LAMBDA 0.693
#define TICK_SECONDS 0.032
#define TICK_MS (TICK_SECONDS * 1000.0)
#define LUT256_SIZE 256
#define CUBIC_LUT64_SIZE 64
#define CUBIC_A_BASE 16384

/* Modeling assumption, not Linux behaviour: effective weight is scaled by
 * (1 + coupling * (load_avg - 0.5)). This is the only path through which
 * decay approximation reaches scheduling decisions. Real CFS does not feed
 * PELT back into vruntime; see README "Known limitations". */
#define DEFAULT_LOAD_COUPLING 0.02

typedef enum {
    MODE_EXACT = 0,
    MODE_LINEAR,
    MODE_LUT256,
    MODE_POLY2,
    MODE_ADAPTIVE
} ApproxMode;

typedef enum {
    STATE_SAFE = 0,
    STATE_CAUTION,
    STATE_STRICT
} SafetyState;

typedef struct {
    int tid;
    int nice;
    int weight;
    bool runnable;
    double vruntime;
    double load_avg;
    double load_avg_exact; /* shadow tracker: same inputs, exact decay */
    double load_current;
    double effective_weight;
    int exec_ticks;
    int wait_ticks_since_run;
    int max_wait_ticks;
    int starvation_events;
} Task;

typedef struct {
    ApproxMode requested_mode;
    SafetyState state;
    int safe_ticks;
    int caution_ticks;
    int strict_ticks;
    int reaction_ticks;
    bool reaction_seen;
    int switch_tick;
    int safe_to_caution;
    int caution_to_strict;
    int strict_to_caution;
    int caution_to_safe;
} Controller;

typedef struct {
    double fairness_violation;  /* value at the last tick */
    double max_vruntime_gap;    /* max over ticks of max pairwise vruntime gap */
    double max_vruntime_dev_pct;
    int starvation_count;
    int max_wait_ticks;
    double avg_wait_ticks;
    /* Error telemetry (Experiment 4). All values are absolute, not relative. */
    double max_step_abs_err;    /* max |d_approx(t) - d_exact(t)| over decay calls */
    double max_approx_decay;    /* max d_approx(t) used */
    double max_window_s;        /* largest decay window t used */
    double max_load_abs_err;    /* max |load_avg - load_avg_exact| over tasks, ticks */
    /* Controller telemetry (Experiment 3). */
    int reaction_ticks;         /* ticks from spike to first state change, -1 if none */
    int safe_to_caution;
    int caution_to_strict;
    int strict_to_caution;
    int caution_to_safe;
} Result;

typedef struct {
    int safe_phase[3];
    int caution_phase[3];
    int strict_phase[3];
    double fairness_phase[3];
    double fairness_sum[3];
    int fairness_count[3];
} ControllerPhaseResult;

typedef struct {
    double mean_cube_error_pct;  /* LUT-only vs LUT+NR */
    double max_cube_error_pct;
    double mean_nr_error_pct;    /* LUT+NR vs libm cbrt() */
    double max_nr_error_pct;
    double exact_ns;
    double approx_ns;
    double baseline_ns;
    double speedup;
} CubicResult;

#endif
