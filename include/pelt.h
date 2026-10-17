/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Userspace port of Linux per-entity load tracking (kernel/sched/pelt.c).
 * Integer arithmetic and constants follow the kernel so results are
 * bit-identical for the same inputs; see src/pelt.c for provenance.
 */
#ifndef PELT_H
#define PELT_H

#include <stdbool.h>
#include <stdint.h>

#define PELT_LOAD_AVG_PERIOD 32
#define PELT_LOAD_AVG_MAX 47742
#define PELT_MIN_DIVIDER (PELT_LOAD_AVG_MAX - 1024)
#define SCHED_CAPACITY_SHIFT 10
#define SCHED_CAPACITY_SCALE (1UL << SCHED_CAPACITY_SHIFT)
/* One PELT period is 1024 "us" of 1024 ns each: delta_ns >> 10, then / 1024. */
#define PELT_PERIOD_NS (1024ULL * 1024ULL)

extern const uint32_t pelt_runnable_avg_yN_inv[PELT_LOAD_AVG_PERIOD];

/* Mirrors struct sched_avg (util_est is handled elsewhere). */
typedef struct {
    uint64_t last_update_time;
    uint64_t load_sum;
    uint64_t runnable_sum;
    uint32_t util_sum;
    uint32_t period_contrib;
    unsigned long load_avg;
    unsigned long runnable_avg;
    unsigned long util_avg;
} PeltAvg;

uint64_t pelt_decay_load(uint64_t val, uint64_t n);
uint32_t pelt_accumulate_segments(uint64_t periods, uint32_t d1, uint32_t d3);
uint32_t pelt_accumulate_sum(uint64_t delta, PeltAvg *sa, unsigned long load, unsigned long runnable, int running);
int pelt_update_load_sum(uint64_t now, PeltAvg *sa, unsigned long load, unsigned long runnable, int running);
void pelt_update_load_avg(PeltAvg *sa, unsigned long load);
/* Convenience: both steps, as __update_load_avg_se / _cfs_rq do. Returns 1 if a period boundary was crossed. */
int pelt_update(uint64_t now, PeltAvg *sa, unsigned long sum_load, unsigned long runnable, int running,
                unsigned long avg_load);

/* Per-CPU PELT clock (rq->clock_pelt). Time while busy is scaled by CPU
 * capacity and current frequency; when the CPU goes idle the clock syncs
 * back to the task clock. shift = log2 of the PELT multiplier (0, 1, 2 for
 * 32/16/8-period half-lives), applied as an extra clock between the task
 * clock and the PELT clock. */
typedef struct {
    uint64_t clock_task;      /* real time, ns */
    uint64_t clock_task_mult; /* clock_task advanced with deltas << shift */
    uint64_t clock_pelt;
    uint64_t lost_idle_time;
    unsigned int shift;
} PeltClock;

void pelt_clock_init(PeltClock *c, unsigned int shift);
/* Advance by delta_ns of real time spent busy (or idle) at the given capacity/frequency scale. */
void pelt_clock_advance(PeltClock *c, uint64_t delta_ns, bool busy, unsigned long cpu_scale,
                        unsigned long freq_scale, uint32_t cpu_util_sum);
uint64_t pelt_clock_now(const PeltClock *c);

#endif
