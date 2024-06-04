// SPDX-License-Identifier: GPL-2.0
/*
 * Performance monitoring unit tracking (PerMUT)
 * PerMut is aimed to task permutation between CPUs &
 * frequency control
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#include <linux/gfp_types.h>
#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/percpu-defs.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/permut.h>
#include <uapi/linux/sched/types.h>

int *permut_pinned_event_ids = NULL;
int *permut_flex_event_ids = NULL;

struct permut_cpu_info {
	struct task_struct *create_thread;
	struct task_struct *release_thread;

	struct kthread_worker create_worker;
	struct kthread_worker release_worker;

	struct kthread_work create_work;
	struct kthread_work release_work;
};

struct permut_cpu_work_info {
	struct completion completion;
	atomic_t nr_todo; /* number of tasks left to complete */
	int ret;
};

static struct permut_cpu_work_info create_work_info;
static struct permut_cpu_work_info release_work_info;

static DEFINE_PER_CPU(struct permut_cpu_info, permut_cpu);
static DEFINE_PER_CPU(__typeof__(struct perf_event *)[NR_PERMUT_PINNED_EVENTS],
	permut_pinned_perf_events);
static DEFINE_PER_CPU(__typeof__(struct perf_event *)[NR_PERMUT_FLEX_EVENTS],
	permut_flex_perf_events);

static inline struct perf_event **perf_events_of_cpu(int cpu, int pinned)
{
	return pinned ? per_cpu(permut_pinned_perf_events, cpu) :
			per_cpu(permut_flex_perf_events, cpu);
}

static inline struct perf_event *perf_event_of_cpu(int cpu, int index, int pinned)
{
	struct perf_event **events = perf_events_of_cpu(cpu, pinned);
	return events[index];
}

int *permut_get_pinned_event_ids(void)
{
	return permut_pinned_event_ids;
}

int *permut_get_flex_event_ids(void)
{
	return permut_flex_event_ids;
}

static inline u64 __permut_read_cpu_counter(struct perf_event **events, int index)
{
	struct perf_event *event = events[index];

	if (!event || !event->pmu)
		return 0;

	return perf_event_read_value_local(event);
}

u64 permut_read_cpu_counter(int cpu, int index, int pinned)
{
	struct perf_event **events = perf_events_of_cpu(cpu, pinned);
	return __permut_read_cpu_counter(events, index);
}

u64 permut_read_cpu_pinned_event(int cpu, int event_id)
{
	struct perf_event **events = perf_events_of_cpu(cpu, EVENT_TYPE_PINNED);
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids) {
		if (event_id == event_ids[index])
			return __permut_read_cpu_counter(events, index);
	}

	return 0;
}

u64 permut_read_cpu_flex_event(int cpu, int event_id)
{
	struct perf_event **events = perf_events_of_cpu(cpu, EVENT_TYPE_FLEX);
	int *event_ids = permut_get_flex_event_ids();
	int index;

	for_each_permut_flex_event(index, event_ids) {
		if (event_id == event_ids[index])
			return __permut_read_cpu_counter(events, index);
	}

	return 0;
}

void permut_read_cpu_events(int cpu, u64 *pinned_data, u64 *flex_data)
{
	int *pinned_event_ids = permut_get_pinned_event_ids();
	int *flex_event_ids = permut_get_flex_event_ids();
	int event_id;
	int index;

	for_each_permut_pinned_event(index, pinned_event_ids)
		pinned_data[index] = permut_read_cpu_counter(cpu, index, EVENT_TYPE_PINNED);

	for_each_permut_flex_event(index, flex_event_ids) {
		event_id = flex_event_ids[index];
		if (event_id == PERMUT_EVENT_GROUP_TERM)
			continue;

		flex_data[index] = permut_read_cpu_counter(cpu, index, EVENT_TYPE_FLEX);
	}
}

static struct perf_event_attr *alloc_attr(int event_id, int pinned)
{
	struct perf_event_attr *attr;

	attr = kzalloc(sizeof(*attr), GFP_KERNEL);
	if (!attr)
		return ERR_PTR(-ENOMEM);

