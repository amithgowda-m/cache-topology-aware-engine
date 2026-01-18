/* CTAE Policy Engine Header
 * Cache-Topology-Aware Execution Engine
 *
 * Purpose: Define policy engine API and tunables
 * Architecture: x86_64
 * Kernel: Linux 5.15+
 */

#ifndef _CTAE_POLICY_H
#define _CTAE_POLICY_H

#include <linux/hashtable.h>
#include <linux/types.h>


/* Module metadata */
#define CTAE_POLICY_VERSION "0.2.0"

/* Policy Tunables */
/* How often the policy thread wakes up to check for contention */
#define CTAE_POLICY_INTERVAL_MS 500

/* Minimum time (ms) between migrating the SAME task again (prevent thrashing)
 */
#define CTAE_MIGRATION_COOLOFF_MS 2000

/* Verification window - time to wait before checking if migration succeeded */
#define CTAE_MIGRATION_VERIFY_MS 500

/* LLC metrics logging window - time to wait before re-sampling LLC after
 * migration */
#define CTAE_LLC_RESAMPLE_MS 1000

/* Maximum migration retries if verification fails */
#define CTAE_MAX_MIGRATION_RETRIES 2

/* Hash table size for migration tracking (must be power of 2) */
#define CTAE_MIGRATION_HASH_BITS 8

/* Migration tracking record */
struct ctae_migration_record {
  struct hlist_node hash_node; /* Hash table linkage */
  pid_t pid;                   /* Process ID */
  unsigned int source_cpu;     /* CPU migrated from */
  unsigned int target_cpu;     /* CPU migrated to */
  unsigned int target_domain;  /* Domain migrated to */
  u64 migration_time;          /* Timestamp of migration (ns) */
  u64 llc_miss_rate_before;    /* LLC miss rate before migration */
  unsigned int retry_count;    /* Number of retries */
  bool verified;               /* Migration verified */
};

/* Function prototypes */
int ctae_policy_init(void);
void ctae_policy_cleanup(void);
int ctae_policy_start(void);
void ctae_policy_stop(void);

#endif /* _CTAE_POLICY_H */
