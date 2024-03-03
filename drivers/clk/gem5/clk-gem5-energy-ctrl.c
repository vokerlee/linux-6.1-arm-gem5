/*
 * Copyright (C) 2013-2014,2016 ARM Limited
 * Copyright (C) 2013 Linaro
 * Copyright (C) 2024 Glaz Roman
 *
 * Authors: Akash Bagdia <Akash.bagdia@arm.com>
 *          Vasileios Spiliopoulos <vasileios.spiliopoulos@arm.com>
 *          Sascha Bischoff <sascha.bischoff@arm.com>
 *          Glaz Roman <vokerlee@gmail.com>
 * (code adapted from clk-vexpress-spc.c)
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

/* Energy conrtoller clock interface for adjusting OPPs of ClockDomains in Gem5 */

#define pr_fmt(fmt) "gem5 energy-ctrl: " fmt

#include <asm-generic/errno-base.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

// Energy controller register addresses
#define DVFS_HANDLER_STATUS		0x00
#define DVFS_NUM_DOMAINS		0x04
#define DVFS_DOMAINID_AT_INDEX		0x08
#define DVFS_HANDLER_TRANS_LATENCY	0x0C
#define DOMAIN_ID			0x10
#define PERF_LEVEL			0x14
#define PERF_LEVEL_ACK			0x18
#define NUM_OF_PERF_LEVELS		0x1C
#define PERF_LEVEL_TO_READ		0x20
#define FREQ_AT_PERF_LEVEL		0x24
#define VOLT_AT_PERF_LEVEL		0x28

#define TIME_OUT			100
#define GEM5_MAX_NUM_DOMAINS		32

#define GEM5_CLOCK_NAME_LEN		32

struct clk_energy_ctrl_opp_data {
	u32 *freqs;
	u32 *voltages;
	int n_opps;

	u32 transition_latency;
};

struct clk_energy_ctrl_base_data {
	int domain_id;
	int domain_index;
	void __iomem *baseaddr;
};

struct clk_energy_ctrl_domain_data {
	struct clk_energy_ctrl_base_data base;
	struct clk_energy_ctrl_opp_data opp;
};

struct clk_energy_ctrl {
	struct clk_hw hw;
	spinlock_t lock;

	struct clk_energy_ctrl_domain_data data;
};

#define to_clk_energy_ctrl(hw_ectrl) container_of(hw_ectrl, struct clk_energy_ctrl, hw)

static inline int read_wait_to(void __iomem *reg, int status, int timeout)
{
	while (timeout-- && readl(reg) != status) {
		cpu_relax();
		udelay(2);
	}
	if (!timeout)
		return -EAGAIN;
	else
		return 0;
}

static bool clk_energy_ctrl_initialized(struct clk_energy_ctrl *clk_ectrl)
{
	return (clk_ectrl->data.base.domain_index >= 0) &&
		(clk_ectrl->data.base.domain_id >= 0) &&
		!IS_ERR_OR_NULL(clk_ectrl->data.base.baseaddr);
}

static int clk_energy_ctrl_find_perf_index(struct clk_energy_ctrl *clk_ectrl, u32 freq)
{
	int index;
	for (index = 0; index < clk_ectrl->data.opp.n_opps; index++) {
		if (clk_ectrl->data.opp.freqs[index] == freq)
			return index;
	}

	return -EINVAL;
}

/* Return rate in Hz, not in kHz */
static unsigned long clk_energy_ctrl_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_energy_ctrl *clk_ectrl = to_clk_energy_ctrl(hw);
	u32 freq;
	int perf;

	if (!clk_energy_ctrl_initialized(clk_ectrl)) {
		pr_err("%s: clock '%s' is not initialized\n",
			__func__, clk_hw_get_name(hw));
		return -EIO;
	}

	if (!clk_ectrl->data.opp.freqs) {
		pr_err("%s: not populated clock '%s' frequencies\n",
			__func__, clk_hw_get_name(hw));
		return -EIO;
	}

	spin_lock(&clk_ectrl->lock);

	writel(clk_ectrl->data.base.domain_id, clk_ectrl->data.base.baseaddr + DOMAIN_ID);
	perf = readl(clk_ectrl->data.base.baseaddr + PERF_LEVEL);

	spin_unlock(&clk_ectrl->lock);

	freq = clk_ectrl->data.opp.freqs[perf];

	return freq * 1000;
}

