/*
 * SPDX-License-Identifier: GPL-2.0
 * Memory Aware CPU Frequency Manager (MACFM) is to control
 * CPU frequency based on memory architecture awareness via PMU
 * events.
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#include "linux/kernel.h"
#include "linux/permut.h"
#include <linux/macfm.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include "sched.h"
#include "../time/tick-sched.h"

// Main MACFM models-functions

unsigned int macfm_calc_optimal_freq(const struct macfm_info *info)
{
	u64 cycles_nomem_scaled, cycles_max_scaled;

	cycles_nomem_scaled =


	return 0;
}

static u64 macfm_calc_mem_load(const struct macfm_info *info,
			       const struct macfm_pmu_stats *pmu_stats)
{
	const struct macfm_stats *stats;
	u64 cycles_mem_scaled;

	stats = &(info->stats[0]);
	cycles_mem_scaled = (stats->mem_parallelism *
		(MACFM_L2_REFILL_WEIGHT * (pmu_stats->l3_refills - pmu_stats->l2_refills) +
		 MACFM_L3_REFILL_WEIGHT * pmu_stats->l3_refills));

	return cycles_mem_scaled;
}

static void macfm_extract_pmu_stats(struct permut_pinned_events *event_vals, int cpu,
				    struct macfm_pmu_stats *pmu_stats)
{
	u64 instrs, cpu_cycles, l2_refills, l3_refills;
	instrs = permut_pinned_data_of_event(event_vals, cpu,
					ARMV8_PMUV3_PERFCTR_INST_RETIRED);
	cpu_cycles = permut_pinned_data_of_event(event_vals, cpu,
					ARMV8_PMUV3_PERFCTR_CPU_CYCLES);
	l2_refills = permut_pinned_data_of_event(event_vals, cpu,
					ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL);
	l3_refills = permut_pinned_data_of_event(event_vals, cpu,
					ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL);

	pmu_stats->instrs = instrs;
	pmu_stats->cpu_cycles = cpu_cycles;
	pmu_stats->l2_refills = l2_refills;
	pmu_stats->l3_refills = l3_refills;
}

static void __macfm_update_pmu_stats(struct macfm_pmu_stats *pmu_stats,
				     struct macfm_pmu_stats *delta)
{
	pmu_stats->instrs += delta->instrs;
	pmu_stats->cpu_cycles += delta->cpu_cycles;
	pmu_stats->l2_refills += delta->l2_refills;
	pmu_stats->l3_refills += delta->l3_refills;
}

static void __macfm_update_util_info(struct macfm_info *info,
				     struct macfm_pmu_stats *pmu_delta, u64 now)
{
	u64 cycles_nomem_scaled, cycles_mem_scaled, cycles_mem_max_scaled, mem_load;
	u64 cycles_scaled, cycles_max_scaled;
	struct macfm_util_info *util;
	struct macfm_stats *stats;
	u64 instrs_max_possible;

	stats = &(info->stats[0]);
	util = &(stats->util_info);

	mem_load = macfm_calc_mem_load(info, pmu_delta);

	cycles_mem_scaled = (mem_load * info->cpufreq_curr) / MACFM_SCALE_VALUE_X1;
	cycles_mem_max_scaled = (mem_load * info->cpufreq_max) / MACFM_SCALE_VALUE_X1;

	cycles_scaled = pmu_delta->cpu_cycles * MACFM_SCALE_VALUE_X2;
	cycles_nomem_scaled = cycles_scaled - cycles_mem_scaled;

	cycles_max_scaled = cycles_nomem_scaled + cycles_mem_max_scaled;
	instrs_max_possible = (pmu_delta->instrs * cycles_max_scaled) / cycles_scaled;

	stats->cycles_nomem_scaled += cycles_nomem_scaled;

	util->instrs += pmu_delta->instrs;
	util->instrs_max_possible += instrs_max_possible;

	util->runtime += now - info->last_update_time;
	info->last_update_time = now;

	util->cputime += (pmu_delta->cpu_cycles * MACFM_SCALE_VALUE_FREQ) / info->cpufreq_curr;

	util->util = (util->cputime * util->instrs * MACFM_SCALE_VALUE_X2) /
		     (util->runtime * util->instrs_max_possible);

	pr_warn("%s: cycles_scaled=%llu, cycles_nomem_scaled=%llu, cycles_max_scaled=%llu, "
		"cpufreq=%u, util=%llu, cputime=%llu, runtime=%llu\n",
		__func__, cycles_scaled, cycles_nomem_scaled, cycles_max_scaled,
		info->cpufreq_curr, util->util, util->cputime, util->runtime);
}

static void macfm_update_task_info(struct task_struct *p,
				   struct macfm_pmu_stats *pmu_delta, u64 now)
{
	struct macfm_pmu_stats *task_stats;
	struct macfm_info *task_info;

	task_info = p->macfm_info;
	if (!task_info)
		return;

	task_stats = &(task_info->stats[0].pmu_counters);

	__macfm_update_pmu_stats(task_stats, pmu_delta);
	__macfm_update_util_info(task_info, pmu_delta, now);
}

static void macfm_update_rq_info(struct rq *rq,
				 struct macfm_pmu_stats *pmu_delta, u64 now)
{
	struct macfm_pmu_stats *rq_stats;
	struct macfm_info *rq_info;

	rq_info = &(rq->macfm_info);
	rq_stats = &(rq_info->stats[0].pmu_counters);

	__macfm_update_pmu_stats(rq_stats, pmu_delta);
	__macfm_update_util_info(rq_info, pmu_delta, now);
}

static void macfm_update_history(struct macfm_info *info, int n_windows)
{
	int i;

	/* History if very old, start from scratch */
	if (n_windows >= MACFM_DEFAULT_WINDOWS_NUM) {
		memset(info->stats, 0, sizeof(info->stats));
		info->stats[0].mem_parallelism = MACFM_DEFAULT_MEM_PARALLELISM;
	} else {
		for (i = sysctl_macfm_windows_num - 1; i >= n_windows; i--)
			info->stats[i] = info->stats[i - n_windows];

		memset(info->stats, 0, n_windows * sizeof(info->stats[0]));

		// TODO: add ewma value for long-history restoring
		info->stats[0].mem_parallelism = info->stats[n_windows].mem_parallelism;
		// info->stats[0].cycles_nomem_scaled = info->stats[n_windows].cycles_nomem_scaled;
	}
}

