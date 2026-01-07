/*
 * SPDX-License-Identifier: GPL-2.0
 * Memory Aware CPU Frequency Manager (MACFM) is to control
 * CPU frequency based on memory architecture awareness via PMU
 * events.
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/cpu_pm.h>
#include <linux/permut.h>
#include <linux/macfm.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <linux/kernel.h>
#include <uapi/linux/sched/types.h>
#include <asm-generic/param.h>
#include "sched.h"

#define DEFAULT_CPU_TIMER_SLACK ((USEC_PER_SEC) / (HZ)) * 2

struct macfmgov_tunables {
	struct gov_attr_set attr_set;
	int cpu_timer_slack;
};

struct macfmgov_policy {
	struct cpufreq_policy *policy;
	struct macfmgov_tunables *tunables;

	raw_spinlock_t update_lock; /* for shared policies */
	raw_spinlock_t cpu_timer_lock;

	unsigned int next_cpufreq; /* aggregated by all users of this policy */

	struct irq_work irq_work;
	struct kthread_work work;
	struct kthread_worker worker;

	struct task_struct *thread;

	bool work_in_progress;
	bool governor_enabled;
};

struct macfmgov_cpu {
	struct macfmgov_policy *mg_policy;
	unsigned int cpu;
	unsigned int voted_cpufreq;

	unsigned int flags;

	struct timer_list cpu_slack_timer;
	bool enabled;
};

static DEFINE_PER_CPU(struct macfmgov_cpu, macfmgov_cpu);

static atomic_t g_ref_count;

static void macfm_update_cpu(int cpu)
{
	struct task_struct *curr;
	struct rq_flags rf;
	struct rq *rq;

	rq = cpu_rq(cpu);
	rq_lock_irq(rq, &rf);

	curr = rq->curr;
	/* Trigger counters & utils update due to change of cluster CPU frequency */
	permut_account_task(curr, curr, cpu, PERMUT_PERF_CHANGE);
	pr_warn("%s: cpu=%d updated\n", __func__, cpu);

	rq_unlock_irq(rq, &rf);
}

static void macfmgov_work(struct kthread_work *work)
{
	struct macfmgov_policy *mg_policy = container_of(work, struct macfmgov_policy, work);
	struct cpufreq_policy *policy = mg_policy->policy;
	const cpumask_t *mask = policy->related_cpus;
	cpumask_t update_mask = { CPU_BITS_NONE };
	int cpu;

	if (!sysctl_macfm_enable)
		return;

	for_each_cpu(cpu, mask)
		if (!idle_cpu(cpu))
			cpumask_set_cpu(cpu, &update_mask);

	cpufreq_driver_target(mg_policy->policy, mg_policy->next_cpufreq,
			      CPUFREQ_RELATION_L);

	for_each_cpu(cpu, &update_mask)
		macfm_update_cpu(cpu);

	pr_warn("%s: change cpufreq=%u\n",
		__func__, mg_policy->next_cpufreq);

	mg_policy->work_in_progress = false;
}

static void macfmgov_irq_work(struct irq_work *irq_work)
{
	struct macfmgov_policy *mg_policy;

	mg_policy = container_of(irq_work, struct macfmgov_policy, irq_work);
	kthread_queue_work(&mg_policy->worker, &mg_policy->work);
}

static struct macfmgov_policy *macfmgov_policy_alloc(struct cpufreq_policy *policy)
{
	struct macfmgov_policy *mg_policy;

	mg_policy = kzalloc(sizeof(*mg_policy), GFP_KERNEL);
	if (!mg_policy)
		return NULL;

	mg_policy->policy = policy;
	raw_spin_lock_init(&mg_policy->update_lock);
	raw_spin_lock_init(&mg_policy->cpu_timer_lock);

	return mg_policy;
}

static void macfmgov_policy_free(struct macfmgov_policy *mg_policy)
{
	kfree(mg_policy);
}

