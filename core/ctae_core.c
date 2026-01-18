/* CTAE Core Module Implementation
 * Cache-Topology-Aware Execution Engine
 * 
 * Implements cache topology discovery using CPUID instruction
 * and builds a kernel-space topology map using linked lists.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/utsname.h>
#include <linux/fs.h>
#include <asm/processor.h>

#include "ctae_core.h"

/* Module information */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CTAE Research Team");
MODULE_DESCRIPTION("Cache-Topology-Aware Execution Engine - Core Module");
MODULE_VERSION(CTAE_VERSION);

/* Global topology instance */
struct ctae_topology *ctae_global_topology = NULL;
EXPORT_SYMBOL(ctae_global_topology);

/* Helper: Execute CPUID instruction */
static inline void ctae_cpuid(unsigned int leaf, unsigned int subleaf,
                              unsigned int *eax, unsigned int *ebx,
                              unsigned int *ecx, unsigned int *edx)
{
	asm volatile("cpuid"
	             : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
	             : "a" (leaf), "c" (subleaf));
}

/* Extract APIC ID for a given CPU */
int ctae_get_cpu_apic_id(unsigned int cpu, unsigned int *apic_id)
{
	unsigned int eax, ebx, ecx, edx;
	
	if (!apic_id)
		return -EINVAL;
	
	/* CPUID leaf 0x1: Processor Info and Feature Bits */
	ctae_cpuid(0x1, 0, &eax, &ebx, &ecx, &edx);
	*apic_id = (ebx >> 24) & 0xFF;
	
	return 0;
}
EXPORT_SYMBOL(ctae_get_cpu_apic_id);

/* Extract CPU topology: socket, core, thread IDs */
int ctae_get_cpu_topology_info(unsigned int cpu, unsigned int *socket,
                                unsigned int *core, unsigned int *thread)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int max_leaf;
	
	if (!socket || !core || !thread)
		return -EINVAL;
	
	/* Check if extended topology enumeration is supported */
	ctae_cpuid(0x0, 0, &eax, &ebx, &ecx, &edx);
	max_leaf = eax;
	
	if (max_leaf >= 0xB) {
		/* CPUID leaf 0xB: Extended Topology Enumeration */
		/* Subleaf 0: SMT level */
		ctae_cpuid(0xB, 0, &eax, &ebx, &ecx, &edx);
		*thread = edx & 0xFF;
		
		/* Subleaf 1: Core level */
		ctae_cpuid(0xB, 1, &eax, &ebx, &ecx, &edx);
		*core = edx & 0xFF;
		
		/* Socket is derived from package ID */
		*socket = topology_physical_package_id(cpu);
	} else {
		/* Fallback: use topology macros */
		*socket = topology_physical_package_id(cpu);
		*core = topology_core_id(cpu);
		*thread = topology_sibling_cpumask(cpu) ? 
		          cpumask_weight(topology_sibling_cpumask(cpu)) : 1;
	}
	
	return 0;
}
EXPORT_SYMBOL(ctae_get_cpu_topology_info);

/* Query cache information using CPUID leaf 0x4 */
int ctae_get_cache_info(unsigned int cpu, unsigned int cache_level,
                        struct ctae_cache_info *info)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int subleaf = 0;
	unsigned int cache_type, level, sets, line_size, partitions, ways;
	
	if (!info)
		return -EINVAL;
	
	/* Iterate through cache subleaves */
	while (subleaf < 16) {
		ctae_cpuid(0x4, subleaf, &eax, &ebx, &ecx, &edx);
		
		cache_type = eax & 0x1F;
		if (cache_type == 0) /* No more caches */
			break;
		
		level = (eax >> 5) & 0x7;
		
		if (level == cache_level) {
			/* Cache parameters */
			line_size = (ebx & 0xFFF) + 1;
			partitions = ((ebx >> 12) & 0x3FF) + 1;
			ways = ((ebx >> 22) & 0x3FF) + 1;
			sets = ecx + 1;
			
			info->level = level;
			info->type = cache_type;
			info->line_size = line_size;
			info->associativity = ways;
			info->size_kb = (ways * partitions * line_size * sets) / 1024;
			info->num_sharing = ((eax >> 14) & 0xFFF) + 1;
			
			return 0;
		}
		
		subleaf++;
	}
	
	return -ENOENT;
}
EXPORT_SYMBOL(ctae_get_cache_info);