static int macfm_update_window_start(u64 *window_start, u64 window_size, u64 now)
{
	int n_windows;
	s64 delta;

	delta = now - *window_start;
	if (delta < 0)
		delta = 0;

	if (delta < window_size)
		return 0;

	n_windows = div64_u64(delta, window_size);
	*window_start += n_windows * window_size;

	return n_windows;
}

static void get_current_cpufreq_info(int cpu, unsigned int *cpufreq_curr, unsigned int *cpufreq_max)
{
	unsigned int new_cpufreq;
	unsigned int new_maxfreq;

	new_cpufreq = cpufreq_get(cpu);
	new_maxfreq = cpufreq_quick_get_max(cpu);

	*cpufreq_curr = new_cpufreq;
	*cpufreq_max = new_maxfreq;
}

static void macfm_update_freq_info(struct task_struct *p, struct rq *rq)
{
	unsigned int new_cpufreq;
	unsigned int new_maxfreq;
	int cpu;

	cpu = rq->cpu;
	get_current_cpufreq_info(cpu, &new_cpufreq, &new_maxfreq);

	rq->macfm_info.cpufreq_curr = new_cpufreq;
	rq->macfm_info.cpufreq_max = new_maxfreq;

	p->macfm_info->cpufreq_curr = new_cpufreq;
	p->macfm_info->cpufreq_max = new_maxfreq;
}

static void macfm_update_window(struct task_struct *p, struct rq *rq, u64 now, int flags)
{
	u64 window_start;
	int n_windows;
	int cpu;

	if (!p->macfm_info)
                return;

	cpu = rq->cpu;
	window_start = rq->macfm_info.window_start;
        n_windows = macfm_update_window_start(&window_start, MACFM_WINDOW_SIZE, now);
	if (n_windows > 0) {
		rq->macfm_info.window_start = window_start;
		macfm_update_history(&(rq->macfm_info), n_windows);
	}

	window_start = p->macfm_info->window_start;
	n_windows = macfm_update_window_start(&window_start, MACFM_WINDOW_SIZE, now);
	if (n_windows > 0) {
		p->macfm_info->window_start = window_start;
		macfm_update_history(p->macfm_info, n_windows);
	}
}