static int macfmgov_kthread_create(struct macfmgov_policy *mg_policy)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 2 };
	struct cpufreq_policy *policy = mg_policy->policy;
	struct task_struct *thread;
	int ret;

	kthread_init_work(&mg_policy->work, macfmgov_work);
	kthread_init_worker(&mg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &mg_policy->worker,
				"macfmgov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("%s: failed to create macfmgov thread: %ld\n",
			__func__, PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	mg_policy->thread = thread;

	if (!policy->dvfs_possible_from_any_cpu)
		kthread_bind_mask(thread, policy->related_cpus);
	init_irq_work(&mg_policy->irq_work, macfmgov_irq_work);

	wake_up_process(thread);

	return 0;
}

static void macfmgov_kthread_stop(struct macfmgov_policy *mg_policy)
{
	kthread_flush_worker(&mg_policy->worker);
	kthread_stop(mg_policy->thread);
}

static void macfmgov_tunables_init(struct cpufreq_policy *policy,
				   struct macfmgov_tunables *tunables)
{
	tunables->cpu_timer_slack = DEFAULT_CPU_TIMER_SLACK;
}

static void macfmgov_tunables_free(struct macfmgov_tunables *tunables)
{
	kfree(tunables);
}

static void macfmgov_update_policy(struct macfmgov_policy *mg_policy)
{
	pr_warn("%s: IRQ work start\n", __func__);

	if (!mg_policy->work_in_progress) {
		mg_policy->work_in_progress = true;
		irq_work_queue(&mg_policy->irq_work);
	}
}

static unsigned int macfmgov_aggregate_cpufreq(struct macfmgov_policy *mg_policy)
{
	struct cpufreq_policy *policy;
	struct macfmgov_cpu *mg_cpu;
	unsigned int max_cpufreq;
	int cpu;

	if (!mg_policy || !mg_policy->governor_enabled)
		return 0;

	policy = mg_policy->policy;
	max_cpufreq = 0;

	for_each_cpu(cpu, policy->cpus) {
		mg_cpu = &per_cpu(macfmgov_cpu, cpu);
		max_cpufreq = max_t(unsigned int, max_cpufreq, mg_cpu->voted_cpufreq);
		mg_cpu->flags = 0;
	}

	return max_cpufreq;
}

static bool macfmgov_is_cpufreq_updated(struct macfmgov_policy *mg_policy)
{
	unsigned int new_cpufreq;
	bool cpufreq_need_change;
	unsigned long flags;

	raw_spin_lock_irqsave(&mg_policy->update_lock, flags);

	cpufreq_need_change = true;
	new_cpufreq = macfmgov_aggregate_cpufreq(mg_policy);

	if (mg_policy->next_cpufreq == new_cpufreq) {
		cpufreq_need_change = false;
	}
	mg_policy->next_cpufreq = new_cpufreq;

	raw_spin_unlock_irqrestore(&mg_policy->update_lock, flags);

	return cpufreq_need_change;
}

void macfm_check_freq_update(int cpu, unsigned int flags)
{
	struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);
	struct macfmgov_policy *mg_policy;

	pr_warn("%s: start collecting new cpufreqs for cpu=%d [flags=%u]\n",
		__func__, cpu, flags);

	atomic_inc(&g_ref_count);
	if (!mg_cpu->enabled)
		goto out;

	mg_policy = mg_cpu->mg_policy;
	if (!mg_policy)
		goto out;

	mg_cpu->flags |= flags;

	if ((flags & MACFM_UPDATE) || (flags & MACFM_MIGRATE)) {
		if (macfmgov_is_cpufreq_updated(mg_policy))
			macfmgov_update_policy(mg_policy);
	} else if (flags & MACFM_POLICY_MIN_RESTORE)
		macfmgov_update_policy(mg_policy);
out:
	atomic_dec(&g_ref_count);
}