static long clk_energy_ctrl_round_rate(struct clk_hw *hw, unsigned long drate,
		unsigned long *parent_rate)
{
	return drate;
}

/* Store rate in kHz instead of Hz */
static int clk_energy_ctrl_set_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	struct clk_energy_ctrl *clk_ectrl = to_clk_energy_ctrl(hw);
	int perf;
	int ret;

	if (!clk_energy_ctrl_initialized(clk_ectrl)) {
		pr_err("%s: clock '%s' is not initialized\n",
			__func__, clk_hw_get_name(hw));
		return -EIO;
	}

	if (!clk_ectrl->data.opp.freqs) {
		pr_err("%s: not populated clock '%s' frequencies\n",
			__func__, clk_hw_get_name(hw));
		return -EIO;
	}

	rate /= 1000;
	perf = clk_energy_ctrl_find_perf_index(clk_ectrl, rate);
	if (perf < 0)
		return -EINVAL;

	ret = 0;
	spin_lock(&clk_ectrl->lock);

	writel(clk_ectrl->data.base.domain_id, clk_ectrl->data.base.baseaddr + DOMAIN_ID);
	writel(perf, clk_ectrl->data.base.baseaddr + PERF_LEVEL);
	if (read_wait_to(clk_ectrl->data.base.baseaddr + PERF_LEVEL_ACK, 1, TIME_OUT))
		ret = -EAGAIN;

	spin_unlock(&clk_ectrl->lock);

	return ret;
}

static struct clk_ops clk_energy_ctrl_ops = {
	.recalc_rate = clk_energy_ctrl_recalc_rate,
	.round_rate = clk_energy_ctrl_round_rate,
	.set_rate = clk_energy_ctrl_set_rate,
};

static int clk_energy_ctrl_populate_opps(struct clk_energy_ctrl *clk_ectrl)
{
	int domain_id;
	u32 n_perf_levels = 0, i;
	int ret = 0;

	if (!clk_energy_ctrl_initialized(clk_ectrl)) {
		pr_err("%s: clk_energy_ctrl is not initialized\n", __func__);
		return -EIO;
	}

	spin_lock(&clk_ectrl->lock);

	domain_id = clk_ectrl->data.base.domain_id;
	writel(domain_id, clk_ectrl->data.base.baseaddr + DOMAIN_ID);
	if (readl(clk_ectrl->data.base.baseaddr + DOMAIN_ID) != domain_id) {
		ret = -EINVAL;
		goto err;
	}

	n_perf_levels = readl(clk_ectrl->data.base.baseaddr + NUM_OF_PERF_LEVELS);
	clk_ectrl->data.opp.n_opps = n_perf_levels;

	clk_ectrl->data.opp.freqs = kzalloc(sizeof(u32) *
				clk_ectrl->data.opp.n_opps, GFP_KERNEL);
	if (!clk_ectrl->data.opp.freqs) {
		ret = -ENOMEM;
		pr_err("%s: out of memory for frequencies allocation\n", __func__);
		goto err_free;
	}

	clk_ectrl->data.opp.voltages = kzalloc(sizeof(u32) *
				clk_ectrl->data.opp.n_opps, GFP_KERNEL);
	if (!clk_ectrl->data.opp.voltages) {

		ret = -ENOMEM;
		pr_err("%s: out of memory for voltages allocation\n", __func__);
		goto err_free;
	}

	for (i = 0; i < clk_ectrl->data.opp.n_opps; i++) {
		writel(i, clk_ectrl->data.base.baseaddr + PERF_LEVEL_TO_READ);
		clk_ectrl->data.opp.freqs[i] =
			readl(clk_ectrl->data.base.baseaddr + FREQ_AT_PERF_LEVEL);
		clk_ectrl->data.opp.voltages[i] =
			readl(clk_ectrl->data.base.baseaddr + VOLT_AT_PERF_LEVEL);
	}

	goto out;

err_free:
	kfree(clk_ectrl->data.opp.freqs);
	kfree(clk_ectrl->data.opp.voltages);
err:
	clk_ectrl->data.opp.n_opps = 0;
out:
	spin_unlock(&clk_ectrl->lock);

	return ret;
}

