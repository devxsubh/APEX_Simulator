/* Reads "now load runnable running avg_load" lines and prints the PELT state
 * after each update, for differential testing against tests/pelt_ref.py. */
#include "pelt.h"

#include <stdio.h>

int main(void) {
    PeltAvg a = {0};
    unsigned long long now;
    unsigned long load, runnable, avg_load;
    int running;
    while (scanf("%llu %lu %lu %d %lu", &now, &load, &runnable, &running, &avg_load) == 5) {
        pelt_update(now, &a, load, runnable, running, avg_load);
        printf("%llu %llu %llu %u %u %lu %lu %lu\n", (unsigned long long)a.last_update_time,
               (unsigned long long)a.load_sum, (unsigned long long)a.runnable_sum, a.util_sum, a.period_contrib,
               a.load_avg, a.runnable_avg, a.util_avg);
    }
    return 0;
}
