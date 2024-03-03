/*
 * ARM Multi-Cluster Platforms CPUFreq support for Gem5
 *
 * Copyright (C) 2013-2014,2016 ARM Ltd.
 * Copyright (C) 2024 Glaz Roman
 *
 * Akash Bagdia <akash.bagdia@arm.com>
 * Vasileios Spiliopoulos <vasileios.spiliopoulos@arm.com>
 * Sascha Bischoff <sascha.bischoff@arm.com>
 * Glaz Roman <vokerlee@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

 #define pr_fmt(fmt) "arm-gem5 cpufreq: " fmt

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/types.h>
#include <linux/types.h>
#include <linux/clk-gem5-energy-ctrl.h>

#define MAX_CPUFREQ_CLUSTERS	32

struct cpufreq_arm_opp_ops {
	char name[CPUFREQ_NAME_LEN];
	int (*get_transition_latency)(struct device *cpu_dev);
	/*
	 * This must set opp table for cpu_dev in a similar way as done by
	 * of_init_opp_table().
	 */
	int (*init_opp_table)(struct device *cpu_dev);
};

static struct cpufreq_arm_opp_ops *arm_opp_ops;
static struct clk *clk[MAX_CPUFREQ_CLUSTERS];
static struct cpufreq_frequency_table *freq_table[MAX_CPUFREQ_CLUSTERS + 1];
static struct mutex cluster_lock[MAX_CPUFREQ_CLUSTERS];

static atomic_t cluster_usage[MAX_CPUFREQ_CLUSTERS + 1] = {
	ATOMIC_INIT(0),
};

static inline int cpu_to_cluster(int cpu)
{
	return topology_physical_package_id(cpu);
}

/* Export internal frequencies/voltages info from clock to cpu device
 * via OPP driver.
 * When calling this function, all clocks are already initialized */
static int gem5_init_cpu_opp_table(struct device *cpu_dev)
{
	int i = -1, count, cpu_cluster = cpu_to_cluster(cpu_dev->id);
	u32 *freq_table;
	u32 *volt_table; /* In micro volts */
	int ret;

	count = clk_energy_ctrl_get_opp_table(clk[cpu_cluster], &freq_table, &volt_table);

	if (!freq_table || !count) {
		pr_err("%s: clock \"%s\" returned invalid freq table",
			__func__, __clk_get_name(clk[cpu_cluster]));
		return -EINVAL;
	}

	if (!volt_table || !count) {
		pr_err("%s: clock \"%s\" returned invalid voltage table",
			__func__, __clk_get_name(clk[cpu_cluster]));
		return -EINVAL;
	}

	while (++i < count) {
		ret = dev_pm_opp_add(cpu_dev, freq_table[i] * 1000, volt_table[i]);
		if (ret) {
			dev_warn(cpu_dev,
				"%s: Failed to add OPP freq %d, u-voltage %d, err: %d\n",
				 __func__, freq_table[i] * 1000, volt_table[i], ret);
			return ret;
		}
	}

	return 0;
}

/* When calling this function, all clocks are already initialized */
static int gem5_get_cpu_transition_latency(struct device *cpu_dev)
{
	int cpu_cluster = cpu_to_cluster(cpu_dev->id);
	return clk_energy_ctrl_get_trans_latency(clk[cpu_cluster]);
}

static struct cpufreq_arm_opp_ops gem5_opp_ops = {
	.name = "gem5-opp-ops",
	.get_transition_latency = gem5_get_cpu_transition_latency,
	.init_opp_table = gem5_init_cpu_opp_table,
};

static unsigned int clk_get_cpu_rate(unsigned int cpu)
{
	u32 cpu_cluster = cpu_to_cluster(cpu);
	u32 rate = clk_get_rate(clk[cpu_cluster]) / 1000;
	pr_debug("%s: cpu: %d, cluster: %d, freq: %u\n", __func__, cpu,
		 cpu_cluster, rate);

	return rate;
}

static unsigned int gem5_cpufreq_get_rate(unsigned int cpu)
{
	return clk_get_cpu_rate(cpu);
}

