/*
 *  linux/drivers/devfreq/gem5/gem5_devfreq.h
 *
 *  Copyright (c) 2024 Glaz Roman
 *
 *  Author: Glaz Roman <vokerlee@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/version.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/devfreq.h>
#include <linux/clk.h>
#include <linux/clk-gem5-energy-ctrl.h>

struct gem5_devfreq_pdata {
        unsigned int polling_ms;
        char *governor;
        void *governor_data;
};

struct gem5_devfreq_device {
        struct devfreq *devfreq;
        struct clk *clk;

        const struct gem5_devfreq_pdata *pdata;
};

struct gem5_devfreq_domain_data {
        char *clock_name;
        char *compatible_name;
        int domain_id;
};

int gem5_devfreq_probe(struct platform_device *pdev,
                        struct gem5_devfreq_domain_data *domain_data,
                        const struct of_device_id of_match[]);

int gem5_devfreq_remove(struct platform_device *pdev);

int gem5_devfreq_target(struct device *dev, unsigned long *freq, u32 flags);

int gem5_devfreq_get_dev_status(struct device *dev, struct devfreq_dev_status *stat);

int gem5_devfreq_get_cur_freq(struct device *dev, unsigned long *freq);