	attr->type = PERF_TYPE_RAW;
	attr->config = (u64)event_id;
	attr->size = sizeof(*attr);
	attr->pinned = (u64)pinned;
	attr->disabled = 1;
	attr->exclude_hv = sysctl_permut_exclude_hv;
	attr->exclude_idle = sysctl_permut_exclude_idle;
	attr->exclude_kernel = sysctl_permut_exclude_kernel;

	return attr;
}

static int create_cpu_counter(int cpu, int event_id, int index, int pinned)
{
	struct perf_event **events = perf_events_of_cpu(cpu, pinned);
	struct perf_event_attr *attr;
	struct perf_event *event;

	attr = alloc_attr(event_id, pinned);
	if (IS_ERR(attr))
		return PTR_ERR(attr);

	event = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(event)) {
		pr_err("%s: unable to create perf event "
			"(cpu:%i-type:%d-pinned:%d-id:0x%llx) : %ld",
			__func__, cpu, attr->type, attr->pinned, attr->config, PTR_ERR(event));

		kfree(attr);
		return PTR_ERR(event);
	}

	events[index] = event;
	perf_event_enable(events[index]);

	if (pinned && event->hw.idx == -1) {
		pr_err("%s: pinned event unable to get onto hardware, perf event "
			"(cpu:%i-type:%d-config:0x%llx)",
			__func__, cpu, attr->type, attr->config);

		kfree(attr);
		return -EINVAL;
	}

	kfree(attr);

	return 0;
}

static int create_cpu_group_counters(int cpu, int *event_ids, int num,
				     int *start_index, int pinned)
{
	struct perf_event **events = perf_events_of_cpu(cpu, pinned);
	struct perf_event_attr *attr;
	struct perf_event *event;
	int event_id;
	int index;
	int err;

	index = *start_index;

	/* Create group leader first */
	err = create_cpu_counter(cpu, event_ids[index], index, pinned);
	if (err) {
		pr_err("%s: create permut perf group leader failed for cpu=%d : %d\n",
			__func__, cpu, err);
		return err;
	}

	/* Then the group members */
	for (index = index + 1; index < num; index++) {
		event_id = event_ids[index];
		if (event_id == PERMUT_EVENT_GROUP_TERM)
			break;

		attr = alloc_attr(event_id, pinned);
		if (IS_ERR(attr))
			return PTR_ERR(attr);

		event = perf_event_create_kernel_group_counter(attr, cpu, NULL,
							       events[index], NULL, NULL);
		if (IS_ERR(event)) {
			pr_err("%s: unable to create perf event "
				"(cpu:%i-type:%d-pinned:%d-id:0x%llx) : %ld\n",
				__func__, cpu, attr->type, attr->pinned,
				attr->config, PTR_ERR(event));

			kfree(attr);
			return PTR_ERR(event);
		}

		events[index] = event;
		perf_event_enable(event);

		kfree(attr);
	}

	*start_index = index;

	return 0;
}

static int release_cpu_counter(int cpu, int event_id, int index, int pinned)
{
	struct perf_event **events = perf_events_of_cpu(cpu, pinned);
	struct perf_event *event;

	event = events[index];
	if (!event)
		return 0;

	perf_event_release_kernel(event);
	events[index] = NULL;

	return 0;
}

static int release_cpu_group_counters(int cpu, int *event_ids, int num,
				      int *start_index, int pinned)
{
	int index = *start_index;
	int event_id;

	/* release group member events */
	for (index = index + 1; index < num; index++) {
		event_id = event_ids[index];
		if (event_id == PERMUT_EVENT_GROUP_TERM)
			break;

		release_cpu_counter(cpu, event_id, index, pinned);
	}
	/* release group leader event */
	release_cpu_counter(cpu, event_id, *start_index, pinned);

	*start_index = index;

	return 0;
}

/* Helpers for permut perf */
static int do_pinned_events(int (*fn)(int, int, int, int), int cpu)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;
	int err;

	for_each_permut_pinned_event(index, event_ids) {
		err = fn(cpu, event_ids[index], index, EVENT_TYPE_PINNED);
		if (err)
			return err;
	}

	return 0;
}

static int do_flex_events(int (*fn)(int, int*, int, int*, int), int cpu)
{
	int *event_ids = permut_get_flex_event_ids();
	int index;
	int err;

	for_each_permut_flex_event(index, event_ids) {
		err = fn(cpu, event_ids, NR_PERMUT_FLEX_EVENTS, &index, EVENT_TYPE_FLEX);
		if (err)
			return err;
	}

	return 0;
}

