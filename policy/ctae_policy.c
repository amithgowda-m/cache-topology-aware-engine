/* CTAE Policy Engine Implementation
 *
 * Logic to migrate threads based on cache topology and contention stats.
 *
 * Fixes implemented:
 * 1. Migrates all contending tasks, not just cpu_curr()
 * 2. Migration cooldown tracking with hash table
 * 3. Migration verification after 500ms
 * 4. LLC metrics logging before/after migration
 * 5. Works with multi-domain topology
 * 6. Uses proper migration API with domain-wide affinity
 */

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>


#include "../core/ctae_core.h"
#include "../monitor/ctae_monitor.h"
#include "ctae_policy.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Research Team");
MODULE_DESCRIPTION("CTAE Policy Engine - Fixed Migration");
MODULE_VERSION(CTAE_POLICY_VERSION);

/* Policy thread task structure */
static struct task_struct *ctae_policy_thread = NULL;

/* Migration tracking hash table */
static DEFINE_HASHTABLE(migration_records, CTAE_MIGRATION_HASH_BITS);
static DEFINE_SPINLOCK(migration_lock);

/*
 * Find migration record for a PID
 */
static struct ctae_migration_record *ctae_find_migration_record(pid_t pid) {
  struct ctae_migration_record *record;

  hash_for_each_possible(migration_records, record, hash_node, pid) {
    if (record->pid == pid)
      return record;
  }

  return NULL;
}

/*
 * Add or update migration record
 */
static struct ctae_migration_record *
ctae_add_migration_record(pid_t pid, unsigned int source_cpu,
                          unsigned int target_cpu, unsigned int target_domain,
                          u64 llc_miss_rate_before) {
  struct ctae_migration_record *record;

  record = ctae_find_migration_record(pid);
  if (!record) {
    record = kzalloc(sizeof(*record), GFP_ATOMIC);
    if (!record)
      return NULL;

    record->pid = pid;
    hash_add(migration_records, &record->hash_node, pid);
  }

  record->source_cpu = source_cpu;
  record->target_cpu = target_cpu;
  record->target_domain = target_domain;
  record->migration_time = ktime_get_ns();
  record->llc_miss_rate_before = llc_miss_rate_before;
  record->verified = false;
  record->retry_count = 0;

  return record;
}

/*
 * Check if task is in migration cooldown period
 */
static bool ctae_in_cooldown(pid_t pid) {
  struct ctae_migration_record *record;
  u64 now = ktime_get_ns();
  u64 cooldown_ns = (u64)CTAE_MIGRATION_COOLOFF_MS * 1000000ULL;

  record = ctae_find_migration_record(pid);
  if (!record)
    return false;

  if ((now - record->migration_time) < cooldown_ns)
    return true;

  return false;
}

/*
 * Clean up stale migration records (for exited processes)
 */
static void ctae_cleanup_stale_records(void) {
  struct ctae_migration_record *record;
  struct hlist_node *tmp;
  int bkt;
  u64 now = ktime_get_ns();
  u64 stale_threshold = 60ULL * 1000000000ULL; /* 60 seconds */

  hash_for_each_safe(migration_records, bkt, tmp, record, hash_node) {
    if ((now - record->migration_time) > stale_threshold) {
      hash_del(&record->hash_node);
      kfree(record);
    }
  }
}

/*
 * Helper: Check if a task is safe to migrate.
 */
static bool ctae_is_migratable(struct task_struct *p) {
  if (!p)
    return false;

  /* Don't migrate kernel threads */
  if (p->flags & PF_KTHREAD)
    return false;

  /* Don't migrate if task is exiting */
  if (p->flags & PF_EXITING)
    return false;

  /* If task is pinned to a single CPU by user, don't move it */
  if (cpumask_weight(&p->cpus_mask) == 1)
    return false;

  return true;
}

/*
 * Find a target CPU in a different cache domain.
 */