int macfm_queue_freq_update(int cpu)
{
	struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);
	struct macfmgov_policy *mg_policy;
	int ret = -1;

	atomic_inc(&g_ref_count);
	if (!mg_cpu->enabled)
		goto out;

	mg_policy = mg_cpu->mg_policy;
	if (!mg_policy || !mg_policy->governor_enabled)
		goto out;

	ret = 0;

	macfmgov_update_policy(mg_policy);
out:
	atomic_dec(&g_ref_count);
	return ret;
}

static void macfm_cpu_slack_timer_resched(struct macfmgov_cpu *mg_cpu)
{
	struct macfmgov_policy *mg_policy = mg_cpu->mg_policy;
	unsigned long flags;
	u64 expires;

	raw_spin_lock_irqsave(&mg_policy->cpu_timer_lock, flags);
	if (!mg_policy || !mg_policy->governor_enabled)
		goto unlock;

	del_timer(&mg_cpu->cpu_slack_timer);

	if (mg_policy->tunables->cpu_timer_slack >= 0 &&
	    mg_cpu->voted_cpufreq > mg_policy->policy->min) {
		expires = jiffies + usecs_to_jiffies(mg_policy->tunables->cpu_timer_slack);
		mg_cpu->cpu_slack_timer.expires = expires;
		add_timer_on(&mg_cpu->cpu_slack_timer, mg_cpu->cpu);
	}
unlock:
	raw_spin_unlock_irqrestore(&mg_policy->cpu_timer_lock, flags);
}

static void macfmgov_change_macfm_cpufreq(int cpu, unsigned int cpufreq)
{
	struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);
	struct macfmgov_policy *mg_policy = mg_cpu->mg_policy;
	struct cpufreq_policy *policy;
	unsigned int new_freq;
	int index;

	if (!mg_policy || !mg_policy->governor_enabled)
		return;

	policy = mg_policy->policy;

	index = cpufreq_frequency_table_target(policy, cpufreq, CPUFREQ_RELATION_L);
	new_freq = policy->freq_table[index].frequency;
	mg_cpu->voted_cpufreq = new_freq;

	macfm_cpu_slack_timer_resched(mg_cpu);
}

static void cpu_clear_macfm_cpufreq_timer(struct timer_list *unused)
{
	int cpu = smp_processor_id();
	struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);

	mg_cpu->voted_cpufreq = 0;

	pr_warn("%s: cpu=%d\n", __func__, cpu);
	macfm_check_freq_update(cpu, MACFM_UPDATE);
}

int macfm_calc_optimal_freq(struct macfmgov_cpu *mg_cpu, int cpu,
			    struct task_struct *p, int flags,
			    unsigned int *cpufreq)
{

	// Calculate new cpufreq
	*cpufreq = 1000000;
	return 0;
}

void macfm_load_pred_freq(int cpu, struct task_struct *p, int flags)
{
	struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);
	struct macfmgov_policy *mg_policy;
	unsigned int cpufreq;
	int ret;

	atomic_inc(&g_ref_count);
	if (!mg_cpu->enabled)
		goto out;

	mg_policy = mg_cpu->mg_policy;
	if (!mg_policy || !p->macfm_info)
		goto out;

	ret = macfm_calc_optimal_freq(mg_cpu, cpu, p, flags, &cpufreq);
	if (ret)
		pr_err("%s: cannot get cpufreq [cpu=%d, pid=%d]\n",
			__func__, cpu, p->pid);

	macfmgov_change_macfm_cpufreq(cpu, cpufreq);

	pr_warn("%s: cpu = %d, cpufreq=%u\n",
		__func__, cpu, cpufreq);

out:
	atomic_dec(&g_ref_count);
}