static int clk_energy_ctrl_populate_transition_latency(struct clk_energy_ctrl *clk_ectrl)
{
	if (!clk_energy_ctrl_initialized(clk_ectrl)) {
		pr_err("%s: clk_energy_ctrl is not initialized\n", __func__);
		return -EIO;
	}

	spin_lock(&clk_ectrl->lock);
	clk_ectrl->data.opp.transition_latency =
		readl(clk_ectrl->data.base.baseaddr + DVFS_HANDLER_TRANS_LATENCY);
	spin_unlock(&clk_ectrl->lock);

	return 0;
}

int clk_energy_ctrl_get_opp_table(struct clk *clk, u32 **freq_table, u32 **volt_table)
{
	struct clk_hw *clk_hw = __clk_get_hw(clk);
	struct clk_energy_ctrl *clk_ectrl = to_clk_energy_ctrl(clk_hw);

	if (!clk_ectrl || !clk_energy_ctrl_initialized(clk_ectrl) ||
		!freq_table || !volt_table)
		return -EINVAL;

	*freq_table = clk_ectrl->data.opp.freqs;
	*volt_table = clk_ectrl->data.opp.voltages;

	return clk_ectrl->data.opp.n_opps;
}
EXPORT_SYMBOL_GPL(clk_energy_ctrl_get_opp_table);

u32 clk_energy_ctrl_get_trans_latency(struct clk *clk)
{
	struct clk_hw *clk_hw = __clk_get_hw(clk);
	struct clk_energy_ctrl *clk_ectrl = to_clk_energy_ctrl(clk_hw);

	if (!clk_ectrl || !clk_energy_ctrl_initialized(clk_ectrl))
		return -EINVAL;

	return clk_ectrl->data.opp.transition_latency;
}
EXPORT_SYMBOL_GPL(clk_energy_ctrl_get_trans_latency);