static int ctae_find_target_cpu(unsigned int source_cpu,
                                unsigned int *target_cpu,
                                unsigned int *target_domain_id) {
  struct ctae_cache_domain *source_domain, *domain, *best_domain = NULL;
  unsigned int cpu;
  u64 lowest_load = U64_MAX;
  u64 domain_load;

  source_domain = ctae_find_domain_by_cpu(source_cpu);
  if (!source_domain)
    return -EINVAL;

  /* Iterate over all domains to find best candidate */
  if (!ctae_global_topology)
    return -EINVAL;

  spin_lock(&ctae_global_topology->lock);
  list_for_each_entry(domain, &ctae_global_topology->cache_domains, list) {
    /* Skip source domain */
    if (domain == source_domain)
      continue;

    /* Get aggregate load for this domain */
    domain_load = ctae_get_domain_llc_miss_rate(domain->domain_id);

    if (domain_load < lowest_load) {
      lowest_load = domain_load;
      best_domain = domain;
    }
  }
  spin_unlock(&ctae_global_topology->lock);

  if (!best_domain)
    return -ENODEV;

  /* Find best CPU in the selected domain */
  lowest_load = U64_MAX;
  *target_cpu = nr_cpu_ids;

  for_each_cpu(cpu, best_domain->cpu_mask) {
    u64 cpu_load;

    if (!cpu_online(cpu))
      continue;

    cpu_load = ctae_get_cpu_llc_miss_rate(cpu);
    if (cpu_load < lowest_load) {
      lowest_load = cpu_load;
      *target_cpu = cpu;
    }
  }

  if (*target_cpu >= nr_cpu_ids)
    return -ENODEV;

  *target_domain_id = best_domain->domain_id;
  return 0;
}

/*
 * Verify that migration succeeded
 */
static void ctae_verify_migration(struct ctae_migration_record *record) {
  struct task_struct *task;
  unsigned int current_cpu;
  struct ctae_cache_domain *target_domain;
  u64 llc_miss_rate_after;
  s64 improvement;

  if (!record || record->verified)
    return;

  /* Find the task */
  rcu_read_lock();
  task = find_task_by_vpid(record->pid);
  if (!task) {
    rcu_read_unlock();
    return; /* Task exited */
  }
  get_task_struct(task);
  rcu_read_unlock();

  current_cpu = task_cpu(task);
  target_domain = ctae_find_domain_by_id(record->target_domain);

  /* Check if task is now in the target domain */
  if (target_domain && cpumask_test_cpu(current_cpu, target_domain->cpu_mask)) {
    record->verified = true;

    /* Log LLC metrics after migration */
    llc_miss_rate_after = ctae_get_cpu_llc_miss_rate(record->source_cpu);
    improvement = (s64)record->llc_miss_rate_before - (s64)llc_miss_rate_after;

    pr_info("CTAE Policy: Migration VERIFIED - PID %d (%s) now on CPU %u "
            "(domain %u)\n",
            record->pid, task->comm, current_cpu, record->target_domain);
    pr_info("CTAE Policy: LLC metrics - Before: %llu/1000, After: %llu/1000, "
            "Improvement: %lld/1000\n",
            record->llc_miss_rate_before, llc_miss_rate_after, improvement);
  } else {
    pr_warn("CTAE Policy: Migration FAILED - PID %d still on CPU %u (expected "
            "domain %u)\n",
            record->pid, current_cpu, record->target_domain);

    /* Retry if under limit */
    if (record->retry_count < CTAE_MAX_MIGRATION_RETRIES) {
      record->retry_count++;
      pr_info("CTAE Policy: Scheduling retry %u/%u for PID %d\n",
              record->retry_count, CTAE_MAX_MIGRATION_RETRIES, record->pid);
      /* Reset migration time to allow retry */
      record->migration_time = 0;
    }
  }

  put_task_struct(task);
}

/*
 * Perform migration for a contended CPU.
 * This now migrates ALL tasks on the CPU, not just cpu_curr().
 */
static void ctae_handle_contention(unsigned int cpu) {
  struct task_struct *p, *t;
  unsigned int target_cpu, target_domain_id;
  struct ctae_cache_domain *target_domain;
  cpumask_var_t target_mask;
  int ret;
  u64 llc_miss_rate_before;
  unsigned long flags;

  if (!zalloc_cpumask_var(&target_mask, GFP_KERNEL))
    return;

  /* Find target domain */
  ret = ctae_find_target_cpu(cpu, &target_cpu, &target_domain_id);
  if (ret != 0) {
    free_cpumask_var(target_mask);
    return;
  }

  target_domain = ctae_find_domain_by_id(target_domain_id);
  if (!target_domain) {
    free_cpumask_var(target_mask);
    return;
  }

  /* Get LLC miss rate before migration */
  llc_miss_rate_before = ctae_get_cpu_llc_miss_rate(cpu);

  /* Set target mask to all CPUs in target domain */
  cpumask_copy(target_mask, target_domain->cpu_mask);

  /* Iterate through all tasks and migrate those running on this CPU */
  rcu_read_lock();
  for_each_process_thread(p, t) {
    unsigned int task_cpu;
    struct ctae_migration_record *record;

    if (!ctae_is_migratable(t))
      continue;

    task_cpu = task_cpu(t);
    if (task_cpu != cpu)
      continue;

    /* Check cooldown */
    spin_lock_irqsave(&migration_lock, flags);
    if (ctae_in_cooldown(t->pid)) {
      spin_unlock_irqrestore(&migration_lock, flags);
      continue;
    }

    /* Add migration record */
    record = ctae_add_migration_record(t->pid, cpu, target_cpu,
                                       target_domain_id, llc_miss_rate_before);
    spin_unlock_irqrestore(&migration_lock, flags);

    if (!record)
      continue;

    pr_info("CTAE Policy: Migrating PID %d (%s) from CPU %u to domain %u "
            "(target CPU %u)\n",
            t->pid, t->comm, cpu, target_domain_id, target_cpu);

    /* Apply migration - use domain mask, not single CPU */
    get_task_struct(t);
    ret = set_cpus_allowed_ptr(t, target_mask);
    if (ret) {
      pr_warn("CTAE Policy: set_cpus_allowed_ptr failed for PID %d: %d\n",
              t->pid, ret);
    } else {
      /* Wake up task to trigger rescheduling */
      wake_up_process(t);
    }
    put_task_struct(t);
  }
  rcu_read_unlock();

  free_cpumask_var(target_mask);
}

