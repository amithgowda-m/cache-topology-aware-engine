/* CTAE Monitor Module Implementation
 * Performance Monitoring Unit hooks for LLC miss tracking
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/perf_event.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/timer.h>


#include "../core/ctae_core.h"
#include "ctae_monitor.h"


MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Research Team");
MODULE_DESCRIPTION("CTAE Monitor - PMU Performance Tracking");
MODULE_VERSION(CTAE_MONITOR_VERSION);

/* Global monitor state */
struct ctae_monitor_state *ctae_global_monitor = NULL;
EXPORT_SYMBOL(ctae_global_monitor);

/* Debugfs directory */
static struct dentry *ctae_debugfs_dir = NULL;

/* Timer callback for periodic sampling */
static void ctae_sampling_timer_callback(struct timer_list *timer) {
  /* Sample all CPUs */
  ctae_pmu_sample_all();

  /* Check for contention */
  ctae_check_all_contention();

  /* Reschedule timer if monitoring is enabled */
  if (ctae_global_monitor && ctae_global_monitor->monitoring_enabled) {
    mod_timer(&ctae_global_monitor->sampling_timer,
              jiffies +
                  msecs_to_jiffies(ctae_global_monitor->sample_period_ms));
  }
}

/* Setup performance events for a specific CPU */
int ctae_pmu_setup_events(unsigned int cpu) {
  struct ctae_cpu_monitor *monitor;
  struct perf_event_attr attr;
  struct perf_event *event;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors)
    return -EINVAL;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);

  /* Initialize perf_event_attr structure */
  memset(&attr, 0, sizeof(struct perf_event_attr));
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(struct perf_event_attr);
  attr.pinned = 0;
  attr.disabled = 1;
  attr.exclude_kernel = 0;
  attr.exclude_hv = 1;

  /* Setup LLC misses counter */
  attr.config = PERF_COUNT_HW_CACHE_MISSES;
  event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
  if (IS_ERR(event)) {
    pr_warn("CTAE: Failed to create LLC miss counter for CPU %u: %ld\n", cpu,
            PTR_ERR(event));
    monitor->events[CTAE_PMU_LLC_MISSES] = NULL;
  } else {
    monitor->events[CTAE_PMU_LLC_MISSES] = event;
    perf_event_enable(event);
  }

  /* Setup LLC references counter */
  attr.config = PERF_COUNT_HW_CACHE_REFERENCES;
  event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
  if (IS_ERR(event)) {
    pr_warn("CTAE: Failed to create LLC ref counter for CPU %u: %ld\n", cpu,
            PTR_ERR(event));
    monitor->events[CTAE_PMU_LLC_REFERENCES] = NULL;
  } else {
    monitor->events[CTAE_PMU_LLC_REFERENCES] = event;
    perf_event_enable(event);
  }

  /* Setup instructions counter */
  attr.config = PERF_COUNT_HW_INSTRUCTIONS;
  event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
  if (IS_ERR(event)) {
    monitor->events[CTAE_PMU_INSTRUCTIONS] = NULL;
  } else {
    monitor->events[CTAE_PMU_INSTRUCTIONS] = event;
    perf_event_enable(event);
  }

  /* Setup CPU cycles counter */
  attr.config = PERF_COUNT_HW_CPU_CYCLES;
  event = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
  if (IS_ERR(event)) {
    monitor->events[CTAE_PMU_CYCLES] = NULL;
  } else {
    monitor->events[CTAE_PMU_CYCLES] = event;
    perf_event_enable(event);
  }

  pr_info("CTAE: PMU events configured for CPU %u\n", cpu);
  return 0;
}
EXPORT_SYMBOL(ctae_pmu_setup_events);

/* Cleanup performance events for a CPU */
void ctae_pmu_cleanup_events(unsigned int cpu) {
  struct ctae_cpu_monitor *monitor;
  int i;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors)
    return;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);

  for (i = 0; i < CTAE_PMU_MAX_EVENTS; i++) {
    if (monitor->events[i]) {
      perf_event_disable(monitor->events[i]);
      perf_event_release_kernel(monitor->events[i]);
      monitor->events[i] = NULL;
    }
  }
}
EXPORT_SYMBOL(ctae_pmu_cleanup_events);