/* Create a new cache domain */
static struct ctae_cache_domain *ctae_create_cache_domain(unsigned int domain_id,
                                                          unsigned int level)
{
	struct ctae_cache_domain *domain;
	
	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain) {
		pr_err("CTAE: Failed to allocate cache domain\n");
		return NULL;
	}
	
	if (!zalloc_cpumask_var(&domain->cpu_mask, GFP_KERNEL)) {
		pr_err("CTAE: Failed to allocate CPU mask\n");
		kfree(domain);
		return NULL;
	}
	
	INIT_LIST_HEAD(&domain->list);
	INIT_LIST_HEAD(&domain->cpu_list);
	domain->domain_id = domain_id;
	domain->cache_level = level;
	domain->num_cpus = 0;
	
	return domain;
}

/* Add CPU to a cache domain */
static int ctae_add_cpu_to_domain(struct ctae_cache_domain *domain,
                                  unsigned int cpu_id)
{
	struct ctae_cpu_node *cpu_node;
	unsigned int apic_id, socket, core, thread;
	int ret;
	
	if (!domain)
		return -EINVAL;
	
	/* Allocate CPU node */
	cpu_node = kzalloc(sizeof(*cpu_node), GFP_KERNEL);
	if (!cpu_node) {
		pr_err("CTAE: Failed to allocate CPU node for CPU %u\n", cpu_id);
		return -ENOMEM;
	}
	
	/* Get CPU topology info */
	ret = ctae_get_cpu_apic_id(cpu_id, &apic_id);
	if (ret) {
		pr_warn("CTAE: Could not get APIC ID for CPU %u\n", cpu_id);
		apic_id = cpu_id;
	}
	
	ret = ctae_get_cpu_topology_info(cpu_id, &socket, &core, &thread);
	if (ret) {
		pr_warn("CTAE: Could not get topology info for CPU %u\n", cpu_id);
		socket = 0;
		core = cpu_id;
		thread = 0;
	}
	
	/* Initialize CPU node */
	INIT_LIST_HEAD(&cpu_node->list);
	cpu_node->cpu_id = cpu_id;
	cpu_node->apic_id = apic_id;
	cpu_node->socket_id = socket;
	cpu_node->core_id = core;
	cpu_node->thread_id = thread;
	cpu_node->parent_domain = domain;
	
	/* Add to domain */
	list_add_tail(&cpu_node->list, &domain->cpu_list);
	cpumask_set_cpu(cpu_id, domain->cpu_mask);
	domain->num_cpus++;
	
	return 0;
}