int clk_energy_ctrl_fill_opp_table(struct clk *clk, struct device *dev)
{
	int i = -1, count;
	u32 *freq_table;
	u32 *volt_table; /* In micro volts */
	int ret;

	count = clk_energy_ctrl_get_opp_table(clk, &freq_table, &volt_table);

	if (!freq_table || !count) {
		pr_err("%s: clock '%s' returned invalid freq table",
			__func__, __clk_get_name(clk));
		return -EINVAL;
	}

	if (!volt_table || !count) {
		pr_err("%s: clock '%s' returned invalid voltage table",
			__func__, __clk_get_name(clk));
		return -EINVAL;
	}

	while (++i < count) {
		ret = dev_pm_opp_add(dev, freq_table[i] * 1000, volt_table[i]);
		if (ret) {
			dev_warn(dev, "%s: failed to add OPP freq %d, u-voltage %d, err: %d\n",
				 __func__, freq_table[i] * 1000, volt_table[i], ret);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(clk_energy_ctrl_fill_opp_table);

struct clk *clk_register_energy_ctrl(const char *name,
				struct clk_energy_ctrl_base_data *base_data)
{
	struct clk_init_data init;
	struct clk_energy_ctrl *clk_ectrl;
	struct clk *clk;
	void *ret_ptr = NULL;

	if (!name) {
		pr_err("%s: invalid name passed for clock with domain-id = %d\n",
			 __func__, base_data->domain_id);
		return ERR_PTR(-EINVAL);
	}

	clk_ectrl = kzalloc(sizeof(*clk_ectrl), GFP_KERNEL);
	if (!clk_ectrl) {
		pr_err("%s: out of memory for '%s' clock allocation\n",
			__func__, name);
		return ERR_PTR(-ENOMEM);
	}

	init.name = name;
	init.ops = &clk_energy_ctrl_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	init.num_parents = 0;

	clk_ectrl->hw.init = &init;
	clk_ectrl->data.base = *base_data;
	spin_lock_init(&clk_ectrl->lock);

	ret_ptr = ERR_PTR(clk_energy_ctrl_populate_opps(clk_ectrl));
	if (ret_ptr) {
		pr_err("%s: failed populating OPPs for clock %s\n",
			__func__, name);
		goto err_free;
	}

	ret_ptr = ERR_PTR(clk_energy_ctrl_populate_transition_latency(clk_ectrl));
	if (ret_ptr) {
		pr_err("%s: failed populating transition latency for clock %s\n",
			__func__, name);
		goto err_free;
	}

	clk = clk_register(NULL, &clk_ectrl->hw);
	if (IS_ERR_OR_NULL(clk)) {
		pr_err("%s: failed to register clock '%s'\n", __func__, name);
		goto err_free;
	}

	return clk;

err_free:
	pr_err("%s: clock registration of '%s' clock failed\n",
		__func__, name);
	kfree(clk_ectrl);

	return ret_ptr;
}

/*
 * Register clocks for each CPU cluster we can find in the DT.
 *
 * Ideally, we parse the information from the cpu-map child of the cpus node.
 * Failing that, we fall back to the old method using clusters.
 */
static void __init gem5_clk_of_register_energy_ctrl(struct device_node *ectrl_node)
{
	u32 n_domains;
	bool dvfs_handler_status;
	int domain_id;
	int domain_index;
	char name[GEM5_CLOCK_NAME_LEN + 1];

	void __iomem *baseaddr;
	struct clk *clk;
	struct clk_energy_ctrl_base_data base_data;

	baseaddr = of_iomap(ectrl_node, 0);
	if (!baseaddr) {
		pr_err("error on of_iomap() with '%s' device node\n",
			ectrl_node->name);
		return;
	}

	dvfs_handler_status = readl(baseaddr + DVFS_HANDLER_STATUS);
	if (!dvfs_handler_status) {
		pr_err("gem5 DVFS handler is disabled, failed clocks "
			"initialization\n");
		return;
	}

	n_domains = readl(baseaddr + DVFS_NUM_DOMAINS);
	if (n_domains > GEM5_MAX_NUM_DOMAINS) {
		pr_err("gem5 DVFS handler manages %u domains when maximum %u "
			"are supported\n", n_domains, DVFS_NUM_DOMAINS);
		return;
	}

	pr_info("gem5 DVFS handler is enabled, physaddr of controller is 0x%px, "
		"%d clock domains are going to be registered\n",
		baseaddr, n_domains);

	for (domain_index = 0; domain_index < n_domains; domain_index++) {
		writel(domain_index, baseaddr + DVFS_DOMAINID_AT_INDEX);
		domain_id = readl(baseaddr + DVFS_DOMAINID_AT_INDEX);

		base_data.domain_id = domain_id;
		base_data.domain_index = domain_index;
		base_data.baseaddr = baseaddr;

		snprintf(name, sizeof(name), "clk-gem5-domain.%x", domain_id);

		clk = clk_register_energy_ctrl(name, &base_data);
		if (IS_ERR_OR_NULL(clk)) {
			pr_err("failed registering clock %s\n", name);
			continue;
		}

		clk_register_clkdev(clk, NULL, name);

		pr_info("clock '%s' is successfully initialized\n", name);
	}
}
CLK_OF_DECLARE(energy_ctrl, "arm,gem5-energy-ctrl", gem5_clk_of_register_energy_ctrl);