static unsigned int gem5_cpufreq_set_rate(u32 cpu, u32 new_cluster, u32 rate)
{
	int ret;
	pr_debug("%s: cpu: %d, new cluster: %d, freq: %d\n",
		 __func__, cpu, new_cluster, rate);

	mutex_lock(&cluster_lock[new_cluster]);
	ret = clk_set_rate(clk[new_cluster], rate * 1000);
	mutex_unlock(&cluster_lock[new_cluster]);

	if (WARN_ON(ret)) {
		pr_err("clk_set_rate failed: %d, new cluster: %d\n", ret,
		       new_cluster);
	}

	return ret;
}

static int gem5_cpufreq_set_target(struct cpufreq_policy *policy,
					unsigned int target_freq,
					unsigned int relation)
{
	struct cpufreq_freqs freqs;
	u32 cpu = policy->cpu, freq_tab_idx, cpu_cluster;
	int ret = 0;

	cpu_cluster = cpu_to_cluster(cpu);
	freqs.old = gem5_cpufreq_get_rate(cpu);

	/* Determine valid target frequency using freq_table */
	freq_tab_idx = cpufreq_frequency_table_target(policy, target_freq, relation);
	freqs.new = freq_table[cpu_cluster][freq_tab_idx].frequency;

	pr_debug("%s: cpu: %d, cluster: %d, oldfreq: %d, target freq: %d, new freq: %d\n",
		 __func__, cpu, cpu_cluster, freqs.old, target_freq, freqs.new);

	if (freqs.old == freqs.new)
		return 0;

	cpufreq_freq_transition_begin(policy, &freqs);
	ret = gem5_cpufreq_set_rate(cpu, cpu_cluster, freqs.new);
	cpufreq_freq_transition_end(policy, &freqs, ret);

	if (ret)
		freqs.new = freqs.old;

	return ret;
}

static void _put_cluster_clk_and_freq_table(struct device *cpu_dev)
{
	u32 cpu_cluster = cpu_to_cluster(cpu_dev->id);
	if (!atomic_dec_return(&cluster_usage[cpu_cluster])) {
		clk_put(clk[cpu_cluster]);
		dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table[cpu_cluster]);
		dev_dbg(cpu_dev, "%s: cluster: %d\n", __func__, cpu_cluster);
	}
}

static void put_cluster_clk_and_freq_table(struct device *cpu_dev)
{
	int cluster = cpu_to_cluster(cpu_dev->id);
	if (cluster < MAX_CPUFREQ_CLUSTERS)
		return _put_cluster_clk_and_freq_table(cpu_dev);
}

static int _get_cluster_clk_and_freq_table(struct device *cpu_dev)
{
	u32 cpu_cluster = cpu_to_cluster(cpu_dev->id);
	char name[] = "clk-gem5-domain.?";
	int ret;

	if (atomic_inc_return(&cluster_usage[cpu_cluster]) != 1)
		return 0;

	name[sizeof(name) - 2] = cpu_cluster + '0';

	clk[cpu_cluster] = clk_get_sys(name, NULL);
	if (IS_ERR_OR_NULL(clk[cpu_cluster])) {
		ret = PTR_ERR(clk[cpu_cluster]);
		dev_err(cpu_dev, "%s: cannot find clock \"%s\" for cpu%d, err: %d\n",
			__func__, name, cpu_dev->id, ret);
		goto atomic_dec;
	}

	/* Export frequencies/voltages to cpu device */
	ret = arm_opp_ops->init_opp_table(cpu_dev);
	if (ret) {
		dev_err(cpu_dev, "%s: init_opp_table failed for cpu%d, err: %d\n",
			__func__, cpu_dev->id, ret);
		goto atomic_dec;
	}

	/* As cpu device already contains valid OPPs, so populate local freq_table */
	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table[cpu_cluster]);
	if (ret) {
		dev_err(cpu_dev, "%s: failed to init cpufreq table for cpu%d, err: %d\n",
			__func__, cpu_dev->id, ret);
		goto atomic_dec;
	}

	return 0;

atomic_dec:
	atomic_dec(&cluster_usage[cpu_cluster]);
	dev_err(cpu_dev, "%s: Failed to get clk for cpu: %d, cluster: %d\n",
		__func__, cpu_dev->id, cpu_cluster);

	return ret;
}

static int get_cluster_clk_and_freq_table(struct device *cpu_dev)
{
	int cpu_cluster = cpu_to_cluster(cpu_dev->id);
	if (cpu_cluster < MAX_CPUFREQ_CLUSTERS)
		return _get_cluster_clk_and_freq_table(cpu_dev);

	return 0;
}

