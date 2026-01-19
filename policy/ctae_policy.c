#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/version.h>

#include "ctae_policy.h"
#include "../core/ctae_core.h"
#include "../monitor/ctae_monitor.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Developer");
MODULE_DESCRIPTION("CTAE Policy Module");

static struct timer_list ctae_timer;

static void ctae_migrate_victim(int source_cpu)
{
    struct task_struct *p, *t;
    struct task_struct *victim = NULL;
    int target_cpu = -1;
    int best_metric = 10000;
    int cpu;
    struct ctae_cpu_stats stats;
    
    // 1. Find the quietest CPU (lowest miss rate)
    for_each_cpu(cpu, &ctae_global_domain.cpus) {
        if (cpu == source_cpu) continue;
        
        if (ctae_get_cpu_stats(cpu, &stats) == 0) {
            if ((int)stats.miss_rate_per_thousand < best_metric) {
                best_metric = stats.miss_rate_per_thousand;
                target_cpu = cpu;
            }
        }
    }

    if (target_cpu == -1) {
        pr_warn("CTAE: No suitable target CPU found for migration.\n");
        return;
    }

    // 2. Find a victim task on the source CPU
    rcu_read_lock();
    for_each_process_thread(p, t) {
        if (task_cpu(t) == source_cpu) {
            // Skip kernel threads
            if (t->flags & PF_KTHREAD) continue;
            
            // Check task state (Running)
            // Kernel 5.14+ uses __state instead of state
            if (READ_ONCE(t->__state) == TASK_RUNNING) {
                // Get reference before using outside RCU
                get_task_struct(t);
                victim = t;
                break; // Found one
            }
        }
    }
    rcu_read_unlock();

    // 3. Migrate the victim
    if (victim) {
        int ret = set_cpus_allowed_ptr(victim, cpumask_of(target_cpu));
        if (ret == 0) {
            pr_info("CTAE: MIGRATION [CPU%d -> CPU%d] Task: %s (PID: %d)\n", 
                    source_cpu, target_cpu, victim->comm, victim->pid);
        } else {
            pr_err("CTAE: Migration failed for PID %d, ret=%d\n", victim->pid, ret);
        }
        put_task_struct(victim);
    }
}

void ctae_timer_callback(struct timer_list *t)
{
    int cpu;
    struct ctae_cpu_stats stats;

    for_each_cpu(cpu, &ctae_global_domain.cpus) {
        if (ctae_get_cpu_stats(cpu, &stats) == 0) {
            // Debug output to dmesg to verify stats are reading
            /* pr_info("CTAE: CPU%d | Refs: %llu | Misses: %llu | Rate: %u/1000\n",
                    cpu, stats.llc_references, stats.llc_misses, stats.miss_rate_per_thousand); 
            */

            if (stats.miss_rate_per_thousand > CTAE_MISS_THRESHOLD_PER_THOUSAND) {
                pr_alert("CTAE: HIGH CACHE THRASHING on CPU%d (Rate: %u/1000)\n", 
                         cpu, stats.miss_rate_per_thousand);
                
                ctae_migrate_victim(cpu);
            }
        }
    }

    // Reschedule
    mod_timer(&ctae_timer, jiffies + msecs_to_jiffies(CTAE_POLL_INTERVAL_MS));
}

static int __init ctae_policy_init(void)
{
    pr_info("CTAE: Policy module loading...\n");
    pr_info("CTAE: Configured Interval: %dms, Threshold: %d/1000 misses\n", 
            CTAE_POLL_INTERVAL_MS, CTAE_MISS_THRESHOLD_PER_THOUSAND);

    timer_setup(&ctae_timer, ctae_timer_callback, 0);
    mod_timer(&ctae_timer, jiffies + msecs_to_jiffies(CTAE_POLL_INTERVAL_MS));

    return 0;
}

static void __exit ctae_policy_exit(void)
{
    del_timer_sync(&ctae_timer);
    pr_info("CTAE: Policy unloaded.\n");
}

module_init(ctae_policy_init);
module_exit(ctae_policy_exit);