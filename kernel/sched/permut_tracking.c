// SPDX-License-Identifier: GPL-2.0
/*
 * Performance monitoring unit tracking (PerMUT)
 * PerMut is aimed to task permutation between CPUs &
 * frequency control
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/sched/task.h>
#include <linux/permut.h>
#include "sched.h"

static DEFINE_PER_CPU(struct permut_event_count, prev_permut_event_count);
static DEFINE_PER_CPU(struct permut_event_count, curr_permut_event_count);
static DEFINE_PER_CPU(u64, permut_last_update_time);

static inline struct permut_event_count *permut_read_prev(int cpu)
{
	return &per_cpu(prev_permut_event_count, cpu);
}

static inline struct permut_event_count *permut_read_curr(int cpu)
{
	struct permut_event_count *curr = &per_cpu(curr_permut_event_count, cpu);
	permut_read_cpu_events(cpu, curr->pinned_data.data, curr->flex_data.data);

	return curr;
}

void permut_clear_cpu_count(int cpu)
{
	struct permut_event_count *curr = &per_cpu(curr_permut_event_count, cpu);
	struct permut_event_count *prev = &per_cpu(prev_permut_event_count, cpu);

	permut_clear_pinned_events(&curr->pinned_data);
	permut_clear_pinned_events(&prev->pinned_data);
}

static inline void permut_clear_count_window(permut_counter_window *window)
{
	struct permut_pinned_events *counter = permut_window_counter(window);
	permut_clear_pinned_events(counter);
}

static inline void permut_save_count_window(struct permut_info *info,
					    struct permut_pinned_events *delta,
					    u64 __maybe_unused delta_exec)
{
	struct permut_pinned_events *counter = permut_window_counter(&info->window);
	permut_assign_pinned_events(counter, delta);
}

int permut_fork(struct task_struct *p)
{
	struct permut_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->new_task = true;
	permut_clear_count_window(&info->window);

	p->permut_info = info;

	return 0;
}

void permut_free(struct task_struct *p)
{
	if (p->permut_info) {
		kfree(p->permut_info);
		p->permut_info = NULL;
	}
}

static void __permut_account_task(struct task_struct *p, unsigned int cpu,
				  struct permut_event_count *delta, unsigned long flags)
{
	struct permut_info *info = p->permut_info;
	u64 *task_update_time;
	u64 *cpu_update_time;
	u64 delta_exec;
	struct rq *rq;
	u64 now;

	rq = cpu_rq(cpu);
	now = rq_clock_task(rq);

	if (!info || is_idle_task(p))
		return;

	task_update_time = &info->last_update_time;
	cpu_update_time = &per_cpu(permut_last_update_time, cpu);
	if (now < *task_update_time)
		return;

	delta_exec = now - *cpu_update_time;

	*task_update_time = now;
	*cpu_update_time = now;

	if (!fair_policy(p->policy))
		return;

	permut_save_count_window(info, &delta->pinned_data, delta_exec);

	if (!info->valid)
		info->valid = true;
}

void permut_account_task(struct task_struct *prev_task, struct task_struct *next_task,
			 int cpu, unsigned long flags)
{
	struct permut_event_count next_delta;
	struct permut_event_count delta;
	struct permut_event_count *prev;
	struct permut_event_count *curr;

	if (!static_branch_likely(&sched_permut))
		return;

	if (!cpumask_test_cpu(cpu, &sysctl_permut_cpumask))
		return;

	prev = permut_read_prev(cpu);
	curr = permut_read_curr(cpu);
	permut_calc_events_delta(prev, curr, &delta);
	*prev = *curr;

	memset(&next_delta, 0, sizeof(next_delta));

	/* Account here all other things */

	if (prev_task == next_task) {
		/* usually triggered by sched_tick() */
		__permut_account_task(prev_task, cpu, &delta, flags);
	} else {
		__permut_account_task(prev_task, cpu, &delta, PERMUT_PREV_MODE | PERMUT_TASK_SWITCH);
		__permut_account_task(next_task, cpu, &next_delta, PERMUT_TASK_SWITCH);
	}
}