/* Per-CPU initialization */
static int gem5_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	u32 cpu_cluster = cpu_to_cluster(policy->cpu);
	struct device *cpu_dev;
	int ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	ret = get_cluster_clk_and_freq_table(cpu_dev);
	if (ret)
		return ret;

	if (cpu_cluster < MAX_CPUFREQ_CLUSTERS)
		policy->freq_table = freq_table[cpu_cluster];

	ret = cpufreq_table_validate_and_sort(policy);
	if (ret)
		return ret;

	if (cpu_cluster < MAX_CPUFREQ_CLUSTERS) {
		cpumask_copy(policy->cpus, topology_core_cpumask(policy->cpu));
	} else {
		pr_err("%s: invalid current cluster %d\n", __func__, cpu_cluster);
		return -ENODEV;
	}

	if (arm_opp_ops->get_transition_latency)
		policy->cpuinfo.transition_latency =
			arm_opp_ops->get_transition_latency(cpu_dev);
	else
		policy->cpuinfo.transition_latency = CPUFREQ_ETERNAL;

	policy->cur = clk_get_cpu_rate(policy->cpu);
	dev_info(cpu_dev, "%s: cpu%d is initialized\n", __func__, policy->cpu);

	return 0;
}

/* Export freq_table to sysfs */
static struct freq_attr *gem5_cpufreq_cpu_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver gem5_cpufreq_driver = {
	.name	= "arm-gem5-cpufreq",
	.flags	= CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify	= cpufreq_generic_frequency_table_verify,
	.target	= gem5_cpufreq_set_target,
	.get	= gem5_cpufreq_get_rate,
	.init	= gem5_cpufreq_cpu_init,
	.attr	= gem5_cpufreq_cpu_attr,
};

int gem5_cpufreq_register(struct cpufreq_arm_opp_ops *ops)
{
	int ret, i;
	if (arm_opp_ops) {
		pr_warn("try to register cpufreq driver, but driver %s "
			"already exists, exiting\n", gem5_cpufreq_driver.name);
		return -EBUSY;
	}

	if (!ops || !strlen(ops->name) || !ops->init_opp_table) {
		pr_err("%s: invalid arm_opp_ops, exiting\n", __func__);
		return -ENODEV;
	}

	arm_opp_ops = ops;

	for (i = 0; i < MAX_CPUFREQ_CLUSTERS; i++)
		mutex_init(&cluster_lock[i]);

	ret = cpufreq_register_driver(&gem5_cpufreq_driver);
	if (ret) {
		pr_info("failed registering platform driver: %s, err: %d\n",
			gem5_cpufreq_driver.name, ret);
		arm_opp_ops = NULL;
	} else {
		pr_info("registered platform driver: %s\n",
			gem5_cpufreq_driver.name);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(gem5_cpufreq_register);

void gem5_cpufreq_unregister(struct cpufreq_arm_opp_ops *ops)
{
	int i;
	if (arm_opp_ops != ops) {
		pr_err("%s: registered with %s, can't unregister, exiting\n",
			__func__, gem5_cpufreq_driver.name);
		return;
	}

	cpufreq_unregister_driver(&gem5_cpufreq_driver);
	pr_info("%s: un-registered platform driver: %s\n", __func__,
		gem5_cpufreq_driver.name);

	for (i = 0; i < MAX_CPUFREQ_CLUSTERS; i++) {
		struct device *cpu_dev = get_cpu_device(i);
		if (!cpu_dev) {
			pr_err("%s: failed to get cpu%d device\n",
				__func__, i);
			return;
		}

		put_cluster_clk_and_freq_table(cpu_dev);
	}

	arm_opp_ops = NULL;
}
EXPORT_SYMBOL_GPL(gem5_cpufreq_unregister);

static int gem5_cpufreq_init(void)
{
	return gem5_cpufreq_register(&gem5_opp_ops);
}
module_init(gem5_cpufreq_init);

static void gem5_cpufreq_exit(void)
{
	return gem5_cpufreq_unregister(&gem5_opp_ops);
}
module_exit(gem5_cpufreq_exit);

MODULE_DESCRIPTION("ARM gem5 multi-cluster cpufreq driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Roman Glaz <vokerlee@gmail.com>");