/* Sample performance counters for a specific CPU */
void ctae_pmu_sample_cpu(unsigned int cpu) {
  struct ctae_cpu_monitor *monitor;
  struct ctae_pmu_stats *stats;
  unsigned long flags;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors)
    return;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);
  stats = &monitor->stats;

  spin_lock_irqsave(&monitor->lock, flags);

  /* Read counters */
  if (monitor->events[CTAE_PMU_LLC_MISSES])
    stats->llc_misses =
        ctae_read_perf_counter(monitor->events[CTAE_PMU_LLC_MISSES]);

  if (monitor->events[CTAE_PMU_LLC_REFERENCES])
    stats->llc_references =
        ctae_read_perf_counter(monitor->events[CTAE_PMU_LLC_REFERENCES]);

  if (monitor->events[CTAE_PMU_INSTRUCTIONS])
    stats->instructions =
        ctae_read_perf_counter(monitor->events[CTAE_PMU_INSTRUCTIONS]);

  if (monitor->events[CTAE_PMU_CYCLES])
    stats->cycles = ctae_read_perf_counter(monitor->events[CTAE_PMU_CYCLES]);

  /* Calculate deltas */
  stats->delta_llc_misses = stats->llc_misses - monitor->prev_stats.llc_misses;
  stats->delta_llc_references =
      stats->llc_references - monitor->prev_stats.llc_references;

  /* Update timestamp */
  stats->last_sample_time = ktime_get_ns();
  stats->sample_count++;

  /* Save for next delta calculation */
  monitor->prev_stats = *stats;

  spin_unlock_irqrestore(&monitor->lock, flags);
}
EXPORT_SYMBOL(ctae_pmu_sample_cpu);

/* Update derived statistics */
void ctae_pmu_update_stats(unsigned int cpu) {
  struct ctae_cpu_monitor *monitor;
  struct ctae_pmu_stats *stats;
  unsigned long flags;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors)
    return;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);
  stats = &monitor->stats;

  spin_lock_irqsave(&monitor->lock, flags);

  /* Calculate LLC miss rate (per 1000 references) */
  if (stats->llc_references > 0) {
    stats->llc_miss_rate = (stats->llc_misses * 1000) / stats->llc_references;
  } else {
    stats->llc_miss_rate = 0;
  }

  /* Calculate IPC (instructions per cycle * 1000) */
  if (stats->cycles > 0) {
    stats->ipc = (stats->instructions * 1000) / stats->cycles;
  } else {
    stats->ipc = 0;
  }

  spin_unlock_irqrestore(&monitor->lock, flags);
}
EXPORT_SYMBOL(ctae_pmu_update_stats);

/* Sample all CPUs */
void ctae_pmu_sample_all(void) {
  unsigned int cpu;

  for_each_online_cpu(cpu) {
    ctae_pmu_sample_cpu(cpu);
    ctae_pmu_update_stats(cpu);
  }
}
EXPORT_SYMBOL(ctae_pmu_sample_all);

/* Detect contention on a specific CPU */
bool ctae_detect_contention(unsigned int cpu) {
  struct ctae_cpu_monitor *monitor;
  struct ctae_pmu_stats *stats;
  bool contention = false;
  unsigned long flags;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors)
    return false;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);
  stats = &monitor->stats;

  spin_lock_irqsave(&monitor->lock, flags);

  /* Check if miss rate exceeds threshold (already multiplied by 1000) */
  if (stats->llc_miss_rate > CTAE_LLC_MISS_RATE_THRESHOLD) {
    contention = true;
  }

  /* Check absolute miss count in delta */
  if (stats->delta_llc_misses > CTAE_HIGH_CONTENTION_THRESHOLD) {
    contention = true;
  }

  stats->high_contention = contention;

  spin_unlock_irqrestore(&monitor->lock, flags);

  return contention;
}
EXPORT_SYMBOL(ctae_detect_contention);

/* Check contention across all CPUs */
void ctae_check_all_contention(void) {
  unsigned int cpu;
  unsigned int contended_cpus = 0;

  for_each_online_cpu(cpu) {
    if (ctae_detect_contention(cpu)) {
      contended_cpus++;
    }
  }

  if (contended_cpus > 0) {
    pr_info("CTAE: Detected contention on %u CPUs\n", contended_cpus);
  }
}
EXPORT_SYMBOL(ctae_check_all_contention);