/* Discover L3 cache topology with multiple domains */
int ctae_discover_cache_topology(void)
{
	unsigned int cpu;
	struct ctae_cache_info cache_info;
	struct ctae_cache_domain *domain;
	unsigned int domain_id = 0;
	int ret;
	char path[128];
	struct file *filp;
	char buf[256];
	loff_t pos = 0;
	ssize_t bytes;
	unsigned int l3_size_kb = 0;
	cpumask_var_t processed_cpus;
	cpumask_var_t shared_cpus;
	bool use_sysfs = true;
	
	if (!ctae_global_topology) {
		pr_err("CTAE: Global topology not initialized\n");
		return -EINVAL;
	}
	
	if (!zalloc_cpumask_var(&processed_cpus, GFP_KERNEL)) {
		pr_err("CTAE: Failed to allocate processed_cpus mask\n");
		return -ENOMEM;
	}
	
	if (!zalloc_cpumask_var(&shared_cpus, GFP_KERNEL)) {
		pr_err("CTAE: Failed to allocate shared_cpus mask\n");
		free_cpumask_var(processed_cpus);
		return -ENOMEM;
	}
	
	pr_info("CTAE: Discovering cache topology (L3)...\n");
	
	/* Try to read L3 size from sysfs first */
	snprintf(path, sizeof(path), 
	         "/sys/devices/system/cpu/cpu0/cache/index3/size");
	
	filp = filp_open(path, O_RDONLY, 0);
	if (!IS_ERR(filp)) {
		bytes = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
		if (bytes > 0) {
			buf[bytes] = '\0';
			/* Parse size (format: "18432K" or "18M") */
			if (sscanf(buf, "%uK", &l3_size_kb) == 1) {
				pr_info("CTAE: Read L3 size from sysfs: %u KB\n", 
				        l3_size_kb);
			} else {
				unsigned int l3_size_mb;
				if (sscanf(buf, "%uM", &l3_size_mb) == 1) {
					l3_size_kb = l3_size_mb * 1024;
					pr_info("CTAE: Read L3 size from sysfs: %u MB (%u KB)\n",
					        l3_size_mb, l3_size_kb);
				}
			}
		}
		filp_close(filp, NULL);
	} else {
		use_sysfs = false;
		pr_warn("CTAE: Cannot access sysfs, will use fallback topology\n");
	}
	
	/* Fallback to CPUID if sysfs failed */
	if (l3_size_kb == 0) {
		ret = ctae_get_cache_info(0, CTAE_CACHE_L3, &cache_info);
		if (ret) {
			pr_warn("CTAE: Could not detect L3 cache, using fallback\n");
			cache_info.size_kb = 8192; /* 8MB fallback */
			cache_info.line_size = 64;
			cache_info.associativity = 16;
			cache_info.num_sharing = num_online_cpus();
		} else {
			l3_size_kb = cache_info.size_kb;
		}
	} else {
		/* We got size from sysfs, set other defaults */
		cache_info.size_kb = l3_size_kb;
		cache_info.line_size = 64;
		cache_info.associativity = 16;
		cache_info.num_sharing = num_online_cpus();
	}
	
	pr_info("CTAE: L3 Cache: %u KB, %u-way, %u B lines\n",
	        cache_info.size_kb, cache_info.associativity,
	        cache_info.line_size);
	
	/* Create multiple domains based on LLC sharing */
	if (use_sysfs) {
		/* Read shared_cpu_list for each CPU to identify LLC domains */
		for_each_online_cpu(cpu) {
			char *token, *str;
			unsigned int shared_cpu;
			
			/* Skip if already processed */
			if (cpumask_test_cpu(cpu, processed_cpus))
				continue;
			
			/* Read shared CPU list for this CPU's L3 cache */
			snprintf(path, sizeof(path),
			         "/sys/devices/system/cpu/cpu%u/cache/index3/shared_cpu_list",
			         cpu);
			
			pos = 0;
			filp = filp_open(path, O_RDONLY, 0);
			if (IS_ERR(filp)) {
				pr_warn("CTAE: Cannot read shared_cpu_list for CPU %u\n", cpu);
				continue;
			}
			
			bytes = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
			filp_close(filp, NULL);
			
			if (bytes <= 0)
				continue;
			
			buf[bytes] = '\0';
			/* Remove trailing newline */
			if (buf[bytes - 1] == '\n')
				buf[bytes - 1] = '\0';
			
			/* Parse CPU list (format: "0-3,8-11" or "0,1,2,3") */
			cpumask_clear(shared_cpus);
			str = buf;
			while ((token = strsep(&str, ",")) != NULL) {
				unsigned int start, end;
				if (sscanf(token, "%u-%u", &start, &end) == 2) {
					/* Range format */
					for (shared_cpu = start; shared_cpu <= end && shared_cpu < nr_cpu_ids; shared_cpu++) {
						cpumask_set_cpu(shared_cpu, shared_cpus);
					}
				} else if (sscanf(token, "%u", &start) == 1) {
					/* Single CPU */
					if (start < nr_cpu_ids)
						cpumask_set_cpu(start, shared_cpus);
				}
			}
			
			/* Create domain for this LLC sharing group */
			domain = ctae_create_cache_domain(domain_id++, CTAE_CACHE_L3);
			if (!domain) {
				pr_err("CTAE: Failed to create cache domain\n");
				continue;
			}
			
			domain->cache_size_kb = cache_info.size_kb;
			domain->cache_line_size = cache_info.line_size;
			domain->associativity = cache_info.associativity;
			
			/* Add all CPUs in this sharing group to the domain */
			for_each_cpu(shared_cpu, shared_cpus) {
				if (!cpu_online(shared_cpu))
					continue;
				
				ret = ctae_add_cpu_to_domain(domain, shared_cpu);
				if (ret) {
					pr_err("CTAE: Failed to add CPU %u to domain\n", shared_cpu);
					continue;
				}
				
				/* Mark as processed */
				cpumask_set_cpu(shared_cpu, processed_cpus);
			}
			
			/* Add domain to global topology */
			spin_lock(&ctae_global_topology->lock);
			list_add_tail(&domain->list, &ctae_global_topology->cache_domains);
			ctae_global_topology->num_domains++;
			spin_unlock(&ctae_global_topology->lock);
			
			pr_info("CTAE: Created domain %u with %u CPUs\n",
			        domain->domain_id, domain->num_cpus);
		}
	}
	
	/* Fallback: create per-socket domains if sysfs failed or no domains created */
	if (ctae_global_topology->num_domains == 0) {
		unsigned int socket;
		cpumask_var_t socket_cpus;
		
		pr_info("CTAE: Using per-socket fallback topology\n");
		
		if (!zalloc_cpumask_var(&socket_cpus, GFP_KERNEL)) {
			pr_err("CTAE: Failed to allocate socket_cpus mask\n");
			free_cpumask_var(processed_cpus);
			free_cpumask_var(shared_cpus);
			return -ENOMEM;
		}
		
		cpumask_clear(processed_cpus);
		
		for_each_online_cpu(cpu) {
			if (cpumask_test_cpu(cpu, processed_cpus))
				continue;
			
			socket = topology_physical_package_id(cpu);
			cpumask_clear(socket_cpus);
			
			/* Find all CPUs in this socket */
			for_each_online_cpu(shared_cpu) {
				if (topology_physical_package_id(shared_cpu) == socket) {
					cpumask_set_cpu(shared_cpu, socket_cpus);
					cpumask_set_cpu(shared_cpu, processed_cpus);
				}
			}
			
			/* Create domain for this socket */
			domain = ctae_create_cache_domain(domain_id++, CTAE_CACHE_L3);
			if (!domain)
				continue;
			
			domain->cache_size_kb = cache_info.size_kb;
			domain->cache_line_size = cache_info.line_size;
			domain->associativity = cache_info.associativity;
			
			for_each_cpu(shared_cpu, socket_cpus) {
				ret = ctae_add_cpu_to_domain(domain, shared_cpu);
				if (ret)
					continue;
			}
			
			spin_lock(&ctae_global_topology->lock);
			list_add_tail(&domain->list, &ctae_global_topology->cache_domains);
			ctae_global_topology->num_domains++;
			spin_unlock(&ctae_global_topology->lock);
			
			pr_info("CTAE: Created socket domain %u with %u CPUs\n",
			        domain->domain_id, domain->num_cpus);
		}
		
		free_cpumask_var(socket_cpus);
	}
	
	/* Final fallback: if still only 1 domain, split it in half */
	if (ctae_global_topology->num_domains == 1) {
		unsigned int half = num_online_cpus() / 2;
		unsigned int count = 0;
		
		pr_warn("CTAE: Only 1 domain detected, creating artificial split for migration testing\n");
		
		domain = ctae_create_cache_domain(domain_id++, CTAE_CACHE_L3);
		if (domain) {
			domain->cache_size_kb = cache_info.size_kb / 2;
			domain->cache_line_size = cache_info.line_size;
			domain->associativity = cache_info.associativity;
			
			for_each_online_cpu(cpu) {
				if (count++ >= half) {
					ret = ctae_add_cpu_to_domain(domain, cpu);
					if (ret)
						continue;
				}
			}
			
			spin_lock(&ctae_global_topology->lock);
			list_add_tail(&domain->list, &ctae_global_topology->cache_domains);
			ctae_global_topology->num_domains++;
			spin_unlock(&ctae_global_topology->lock);
			
			pr_info("CTAE: Created artificial domain %u with %u CPUs\n",
			        domain->domain_id, domain->num_cpus);
		}
	}
	
	ctae_global_topology->num_cpus = num_online_cpus();
	ctae_global_topology->initialized = true;
	
	free_cpumask_var(processed_cpus);
	free_cpumask_var(shared_cpus);
	
	pr_info("CTAE: Topology discovery complete: %u domains, %u CPUs\n",
	        ctae_global_topology->num_domains,
	        ctae_global_topology->num_cpus);
	
	return 0;
}

