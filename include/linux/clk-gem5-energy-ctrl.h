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

/* External interface for cpufreq and devfreq driver for adjusting OPPs of ClockDomains in Gem5 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/types.h>
#include <asm-generic/errno-base.h>

#if defined(CONFIG_CLK_GEM5_ENERGY_CTRL)

int clk_energy_ctrl_fill_opp_table(struct clk *clk, struct device *dev);
int clk_energy_ctrl_get_opp_table(struct clk *clk, u32 **freq_table, u32 **volt_table);
u32 clk_energy_ctrl_get_trans_latency(struct clk *clk);

#else

static inline
int clk_energy_ctrl_fill_opp_table(struct clk *clk, struct device *dev);
{
        return -EOPNOTSUPP;
}

static inline
int clk_energy_ctrl_get_opp_table(struct clk *clk, u32 **freq_table, u32 **volt_table)
{
        return -EOPNOTSUPP;
}

static inline
u32 clk_energy_ctrl_get_trans_latency(struct clk *clk)
{
        return -EOPNOTSUPP;
}

#endif