/* Debugfs: Show stats for all CPUs */
static int ctae_stats_show(struct seq_file *m, void *v) {
  unsigned int cpu;
  struct ctae_pmu_stats stats;
  int ret;

  seq_printf(m, "CTAE Performance Monitor Statistics\n");
  seq_printf(m, "====================================\n\n");

  if (!ctae_global_monitor || !ctae_global_monitor->monitoring_enabled) {
    seq_printf(m, "Monitoring is not enabled\n");
    return 0;
  }

  seq_printf(m, "Sample period: %u ms\n",
             ctae_global_monitor->sample_period_ms);
  seq_printf(m, "\n");

  for_each_online_cpu(cpu) {
    ret = ctae_get_cpu_stats(cpu, &stats);
    if (ret)
      continue;

    seq_printf(m, "CPU %2u:\n", cpu);
    seq_printf(m, "  LLC Misses:      %12llu (delta: %llu)\n", stats.llc_misses,
               stats.delta_llc_misses);
    seq_printf(m, "  LLC References:  %12llu (delta: %llu)\n",
               stats.llc_references, stats.delta_llc_references);
    seq_printf(m, "  Miss Rate:       %12llu per 1000\n", stats.llc_miss_rate);
    seq_printf(m, "  Instructions:    %12llu\n", stats.instructions);
    seq_printf(m, "  Cycles:          %12llu\n", stats.cycles);
    seq_printf(m, "  IPC:             %12llu.%03llu\n", stats.ipc / 1000,
               stats.ipc % 1000);
    seq_printf(m, "  Samples:         %12llu\n", stats.sample_count);
    seq_printf(m, "  Contention:      %s\n",
               stats.high_contention ? "YES" : "NO");
    seq_printf(m, "\n");
  }

  return 0;
}

static int ctae_stats_open(struct inode *inode, struct file *file) {
  return single_open(file, ctae_stats_show, NULL);
}

static const struct file_operations ctae_stats_fops = {
    .owner = THIS_MODULE,
    .open = ctae_stats_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* Debugfs: Trigger immediate sample */
static ssize_t ctae_trigger_write(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos) {
  pr_info("CTAE: Manual sampling triggered\n");
  ctae_pmu_sample_all();
  ctae_check_all_contention();
  pr_info("CTAE: Manual sampling complete\n");
  return count;
}

static const struct file_operations ctae_trigger_fops = {
    .owner = THIS_MODULE,
    .write = ctae_trigger_write,
};

/* Setup debugfs interface */
static int ctae_debugfs_init(void) {
  ctae_debugfs_dir = debugfs_create_dir("ctae", NULL);
  if (!ctae_debugfs_dir) {
    pr_err("CTAE: Failed to create debugfs directory\n");
    return -ENOMEM;
  }

  debugfs_create_file("stats", 0444, ctae_debugfs_dir, NULL, &ctae_stats_fops);
  debugfs_create_file("trigger", 0200, ctae_debugfs_dir, NULL,
                      &ctae_trigger_fops);

  pr_info("CTAE: Debugfs interface created at /sys/kernel/debug/ctae/\n");
  return 0;
}

/* Cleanup debugfs */
static void ctae_debugfs_cleanup(void) {
  if (ctae_debugfs_dir) {
    debugfs_remove_recursive(ctae_debugfs_dir);
    ctae_debugfs_dir = NULL;
  }
}

/* Get CPU statistics */
int ctae_get_cpu_stats(unsigned int cpu, struct ctae_pmu_stats *stats) {
  struct ctae_cpu_monitor *monitor;
  unsigned long flags;

  if (!ctae_global_monitor || !ctae_global_monitor->cpu_monitors || !stats)
    return -EINVAL;

  if (cpu >= nr_cpu_ids)
    return -EINVAL;

  monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);

  spin_lock_irqsave(&monitor->lock, flags);
  *stats = monitor->stats;
  spin_unlock_irqrestore(&monitor->lock, flags);

  return 0;
}
EXPORT_SYMBOL(ctae_get_cpu_stats);

