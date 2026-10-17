#ifndef TRACE_H
#define TRACE_H

#include "workload.h"

#include <stdint.h>

typedef struct {
    WorkloadSpec wl;
    uint64_t sample_ns;        /* CSV sampling period; default one PELT period (1024 us) */
    unsigned int hz;           /* scheduler tick rate; PELT updates happen at ticks and events */
    unsigned int pelt_shift;   /* 0/1/2: PELT multiplier x1/x2/x4 */
    unsigned long cpu_scale;   /* CPU capacity, of 1024 */
    unsigned long freq_scale;  /* current frequency / max frequency, of 1024 (fixed until cpufreq lands) */
    const char *csv_path;      /* NULL: no CSV */
} TraceConfig;

typedef struct {
    double busy_pct;
    double mean_cpu_util;
    unsigned long max_cpu_util;
    double ramp80_ms;          /* first time CPU util_avg >= 80% of 1024 after step start, -1 if never */
    double ramp95_ms;
    uint64_t samples;
    uint64_t pelt_updates;
    uint64_t switches;
} TraceSummary;

void trace_defaults(TraceConfig *cfg);
int run_trace(const TraceConfig *cfg, TraceSummary *out);

#endif