static int macfmgov_init(struct cpufreq_policy *policy)
{
	struct macfmgov_policy *mg_policy;
	struct macfmgov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	mg_policy = macfmgov_policy_alloc(policy);
	if (!mg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = macfmgov_kthread_create(mg_policy);
	if (ret)
		goto free_mg_policy;


	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	macfmgov_tunables_init(policy, tunables);

	policy->governor_data = mg_policy;
	mg_policy->tunables = tunables;

	return 0;

stop_kthread:
	macfmgov_kthread_stop(mg_policy);

free_mg_policy:
	macfmgov_policy_free(mg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void macfmgov_exit(struct cpufreq_policy *policy)
{
	struct macfmgov_policy *mg_policy = policy->governor_data;
	struct macfmgov_tunables *tunables = mg_policy->tunables;

	policy->governor_data = NULL;
	macfmgov_tunables_free(tunables);

	macfmgov_kthread_stop(mg_policy);
	macfmgov_policy_free(mg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int macfmgov_start(struct cpufreq_policy *policy)
{
	struct macfmgov_policy *mg_policy = policy->governor_data;
	unsigned long flags;
	unsigned int cpu;

	pr_warn("%s: start macfm_gov\n", __func__);

	if (!sysctl_macfm_enable) {
		pr_err("%s: MDEAS is disabled, so it is impossible to use '%s'"
			"governor\n", __func__, policy->governor->name);
		return -ENOENT;
	}

	mg_policy->next_cpufreq = 0;
	mg_policy->work_in_progress = false;

	for_each_cpu(cpu, policy->cpus) {
		struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);

		memset(mg_cpu, 0, sizeof(*mg_cpu));
		mg_cpu->cpu		= cpu;
		mg_cpu->mg_policy	= mg_policy;
	}

	raw_spin_lock_irqsave(&mg_policy->cpu_timer_lock, flags);
	for_each_cpu(cpu, policy->cpus) {
		struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);
		timer_setup(&mg_cpu->cpu_slack_timer, cpu_clear_macfm_cpufreq_timer, 0);
		add_timer_on(&mg_cpu->cpu_slack_timer, cpu);
		mg_cpu->enabled = true;
	}

	mg_policy->governor_enabled = true;
	raw_spin_unlock_irqrestore(&mg_policy->cpu_timer_lock, flags);

	return 0;
}

static void macfmgov_stop(struct cpufreq_policy *policy)
{
	struct macfmgov_policy *mg_policy = policy->governor_data;
	unsigned long flags;
	unsigned int cpu;

	raw_spin_lock_irqsave(&mg_policy->cpu_timer_lock, flags);

	mg_policy->governor_enabled = false;
	for_each_cpu(cpu, policy->cpus) {
		struct macfmgov_cpu *mg_cpu = &per_cpu(macfmgov_cpu, cpu);

		mg_cpu->enabled = false;
		del_timer_sync(&mg_cpu->cpu_slack_timer);
	}

	raw_spin_unlock_irqrestore(&mg_policy->cpu_timer_lock, flags);

	while (atomic_read(&g_ref_count))
		cpu_relax();

	irq_work_sync(&mg_policy->irq_work);
	kthread_cancel_work_sync(&mg_policy->work);
}

static void macfmgov_limits(struct cpufreq_policy *policy)
{
	macfm_check_freq_update(policy->cpu, MACFM_POLICY_MIN_RESTORE);
}

struct cpufreq_governor macfm_gov = {
	.name			= "macfm",
	.owner			= THIS_MODULE,
	.init			= macfmgov_init,
	.exit			= macfmgov_exit,
	.start			= macfmgov_start,
	.stop			= macfmgov_stop,
	.limits			= macfmgov_limits,
};

static int __init cpufreq_macfmgov_init(void)
{
	return cpufreq_register_governor(&macfm_gov);
}

static void __exit cpufreq_macfmgov_exit(void)
{
	cpufreq_unregister_governor(&macfm_gov);
}

fs_initcall(cpufreq_macfmgov_init);
module_exit(cpufreq_macfmgov_exit);
