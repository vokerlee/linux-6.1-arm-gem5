/*
 * SPDX-License-Identifier: GPL-2.0
 * Memory Aware CPU Frequency Manager (MACFM) is to control
 * CPU frequency based on memory architecture awareness via PMU
 * events.
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#ifndef _LINUX_MACFM_H
#define _LINUX_MACFM_H

#include <linux/types.h>
#include <linux/permut.h>

#ifdef CONFIG_MACFM

#define MACFM_DEFAULT_WINDOWS_NUM	4U
#define MACFM_WINDOW_SIZE		(1000000000ULL / (HZ))

#define MACFM_DEFAULT_MEM_PARALLELISM	1024ULL

#define MACFM_SCALE_VALUE_X1		1024ULL
#define MACFM_SCALE_VALUE_X2		(MACFM_SCALE_VALUE_X1 * MACFM_SCALE_VALUE_X1)
#define MACFM_SCALE_VALUE_X3		(MACFM_SCALE_VALUE_X2 * MACFM_SCALE_VALUE_X1)
#define MACFM_SCALE_VALUE_FREQ		1000000UL

#define MACFM_L2_REFILL_WEIGHT		34ULL
#define MACFM_L3_REFILL_WEIGHT		129ULL

#define MACFM_UPDATE			(1 << 0)
#define MACFM_MIGRATE			(1 << 1)
#define MACFM_POLICY_MIN_RESTORE	(1 << 2)

extern unsigned int sysctl_macfm_enable;
extern unsigned int sysctl_macfm_windows_num;

struct macfm_pmu_stats {
	u64 instrs;
	u64 cpu_cycles;
	u64 l2_refills;
	u64 l3_refills;
};

struct macfm_util_info {
	u64 instrs;
	u64 instrs_max_possible; // maximum CPU frequency

	u64 runtime;
	u64 cputime;
	u64 util;
};

struct macfm_stats {
	struct macfm_pmu_stats pmu_counters;
	struct macfm_util_info util_info;

	u64 mem_parallelism;
	u64 cycles_nomem_scaled; // without L2/L3 caches & DDR
};

struct macfm_info {
	u64 window_start;
	u64 last_update_time;

	unsigned int cpufreq_curr;
	unsigned int cpufreq_max;

	struct macfm_stats stats[MACFM_DEFAULT_WINDOWS_NUM];
};

// int macfm_calc_optimal_freq(struct macfmgov_cpu *mg_cpu, int cpu,
// 			    struct task_struct *p, int flags,
// 			    unsigned int *cpufreq);

void macfm_migrate_update(struct task_struct *p, int cpu);
void macfm_tick_update(struct task_struct *p, int cpu);
void macfm_first_running_task_update(struct task_struct *p);

void macfm_set_window_start(void *info);

void macfm_free(struct task_struct *p);
int macfm_fork(struct task_struct *p);

void init_macfm_info(struct macfm_info *macfm_info);

int macfm_start(void);
int macfm_stop(void);

#else /* CONFIG_MACFM */

static inline void macfm_migrate_update(struct task_struct *p, int cpu)
{
}

static inline void macfm_tick_update(struct task_struct *p, int cpu)
{
}

static inline void macfm_first_running_task_update(struct task_struct *p)
{
}

#endif /* CONFIG_MACFM */

#endif /* _LINUX_MACFM_H */