EXPORT_SYMBOL(ctae_discover_cache_topology);

/* Print topology to kernel log */
void ctae_print_topology(void)
{
	struct ctae_cache_domain *domain;
	struct ctae_cpu_node *cpu_node;
	
	if (!ctae_global_topology || !ctae_global_topology->initialized) {
		pr_info("CTAE: Topology not initialized\n");
		return;
	}
	
	pr_info("CTAE: ========== Cache Topology ==========\n");
	pr_info("CTAE: Total Domains: %u\n", ctae_global_topology->num_domains);
	pr_info("CTAE: Total CPUs: %u\n", ctae_global_topology->num_cpus);
	
	spin_lock(&ctae_global_topology->lock);
	list_for_each_entry(domain, &ctae_global_topology->cache_domains, list) {
		pr_info("CTAE: Domain %u (L%u): %u KB, %u CPUs\n",
		        domain->domain_id, domain->cache_level,
		        domain->cache_size_kb, domain->num_cpus);
		
		list_for_each_entry(cpu_node, &domain->cpu_list, list) {
			pr_info("CTAE:   CPU %u: Socket=%u Core=%u Thread=%u APIC=%u\n",
			        cpu_node->cpu_id, cpu_node->socket_id,
			        cpu_node->core_id, cpu_node->thread_id,
			        cpu_node->apic_id);
		}
	}
	spin_unlock(&ctae_global_topology->lock);
	
	pr_info("CTAE: ====================================\n");
}
EXPORT_SYMBOL(ctae_print_topology);