static void permut_init_completion_state(struct permut_cpu_work_info *done,
					 unsigned int nr_todo)
{
	memset(done, 0, sizeof(*done));
	atomic_set(&done->nr_todo, nr_todo);
	init_completion(&done->completion);
}

static void __permut_perf_create(struct kthread_work *work)
{
	int cpu = raw_smp_processor_id();
	int err;

	err = do_pinned_events(create_cpu_counter, cpu);
	if (err) {
		pr_err("%s: create pinned events failed : %d\n", __func__, err);
		do_pinned_events(release_cpu_counter, cpu);
		goto out;
	}

	err = do_flex_events(create_cpu_group_counters, cpu);
	if (err) {
		pr_err("%s: create flexible events failed : %d\n", __func__, err);
		do_flex_events(release_cpu_group_counters, cpu);
		goto out;
	}

	pr_info("CPU%d permut class event create success\n", cpu);

out:
	create_work_info.ret += err;
	if (atomic_dec_and_test(&create_work_info.nr_todo))
		complete(&create_work_info.completion);
}

int permut_perf_create(int *pinned_event_ids, int *flex_events_ids,
		       const struct cpumask *cpus)
{
	int cpu;
	int ret;

	permut_pinned_event_ids = pinned_event_ids;
	permut_flex_event_ids = flex_events_ids;

	permut_init_completion_state(&create_work_info, cpumask_weight(cpus));
	for_each_cpu(cpu, cpus) {
		struct permut_cpu_info *pi_cpu = &per_cpu(permut_cpu, cpu);

		ret = kthread_queue_work(&pi_cpu->create_worker, &pi_cpu->create_work);
		if (!ret) {
			pr_err("%s: permut cannot create kthread for cpu=%d : %d\n",
				__func__, cpu, ret);
			return -ENOSYS;
		}
	}

	wait_for_completion(&create_work_info.completion);

	return create_work_info.ret;
}

static void __permut_perf_release(struct kthread_work *work)
{
	int cpu = raw_smp_processor_id();

	permut_clear_cpu_count(cpu);

	/* release pinned & flexible events */
	do_pinned_events(release_cpu_counter, cpu);
	do_flex_events(release_cpu_group_counters, cpu);

	if (atomic_dec_and_test(&release_work_info.nr_todo))
		complete(&release_work_info.completion);

	pr_info("cpu%d permut class event release success\n", cpu);
}

void permut_perf_release(const struct cpumask *cpus)
{
	int cpu;

	permut_init_completion_state(&release_work_info, cpumask_weight(cpus));
	for_each_cpu(cpu, cpus) {
		struct permut_cpu_info *pi_cpu = &per_cpu(permut_cpu, cpu);

		kthread_queue_work(&pi_cpu->release_worker, &pi_cpu->release_work);
	}

	wait_for_completion(&release_work_info.completion);
}

int permut_init(void)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 2 };
	struct task_struct *release_thread;
	struct task_struct *create_thread;
	int cpu;

	for_each_cpu(cpu, cpu_online_mask) {
		struct permut_cpu_info *pi_cpu = &per_cpu(permut_cpu, cpu);

		kthread_init_worker(&pi_cpu->release_worker);
		kthread_init_work(&pi_cpu->release_work, __permut_perf_release);
		release_thread = kthread_create_on_cpu(kthread_worker_fn, &pi_cpu->release_worker,
						       cpu, "permut_release");
		sched_setscheduler_nocheck(release_thread, SCHED_FIFO, &param);
		pi_cpu->release_thread = release_thread;
		wake_up_process(release_thread);

		kthread_init_worker(&pi_cpu->create_worker);
		kthread_init_work(&pi_cpu->create_work, __permut_perf_create);
		create_thread = kthread_create_on_cpu(kthread_worker_fn, &pi_cpu->create_worker,
						      cpu, "permut_create");
		sched_setscheduler_nocheck(create_thread, SCHED_FIFO, &param);
		pi_cpu->create_thread = create_thread;
		wake_up_process(create_thread);
	}

	return 0;
}

module_init(permut_init);