/*
 * Verify pending migrations
 */
static void ctae_verify_pending_migrations(void) {
  struct ctae_migration_record *record;
  struct hlist_node *tmp;
  int bkt;
  u64 now = ktime_get_ns();
  u64 verify_window_ns = (u64)CTAE_MIGRATION_VERIFY_MS * 1000000ULL;
  unsigned long flags;

  spin_lock_irqsave(&migration_lock, flags);
  hash_for_each_safe(migration_records, bkt, tmp, record, hash_node) {
    if (!record->verified &&
        (now - record->migration_time) >= verify_window_ns) {
      spin_unlock_irqrestore(&migration_lock, flags);
      ctae_verify_migration(record);
      spin_lock_irqsave(&migration_lock, flags);
    }
  }
  spin_unlock_irqrestore(&migration_lock, flags);
}

/*
 * Main Policy Loop
 */
static int ctae_policy_kthread_fn(void *data) {
  unsigned int cpu;
  unsigned int cleanup_counter = 0;

  pr_info("CTAE Policy: Thread started.\n");

  while (!kthread_should_stop()) {
    /* Sleep for interval */
    msleep(CTAE_POLICY_INTERVAL_MS);

    if (kthread_should_stop())
      break;

    /* Check for contention and handle migrations */
    for_each_online_cpu(cpu) {
      if (ctae_detect_contention(cpu)) {
        ctae_handle_contention(cpu);
      }
    }

    /* Verify pending migrations */
    ctae_verify_pending_migrations();

    /* Periodic cleanup of stale records (every ~30 seconds) */
    if (++cleanup_counter >= (30000 / CTAE_POLICY_INTERVAL_MS)) {
      ctae_cleanup_stale_records();
      cleanup_counter = 0;
    }
  }

  pr_info("CTAE Policy: Thread stopping.\n");
  return 0;
}

int ctae_policy_start(void) {
  if (ctae_policy_thread)
    return 0;

  ctae_policy_thread = kthread_run(ctae_policy_kthread_fn, NULL, "ctae_policy");
  if (IS_ERR(ctae_policy_thread)) {
    pr_err("CTAE Policy: Failed to start kernel thread\n");
    return PTR_ERR(ctae_policy_thread);
  }

  pr_info("CTAE Policy: Started with %dms interval, %dms cooldown\n",
          CTAE_POLICY_INTERVAL_MS, CTAE_MIGRATION_COOLOFF_MS);

  return 0;
}
EXPORT_SYMBOL(ctae_policy_start);

void ctae_policy_stop(void) {
  if (ctae_policy_thread) {
    kthread_stop(ctae_policy_thread);
    ctae_policy_thread = NULL;
  }
}
EXPORT_SYMBOL(ctae_policy_stop);

int ctae_policy_init(void) {
  pr_info("CTAE Policy: Initializing...\n");
  hash_init(migration_records);
  return ctae_policy_start();
}

void ctae_policy_cleanup(void) {
  struct ctae_migration_record *record;
  struct hlist_node *tmp;
  int bkt;
  unsigned long flags;

  ctae_policy_stop();

  /* Free all migration records */
  spin_lock_irqsave(&migration_lock, flags);
  hash_for_each_safe(migration_records, bkt, tmp, record, hash_node) {
    hash_del(&record->hash_node);
    kfree(record);
  }
  spin_unlock_irqrestore(&migration_lock, flags);

  pr_info("CTAE Policy: Cleanup complete.\n");
}

/* Module hooks */
static int __init ctae_policy_module_init(void) { return ctae_policy_init(); }

static void __exit ctae_policy_module_exit(void) { ctae_policy_cleanup(); }

module_init(ctae_policy_module_init);
module_exit(ctae_policy_module_exit);
