#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/version.h>

/* Local header include */
#include "ctae_core.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Developer");
MODULE_DESCRIPTION("CTAE Core Module");

/* * Definition of the global domain. 
 * The compiler MUST see struct ctae_l3_domain { ... } 
 * from ctae_core.h before this line.
 */
struct ctae_l3_domain ctae_global_domain;
EXPORT_SYMBOL(ctae_global_domain);

static int __init ctae_core_init(void)
{
    pr_info("CTAE: Core module loading...\n");
    
    cpumask_copy(&ctae_global_domain.cpus, cpu_online_mask);
    ctae_global_domain.id = 0;
    
    pr_info("CTAE: Core initialized. Discovered %d CPUs.\n", 
            cpumask_weight(&ctae_global_domain.cpus));
            
    return 0;
}

static void __exit ctae_core_exit(void)
{
    pr_info("CTAE: Core module unloaded.\n");
}

module_init(ctae_core_init);
module_exit(ctae_core_exit);