void permut_account_macfm(struct task_struct *prev_task, struct task_struct *next_task,
			  int cpu, struct permut_event_count *delta,
			  unsigned long flags)
{
	struct macfm_pmu_stats pmu_stats;
	struct rq *rq;
	u64 now;

	if (!sysctl_macfm_enable)
		return;

	rq = cpu_rq(cpu);
	now = sched_clock();

	if (!is_idle_task(next_task) && (flags & PERMUT_TASK_SWITCH))
		macfm_update_freq_info(next_task, rq);

	if (!is_idle_task(prev_task)) {
		macfm_extract_pmu_stats(&delta->pinned_data, cpu, &pmu_stats);
		macfm_update_task_info(prev_task, &pmu_stats, now);
		macfm_update_rq_info(rq, &pmu_stats, now);
	}

	macfm_update_window(next_task, rq, now, flags);
	if (flags & PERMUT_PERF_CHANGE)
		macfm_update_freq_info(next_task, rq);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////

// static u16 ow_fixup(s64 runtime, bool new_window_start, u16 *overload_windows)
// {
// 	if (new_window_start && runtime > 0) {
// 		if (runtime * 100 >= MACFM_WINDOW_SIZE * macfm_runtime_overload_percent)
// 			++*overload_windows;
// 		else
// 			*overload_windows = 0;
// 	}

// 	/* Prevent overflow */
// 	*overload_windows = min(*overload_windows, MAX_OVERLOAD_WINDOWS);
// 	return *overload_windows;
// }

//////////////////////////////////////////////////////////////////////////////////////

void macfm_migrate_update(struct task_struct *p, int cpu)
{
	struct task_struct *src_task;
	int src_cpu;

	if (!sysctl_macfm_enable)
		return;

	src_cpu = task_cpu(p);
	src_task = cpu_rq(src_cpu)->curr;

	// pr_warn("S_MIGR==========================================\n");
	// pr_warn("%s: cpu%d: domain=%d, cpu%d: domain=%d\n",
	// 	__func__, cpu, topology_physical_package_id(cpu),
	// 	src_cpu, topology_physical_package_id(src_cpu));

	// if (topology_physical_package_id(cpu) !=
	//     topology_physical_package_id(src_cpu)) {
	// 	macfm_load_pred_freq(src_cpu, src_task, MDEAS_MIGRATE);
	// 	macfm_check_freq_update(src_cpu, MDEAS_MIGRATE);

	// 	macfm_load_pred_freq(cpu, p, MDEAS_MIGRATE);
	// 	macfm_check_freq_update(cpu, MDEAS_MIGRATE);
	// }
	// pr_warn("E_MIGR==========================================\n");
}

void macfm_tick_update(struct task_struct *p, int cpu)
{
	if (!sysctl_macfm_enable)
		return;

	// pr_warn("S_TICK==========================================\n");
	// pr_warn("%s: macfm tick for cpu=%d, pid=%d\n",
	// 	__func__, cpu, p->pid);

	// macfm_load_pred_freq(cpu, p, MDEAS_UPDATE);
	// macfm_check_freq_update(cpu, MDEAS_UPDATE);
	// pr_warn("E_TICK==========================================\n");
}

void macfm_first_running_task_update(struct task_struct *p)
{
	unsigned long flags;
	int cpu;

	if (!sysctl_macfm_enable)
		return;

	// pr_warn("S_1st==========================================\n");
	// pr_warn("%s: macfm 1st running task update [pid=%d]",
	// 	__func__, p->pid);

	local_irq_save(flags);
	cpu = smp_processor_id();

	// macfm_load_pred_freq(cpu, p, MDEAS_MIGRATE);
	// macfm_check_freq_update(cpu, MDEAS_UPDATE);

	local_irq_restore(flags);
	// pr_warn("E_1st==========================================\n");
}

//////////////////////////////////////////////////////////////////////////////////////

void macfm_set_window_start(void *info)
{
	ktime_t start, now, delta;
	u64 n_windows;
	struct rq *rq;

	start = tick_init_jiffy_update();
	now = ktime_get();
	rq = this_rq();

	delta = now - start;
	BUG_ON(delta < 0);

	n_windows = delta / MACFM_WINDOW_SIZE;
	rq->macfm_info.window_start = start + n_windows * MACFM_WINDOW_SIZE;
}

void init_macfm_info(struct macfm_info *macfm_info)
{
	int i;

	memset(macfm_info, 0, sizeof(*macfm_info));
	for (i = 0; i < MACFM_DEFAULT_WINDOWS_NUM; i++)
		macfm_info->stats[i].mem_parallelism = MACFM_DEFAULT_MEM_PARALLELISM;
}

int macfm_fork(struct task_struct *p)
{
	struct macfm_info *macfm_info;
	ktime_t start, now, delta;
	u64 n_windows;

	start = tick_init_jiffy_update();
	now = ktime_get();
	delta = now - start;
	n_windows = delta / MACFM_WINDOW_SIZE;

	macfm_info = kzalloc(sizeof(*macfm_info), GFP_KERNEL);
	if (!macfm_info)
		return -ENOMEM;

	init_macfm_info(macfm_info);
	macfm_info->window_start = start + n_windows * MACFM_WINDOW_SIZE;

	p->macfm_info = macfm_info;

	return 0;
}

void macfm_free(struct task_struct *p)
{
	if (p->macfm_info) {
		kfree(p->macfm_info);
		p->macfm_info = NULL;
	}
}

int macfm_start(void)
{
	int ret;

	ret = set_permut_state(false, NULL);
	if (ret < 0)
		return ret;

	sysctl_permut_exclude_hv = 0;
	sysctl_permut_exclude_idle = 0;
	sysctl_permut_exclude_kernel = 0;

	ret = set_permut_state(true, cpu_possible_mask);
	if (ret == 0)
		sysctl_macfm_enable = true;

	return ret;
}

int macfm_stop(void)
{
	sysctl_macfm_enable = false;
	return set_permut_state(false, NULL);
}