/* Print statistics for a single CPU */
void ctae_print_stats(unsigned int cpu) {
  struct ctae_pmu_stats stats;

  if (ctae_get_cpu_stats(cpu, &stats) != 0)
    return;

  pr_info("CTAE: CPU %u Stats:\n", cpu);
  pr_info("  LLC Misses: %llu (delta: %llu)\n", stats.llc_misses,
          stats.delta_llc_misses);
  pr_info("  LLC References: %llu (delta: %llu)\n", stats.llc_references,
          stats.delta_llc_references);
  pr_info("  Miss Rate: %llu per 1000\n", stats.llc_miss_rate);
  pr_info("  Instructions: %llu\n", stats.instructions);
  pr_info("  Cycles: %llu\n", stats.cycles);
  pr_info("  IPC: %llu.%03llu\n", stats.ipc / 1000, stats.ipc % 1000);
  pr_info("  Contention: %s\n", stats.high_contention ? "YES" : "NO");
}
EXPORT_SYMBOL(ctae_print_stats);

/* Print statistics for all CPUs */
void ctae_print_all_stats(void) {
  unsigned int cpu;

  pr_info("CTAE: ========== Performance Statistics ==========\n");

  for_each_online_cpu(cpu) { ctae_print_stats(cpu); }

  pr_info("CTAE: ==============================================\n");
}
EXPORT_SYMBOL(ctae_print_all_stats);

/* Start monitoring */
int ctae_monitor_start(void) {
  unsigned int cpu;
  int ret;

  if (!ctae_global_monitor) {
    pr_err("CTAE: Monitor not initialized\n");
    return -EINVAL;
  }

  if (ctae_global_monitor->monitoring_enabled) {
    pr_info("CTAE: Monitoring already started\n");
    return 0;
  }

  pr_info("CTAE: Starting PMU monitoring...\n");

  /* Setup events for all online CPUs */
  for_each_online_cpu(cpu) {
    ret = ctae_pmu_setup_events(cpu);
    if (ret) {
      pr_err("CTAE: Failed to setup events for CPU %u\n", cpu);
    }
  }

  /* Enable monitoring */
  ctae_global_monitor->monitoring_enabled = 1;

  /* Start sampling timer */
  mod_timer(&ctae_global_monitor->sampling_timer,
            jiffies + msecs_to_jiffies(ctae_global_monitor->sample_period_ms));

  pr_info("CTAE: Monitoring started (period: %u ms)\n",
          ctae_global_monitor->sample_period_ms);

  return 0;
}
EXPORT_SYMBOL(ctae_monitor_start);

/* Stop monitoring */
int ctae_monitor_stop(void) {
  unsigned int cpu;

  if (!ctae_global_monitor) {
    return -EINVAL;
  }

  if (!ctae_global_monitor->monitoring_enabled) {
    pr_info("CTAE: Monitoring already stopped\n");
    return 0;
  }

  pr_info("CTAE: Stopping PMU monitoring...\n");

  /* Disable monitoring */
  ctae_global_monitor->monitoring_enabled = 0;

  /* Stop timer */
  del_timer_sync(&ctae_global_monitor->sampling_timer);

  /* Cleanup events for all CPUs */
  for_each_online_cpu(cpu) { ctae_pmu_cleanup_events(cpu); }

  pr_info("CTAE: Monitoring stopped\n");

  return 0;
}
EXPORT_SYMBOL(ctae_monitor_stop);

