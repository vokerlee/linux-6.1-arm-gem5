// SPDX-License-Identifier: GPL-2.0
/*
 * Performance monitoring unit tracking (PerMUT)
 * PerMut is aimed to task permutation between CPUs &
 * frequency control
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#ifndef _LINUX_PERMUT_H
#define _LINUX_PERMUT_H

#include <linux/sched.h>
#include <asm/perf_event.h>

#define EVENT_TYPE_FLEX			0
#define EVENT_TYPE_PINNED		1

#define NR_PERMUT_PINNED_EVENTS		16
#define NR_PERMUT_FLEX_EVENTS		16

#define PERMUT_EVENT_GROUP_TERM		-1
#define PERMUT_EVENT_FINAL_TERM		-2

#define PERMUT_EVENT_OVERFLOW		(~0ULL)
#define PERMUT_TASK_SWITCH		1 << 0
#define PERMUT_TASK_TICK		1 << 1
#define PERMUT_PREV_MODE		1 << 2
#define PERMUT_CONTEXT_ROTATE           1 << 3

#define PERMUT_SCALE_VALUE		1024

#define __for_each_permut_event(index, events, __nr_events)       \
	for (index = 0; events != NULL && index < __nr_events &&  \
		events[index] != PERMUT_EVENT_FINAL_TERM; index++)

#define for_each_permut_pinned_event(index, events) \
	__for_each_permut_event(index, events, NR_PERMUT_PINNED_EVENTS)

#define for_each_permut_flex_event(index, events) \
	__for_each_permut_event(index, events, NR_PERMUT_FLEX_EVENTS)

DECLARE_STATIC_KEY_FALSE(sched_permut);

extern struct cpumask sysctl_permut_cpumask;

extern unsigned int sysctl_permut_exclude_hv;
extern unsigned int sysctl_permut_exclude_idle;
extern unsigned int sysctl_permut_exclude_kernel;

extern int sysctl_permut_pinned_events[NR_PERMUT_PINNED_EVENTS];
extern int sysctl_permut_flex_events[NR_PERMUT_FLEX_EVENTS];

struct permut_pinned_events {
	u64 data[NR_PERMUT_PINNED_EVENTS];
};

struct permut_flex_events {
	u64 data[NR_PERMUT_FLEX_EVENTS];
};

struct permut_event_count {
	struct permut_pinned_events pinned_data;
	struct permut_flex_events flex_data;
};

typedef struct permut_pinned_events permut_counter_window;

static inline struct permut_pinned_events *permut_window_counter(permut_counter_window *window)
{
	return window;
}

struct permut_info {
	permut_counter_window window;

	u64 last_update_time;

	bool valid;
	bool new_task;
};

#ifdef CONFIG_PERMUT

void permut_account_task(struct task_struct *prev_task, struct task_struct *next_task,
			 int cpu, unsigned long flags);

int permut_fork(struct task_struct *p);
void permut_free(struct task_struct *p);

void permut_perf_release(const struct cpumask *cpus);
int permut_perf_create(int *pinned_event_ids, int *flex_events_ids,
		       const struct cpumask *cpus);

void permut_clear_pinned_events(struct permut_pinned_events *term);

void permut_calc_events_delta(struct permut_event_count *term_l,
			      struct permut_event_count *term_r,
			      struct permut_event_count *delta);

void permut_assign_pinned_events(struct permut_pinned_events *term_l,
				 struct permut_pinned_events *term_r);

void permut_clear_cpu_count(int cpu);

void permut_read_cpu_events(int cpu, u64 *pinned_data, u64 *flex_data);
u64 permut_read_cpu_flex_event(int cpu, int event_id);
u64 permut_read_cpu_pinned_event(int cpu, int event_id);
u64 permut_read_cpu_counter(int cpu, int index, int pinned);

int *permut_get_pinned_event_ids(void);
int *permut_get_flex_event_ids(void);

static inline bool permut_task_valid(struct task_struct *p)
{
	return p->permut_info->valid;
}

static inline bool permut_task_new(struct task_struct *p)
{
	return p->permut_info->new_task;
}

static inline int permut_create_one_cpu(int cpu)
{
	return permut_perf_create(sysctl_permut_pinned_events,
				  sysctl_permut_flex_events, cpumask_of(cpu));
}

static inline void permut_release_one_cpu(int cpu)
{
	permut_perf_release(cpumask_of(cpu));
}

static inline int permut_create(void)
{
	struct cpumask online_permut_cpumask;
	int err;

	cpumask_and(&online_permut_cpumask, &sysctl_permut_cpumask, cpu_online_mask);
	err = permut_perf_create(sysctl_permut_pinned_events,
				 sysctl_permut_flex_events, &online_permut_cpumask);
	return err;
}

static inline void permut_release(void)
{
	struct cpumask online_permut_cpumask;

	cpumask_and(&online_permut_cpumask, &sysctl_permut_cpumask, cpu_online_mask);
	permut_perf_release(&online_permut_cpumask);
}

#else

static inline bool permut_task_valid(struct task_struct __maybe_unused *p)
{
	return 0;
}

static inline bool permut_task_new(struct task_struct __maybe_unused *p)
{
	return 0;
}

static inline int permut_create_one_cpu(int __maybe_unused cpu)
{
	return 0;
}

static inline void permut_release_one_cpu(int __maybe_unused cpu)
{
}

static inline int permut_create(void)
{
	return 0;
}

static inline void permut_release(void)
{
}

static void permut_account_task(struct task_struct *prev_task, struct task_struct *next_task,
				int cpu, unsigned long flags)
{
}

#endif /* CONFIG_PERMUT */

#endif /* _LINUX_PERMUT_H */
