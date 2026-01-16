/* CTAE Policy Engine Header
 * Cache-Topology-Aware Execution Engine
 *
 * Purpose: Define policy engine API and tunables
 * Architecture: x86_64
 * Kernel: Linux 5.15+
 */

#ifndef _CTAE_POLICY_H
#define _CTAE_POLICY_H

#include <linux/types.h>

/* Module metadata */
#define CTAE_POLICY_VERSION "0.1.0"

/* Policy Tunables */
/* How often the policy thread wakes up to check for contention */
#define CTAE_POLICY_INTERVAL_MS 500

/* Minimum time (ms) between migrating the SAME task again (prevent thrashing)
 */
#define CTAE_MIGRATION_COOLOFF_MS 2000

/* Minimum difference in load to justify migration */
#define CTAE_MIGRATION_COST_THRESHOLD 10

/* Function prototypes */
int ctae_policy_init(void);
void ctae_policy_cleanup(void);
int ctae_policy_start(void);
void ctae_policy_stop(void);

#endif /* _CTAE_POLICY_H */
