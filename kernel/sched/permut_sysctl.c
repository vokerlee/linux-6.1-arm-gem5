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
#include <linux/sysctl.h>
#include <linux/capability.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/sched/task.h>
#include <linux/cpuhotplug.h>
#include <linux/permut.h>
#include "sched.h"

#ifdef CONFIG_PROC_SYSCTL

static int __maybe_unused zero = 0;
static int __maybe_unused one = 1;

DEFINE_STATIC_KEY_FALSE(sched_permut);

struct cpumask sysctl_permut_cpumask;

unsigned int sysctl_permut_exclude_hv = 1;
unsigned int sysctl_permut_exclude_idle = 0;
unsigned int sysctl_permut_exclude_kernel = 1;

int sysctl_permut_pinned_events[NR_PERMUT_PINNED_EVENTS] = {
	ARMV8_PMUV3_PERFCTR_INST_RETIRED,
	ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL,
	ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL,
	PERMUT_EVENT_FINAL_TERM
};
int sysctl_permut_flex_events[NR_PERMUT_FLEX_EVENTS] = {
	PERMUT_EVENT_FINAL_TERM
};

static unsigned long *permut_cpumask_bits = cpumask_bits(&sysctl_permut_cpumask);

static void permut_invalid_all_tasks(void)
{
	struct task_struct *task;
	struct task_struct *proc;

	read_lock(&tasklist_lock);

	for_each_process_thread(proc, task) {
		permut_counter_window *window = &task->permut_info->window;
		memset(window, 0, sizeof(*window));
		task->permut_info->valid = false;
	}

	read_unlock(&tasklist_lock);
}

/* For CPU-hotplug */
static int __ref permut_online(unsigned int cpu)
{
	int ret = 0;
	if (cpumask_test_cpu(cpu, &sysctl_permut_cpumask))
		ret = permut_create_one_cpu(cpu);
	if (ret)
		pr_err("create cpu %d permut fail\n", cpu);

	return 0;
}

/* For CPU-hotplug */
static int __ref permut_offline(unsigned int cpu)
{
	if (cpumask_test_cpu(cpu, &sysctl_permut_cpumask))
		permut_release_one_cpu(cpu);

	return 0;
}

static DEFINE_MUTEX(state_lock);
int set_permut_state(bool enabled, const struct cpumask *new_mask)
{
	int ret = 0;
	int state;

	if (enabled && !new_mask)
		return -EINVAL;

	cpus_read_lock();
	mutex_lock(&state_lock);

	state = static_branch_likely(&sched_permut);
	if (enabled == state) {
		pr_warn("permut has already been %s\n", state ? "enabled" : "disabled");
		goto out;
	}

	if (enabled) {
		cpumask_copy(&sysctl_permut_cpumask, new_mask);

		ret = permut_create();
		if (ret) {
			pr_err("permut enable failed\n");
			cpumask_clear(&sysctl_permut_cpumask);
			goto out;
		}

		static_branch_enable(&sched_permut);
		pr_info("permut enabled\n");
	} else {
		static_branch_disable(&sched_permut);

		permut_release();
		permut_invalid_all_tasks();

		cpumask_clear(&sysctl_permut_cpumask);
		pr_info("permut disabled\n");
	}
out:
	mutex_unlock(&state_lock);
	cpus_read_unlock();

	return ret;
}

/*
 * the other procfs files of permut cannot be modified if sched_permut is already enabled
 */
static int permut_proc_state(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int state;
	int err;

	state = static_branch_likely(&sched_permut);

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;

	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;
	if (write)
		err = set_permut_state(state, &sysctl_permut_cpumask);

	return err;
}

static int permut_proc_cpumask(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int err;

	cpus_read_lock();
	if (write && static_branch_likely(&sched_permut)) {
		err = -EPERM;
		pr_info("[%s] unmodifiable when permut enabled\n", table->procname);
		goto out;
	}

	err = proc_do_large_bitmap(table, write, buffer, lenp, ppos);
	if (err < 0 || !write)
		goto out;

	cpumask_and(&sysctl_permut_cpumask, &sysctl_permut_cpumask, cpu_possible_mask);
out:
	cpus_read_unlock();

	return err;
}

static int permut_proc_dointvec(struct ctl_table *table, int write,
			        void __user *buffer, size_t *lenp, loff_t *ppos)
{
	if (write && static_branch_likely(&sched_permut)) {
		pr_info("[%s] unmodifiable when permut enabled\n", table->procname);
		return -EPERM;
	}
	return proc_dointvec(table, write, buffer, lenp, ppos);
}

static int permut_proc_dointvec_minmax(struct ctl_table *table, int write,
				       void __user *buffer, size_t *lenp, loff_t *ppos)
{
	if (write && static_branch_likely(&sched_permut)) {
		pr_info("[%s] unmodifiable when permut enabled\n", table->procname);
		return -EPERM;
	}
	return proc_dointvec_minmax(table, write, buffer, lenp, ppos);
}

struct ctl_table permut_table[] = {
	{
		.procname	= "enabled",
		.data		= NULL,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0640,
		.proc_handler	= permut_proc_state,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{
		.procname	= "cpumask",
		.data		= &permut_cpumask_bits,
		.maxlen		= NR_CPUS,
		.mode		= 0640,
		.proc_handler	= permut_proc_cpumask,
	},
	{
		.procname	= "flexible_events",
		.data		= sysctl_permut_flex_events,
		.maxlen		= sizeof(sysctl_permut_flex_events),
		.mode		= 0640,
		.proc_handler	= permut_proc_dointvec,
	},
	{
		.procname	= "pinned_events",
		.data		= sysctl_permut_pinned_events,
		.maxlen		= sizeof(sysctl_permut_pinned_events),
		.mode		= 0640,
		.proc_handler	= permut_proc_dointvec,
	},
	{
		.procname	= "exclude_hv",
		.data		= &sysctl_permut_exclude_hv,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0640,
		.proc_handler	= permut_proc_dointvec_minmax,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{
		.procname	= "exclude_idle",
		.data		= &sysctl_permut_exclude_idle,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0640,
		.proc_handler	= permut_proc_dointvec_minmax,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{
		.procname	= "exclude_kernel",
		.data		= &sysctl_permut_exclude_kernel,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0640,
		.proc_handler	= permut_proc_dointvec_minmax,
		.extra1		= &zero,
		.extra2		= &one,
	},
	{ }
};
#endif /* CONFIG_PROC_SYSCTL */

static int __init permut_init(void)
{
	int cpu, ret;

	for_each_possible_cpu(cpu)
		cpumask_set_cpu(cpu, &sysctl_permut_cpumask);

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "permut:online",
					permut_online, permut_offline);
	if (ret < 0)
		pr_err("permut set hotplug state failed\n");

	return ret;
}
late_initcall(permut_init);
