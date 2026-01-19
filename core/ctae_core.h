#ifndef CTAE_CORE_H
#define CTAE_CORE_H

#include <linux/types.h>
#include <linux/cpumask.h>

/* Complete definition of the struct */
struct ctae_l3_domain {
    cpumask_t cpus;
    int id;
};

/* Extern declaration */
extern struct ctae_l3_domain ctae_global_domain;

#endif /* CTAE_CORE_H */