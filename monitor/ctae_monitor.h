#ifndef CTAE_MONITOR_H
#define CTAE_MONITOR_H

#include <linux/types.h>

struct ctae_cpu_stats {
    u64 llc_references;
    u64 llc_misses;
    u32 miss_rate_per_thousand;
};

int ctae_get_cpu_stats(int cpu, struct ctae_cpu_stats *stats);

#endif // CTAE_MONITOR_H