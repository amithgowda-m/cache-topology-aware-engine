/* CTAE Policy Engine Implementation
 *
 * Logic to migrate threads based on cache topology and contention stats.
 */

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/timekeeping.h>

#include "../core/ctae_core.h"
#include "../monitor/ctae_monitor.h"
#include "ctae_policy.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Research Team");
MODULE_DESCRIPTION("CTAE Policy Engine");
MODULE_VERSION(CTAE_POLICY_VERSION);

/* Policy thread task structure */
static struct task_struct *ctae_policy_thread = NULL;

/*
 * Helper: Check if a task is safe to migrate.
 * We avoid migrating kernel threads (except specific ones, but generally safer
 * to avoid), and tasks that are already pinned to a specific CPU by the user.
 */
static bool ctae_is_migratable(struct task_struct *p) {
  if (p->flags & PF_KTHREAD)
    return false;

  /* If task is pinned to a single CPU, don't move it */
  if (cpumask_weight(&p->cpus_mask) == 1)
    return false;

  return true;
}

/*
 * Find a target CPU in a different cache domain.
 * We want a domain that is NOT the source domain, and a CPU that is relatively
 * idle.
 */
static int ctae_find_target_cpu(unsigned int source_cpu,
                                unsigned int *target_cpu) {
  struct ctae_cache_domain *source_domain, *domain;
  struct ctae_pmu_stats stats;
  unsigned int best_cpu = nr_cpu_ids;
  u64 lowest_load = U64_MAX;
  int ret;
  unsigned int cpu;

  source_domain = ctae_find_domain_by_cpu(source_cpu);
  if (!source_domain)
    return -EINVAL;

  /* Iterate over all domains to find a candidate */
  if (!ctae_global_topology)
    return -EINVAL;

  spin_lock(&ctae_global_topology->lock);
  list_for_each_entry(domain, &ctae_global_topology->cache_domains, list) {
    /* Skip source domain */
    if (domain == source_domain)
      continue;

    /* Look for best CPU in this domain */
    for_each_cpu(cpu, domain->cpu_mask) {
      /* Skip offline CPUs */
      if (!cpu_online(cpu))
        continue;

      /* Check stats */
      ret = ctae_get_cpu_stats(cpu, &stats);
      if (ret)
        continue;

      /* Heuristic: lowest LLC miss rate (or just lowest misses) */
      // Using miss rate as proxy for "load" on the cache
      if (stats.llc_miss_rate < lowest_load) {
        lowest_load = stats.llc_miss_rate;
        best_cpu = cpu;
      }
    }
  }
  spin_unlock(&ctae_global_topology->lock);

  if (best_cpu < nr_cpu_ids) {
    *target_cpu = best_cpu;
    return 0;
  }

  return -ENODEV;
}

/*
 * Perform migration for a contended CPU.
 */
static void ctae_handle_contention(unsigned int cpu) {
  struct task_struct *curr_task;
  unsigned int target_cpu;
  int ret;

  /*
   * Get the task currently running on this CPU.
   * Note: This is an approximation. synchronizing this safely requires RCU
   * lock.
   */
  rcu_read_lock();
  curr_task = rcu_dereference(cpu_curr(cpu));
  if (!curr_task) {
    rcu_read_unlock();
    return;
  }
  get_task_struct(curr_task); /* Bump refcount so it doesn't disappear */
  rcu_read_unlock();

  if (!ctae_is_migratable(curr_task)) {
    goto out;
  }

  /* Find a better place for it */
  ret = ctae_find_target_cpu(cpu, &target_cpu);
  if (ret == 0) {
    pr_info("CTAE Policy: Migrating PID %d (%s) from CPU %d to CPU %d to "
            "reduce contention.\n",
            curr_task->pid, curr_task->comm, cpu, target_cpu);

    /* Apply migration */
    ret = set_cpus_allowed_ptr(curr_task, cpumask_of(target_cpu));
    if (ret) {
      pr_warn("CTAE Policy: Failed to migrate task: %d\n", ret);
    }
  } else {
    // pr_debug("CTAE Policy: No suitable target CPU found for CPU %d\n", cpu);
  }

out:
  put_task_struct(curr_task);
}

/*
 * Main Policy Loop
 */
static int ctae_policy_kthread_fn(void *data) {
  unsigned int cpu;

  pr_info("CTAE Policy: Thread started.\n");

  while (!kthread_should_stop()) {
    /* Sleep for interval */
    msleep(CTAE_POLICY_INTERVAL_MS);

    if (kthread_should_stop())
      break;

    /* 1. Check for contention globally */
    /* Monitor module already checks this, we just need to detect it */
    /* But let's iterate CPUs and check the flag set by Monitor */

    for_each_online_cpu(cpu) {
      if (ctae_detect_contention(cpu)) {
        ctae_handle_contention(cpu);
      }
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
  return ctae_policy_start();
}

void ctae_policy_cleanup(void) {
  ctae_policy_stop();
  pr_info("CTAE Policy: Cleanup complete.\n");
}

/* Module hooks */
static int __init ctae_policy_module_init(void) { return ctae_policy_init(); }

static void __exit ctae_policy_module_exit(void) { ctae_policy_cleanup(); }

module_init(ctae_policy_module_init);
module_exit(ctae_policy_module_exit);