/* Initialize monitor module */
int ctae_monitor_init(void) {
  unsigned int cpu;
  struct ctae_cpu_monitor *monitor;

  pr_info("CTAE: Initializing monitor module...\n");

  /* Allocate global state */
  ctae_global_monitor = kzalloc(sizeof(*ctae_global_monitor), GFP_KERNEL);
  if (!ctae_global_monitor) {
    pr_err("CTAE: Failed to allocate monitor state\n");
    return -ENOMEM;
  }

  /* Allocate per-CPU monitors */
  ctae_global_monitor->cpu_monitors = alloc_percpu(struct ctae_cpu_monitor);
  if (!ctae_global_monitor->cpu_monitors) {
    pr_err("CTAE: Failed to allocate per-CPU monitors\n");
    kfree(ctae_global_monitor);
    ctae_global_monitor = NULL;
    return -ENOMEM;
  }

  /* Initialize per-CPU monitors */
  for_each_possible_cpu(cpu) {
    monitor = per_cpu_ptr(ctae_global_monitor->cpu_monitors, cpu);
    monitor->cpu_id = cpu;
    memset(monitor->events, 0, sizeof(monitor->events));
    memset(&monitor->stats, 0, sizeof(monitor->stats));
    memset(&monitor->prev_stats, 0, sizeof(monitor->prev_stats));
    spin_lock_init(&monitor->lock);
  }

  /* Initialize timer */
  timer_setup(&ctae_global_monitor->sampling_timer,
              ctae_sampling_timer_callback, 0);

  /* Set default parameters */
  ctae_global_monitor->sample_period_ms = CTAE_MONITOR_PERIOD_MS;
  ctae_global_monitor->monitoring_enabled = 0;
  spin_lock_init(&ctae_global_monitor->lock);

  pr_info("CTAE: Monitor module initialized\n");

  return 0;
}

/* Cleanup monitor module */
void ctae_monitor_cleanup(void) {
  if (!ctae_global_monitor)
    return;

  pr_info("CTAE: Cleaning up monitor module...\n");

  /* Stop monitoring if active */
  if (ctae_global_monitor->monitoring_enabled) {
    ctae_monitor_stop();
  }

  /* Free per-CPU monitors */
  if (ctae_global_monitor->cpu_monitors) {
    free_percpu(ctae_global_monitor->cpu_monitors);
  }

  /* Free global state */
  kfree(ctae_global_monitor);
  ctae_global_monitor = NULL;

  pr_info("CTAE: Monitor module cleanup complete\n");
}

/* Helper: Get LLC miss rate for a specific CPU */
u64 ctae_get_cpu_llc_miss_rate(unsigned int cpu) {
  struct ctae_pmu_stats stats;

  if (ctae_get_cpu_stats(cpu, &stats) != 0)
    return 0;

  return stats.llc_miss_rate;
}
EXPORT_SYMBOL(ctae_get_cpu_llc_miss_rate);

/* Helper: Get aggregate LLC miss rate for a domain */
u64 ctae_get_domain_llc_miss_rate(unsigned int domain_id) {
  struct ctae_cache_domain *domain;
  unsigned int cpu;
  u64 total_miss_rate = 0;
  unsigned int cpu_count = 0;

  domain = ctae_find_domain_by_id(domain_id);
  if (!domain)
    return 0;

  /* Calculate average miss rate across all CPUs in domain */
  for_each_cpu(cpu, domain->cpu_mask) {
    if (!cpu_online(cpu))
      continue;

    total_miss_rate += ctae_get_cpu_llc_miss_rate(cpu);
    cpu_count++;
  }

  if (cpu_count == 0)
    return 0;

  return total_miss_rate / cpu_count;
}
EXPORT_SYMBOL(ctae_get_domain_llc_miss_rate);

/* Module init/exit */
static int __init ctae_monitor_module_init(void) {
  int ret;

  pr_info("CTAE: Loading monitor module v%s\n", CTAE_MONITOR_VERSION);

  ret = ctae_monitor_init();
  if (ret) {
    pr_err("CTAE: Monitor initialization failed\n");
    return ret;
  }

  /* Setup debugfs */
  ret = ctae_debugfs_init();
  if (ret) {
    pr_warn("CTAE: Debugfs setup failed, continuing without it\n");
  }

  /* Auto-start monitoring */
  ret = ctae_monitor_start();
  if (ret) {
    pr_warn("CTAE: Failed to auto-start monitoring\n");
  }

  pr_info("CTAE: Monitor module loaded successfully\n");
  pr_info("CTAE: View stats: cat /sys/kernel/debug/ctae/stats\n");
  pr_info("CTAE: Trigger sample: echo 1 > /sys/kernel/debug/ctae/trigger\n");
  return 0;
}

static void __exit ctae_monitor_module_exit(void) {
  pr_info("CTAE: Unloading monitor module...\n");

  ctae_debugfs_cleanup();
  ctae_monitor_cleanup();

  pr_info("CTAE: Monitor module unloaded\n");
}

module_init(ctae_monitor_module_init);
module_exit(ctae_monitor_module_exit);