/* Find cache domain by CPU ID */
struct ctae_cache_domain *ctae_find_domain_by_cpu(unsigned int cpu_id)
{
	struct ctae_cache_domain *domain;
	
	if (!ctae_global_topology || !ctae_global_topology->initialized)
		return NULL;
	
	spin_lock(&ctae_global_topology->lock);
	list_for_each_entry(domain, &ctae_global_topology->cache_domains, list) {
		if (cpumask_test_cpu(cpu_id, domain->cpu_mask)) {
			spin_unlock(&ctae_global_topology->lock);
			return domain;
		}
	}
	spin_unlock(&ctae_global_topology->lock);
	
	return NULL;
}
EXPORT_SYMBOL(ctae_find_domain_by_cpu);

/* Find cache domain by domain ID */
struct ctae_cache_domain *ctae_find_domain_by_id(unsigned int domain_id)
{
	struct ctae_cache_domain *domain;
	
	if (!ctae_global_topology || !ctae_global_topology->initialized)
		return NULL;
	
	spin_lock(&ctae_global_topology->lock);
	list_for_each_entry(domain, &ctae_global_topology->cache_domains, list) {
		if (domain->domain_id == domain_id) {
			spin_unlock(&ctae_global_topology->lock);
			return domain;
		}
	}
	spin_unlock(&ctae_global_topology->lock);
	
	return NULL;
}
EXPORT_SYMBOL(ctae_find_domain_by_id);

/* Initialize topology structure */
int ctae_topology_init(void)
{
	ctae_global_topology = kzalloc(sizeof(*ctae_global_topology), GFP_KERNEL);
	if (!ctae_global_topology) {
		pr_err("CTAE: Failed to allocate global topology\n");
		return -ENOMEM;
	}
	
	INIT_LIST_HEAD(&ctae_global_topology->cache_domains);
	spin_lock_init(&ctae_global_topology->lock);
	ctae_global_topology->num_domains = 0;
	ctae_global_topology->num_cpus = 0;
	ctae_global_topology->initialized = false;
	
	pr_info("CTAE: Topology structure initialized\n");
	return 0;
}

/* Cleanup topology structure */
void ctae_topology_cleanup(void)
{
	struct ctae_cache_domain *domain, *tmp_domain;
	struct ctae_cpu_node *cpu_node, *tmp_cpu;
	
	if (!ctae_global_topology)
		return;
	
	pr_info("CTAE: Cleaning up topology...\n");
	
	spin_lock(&ctae_global_topology->lock);
	
	/* Free all domains and their CPU nodes */
	list_for_each_entry_safe(domain, tmp_domain,
	                         &ctae_global_topology->cache_domains, list) {
		/* Free CPU nodes */
		list_for_each_entry_safe(cpu_node, tmp_cpu,
		                         &domain->cpu_list, list) {
			list_del(&cpu_node->list);
			kfree(cpu_node);
		}
		
		/* Free domain */
		list_del(&domain->list);
		free_cpumask_var(domain->cpu_mask);
		kfree(domain);
	}
	
	spin_unlock(&ctae_global_topology->lock);
	
	kfree(ctae_global_topology);
	ctae_global_topology = NULL;
	
	pr_info("CTAE: Topology cleanup complete\n");
}

/* Module initialization */
static int __init ctae_core_init(void)
{
	int ret;
	
	pr_info("CTAE: Initializing Cache-Topology-Aware Execution Engine\n");
	pr_info("CTAE: Version %s\n", CTAE_VERSION);
	pr_info("CTAE: Kernel %s on %s\n", init_uts_ns.name.release, 
	        init_uts_ns.name.machine);
	
	/* Initialize topology */
	ret = ctae_topology_init();
	if (ret) {
		pr_err("CTAE: Topology initialization failed\n");
		return ret;
	}
	
	/* Discover cache topology */
	ret = ctae_discover_cache_topology();
	if (ret) {
		pr_err("CTAE: Cache topology discovery failed\n");
		ctae_topology_cleanup();
		return ret;
	}
	
	/* Print discovered topology */
	ctae_print_topology();
	
	pr_info("CTAE: Core module loaded successfully\n");
	return 0;
}

/* Module cleanup */
static void __exit ctae_core_exit(void)
{
	pr_info("CTAE: Unloading core module...\n");
	
	ctae_topology_cleanup();
	
	pr_info("CTAE: Core module unloaded\n");
}

module_init(ctae_core_init);
module_exit(ctae_core_exit);