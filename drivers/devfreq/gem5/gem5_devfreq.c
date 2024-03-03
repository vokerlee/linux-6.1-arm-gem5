/*
 *  linux/drivers/devfreq/gem5/gem5_devfreq.c
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

#define pr_fmt(fmt) "gem5_devfreq: " fmt

#include <linux/version.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/devfreq.h>
#include <linux/clk.h>
#include "gem5_devfreq.h"
#include "../governor.h"

static struct devfreq_dev_profile gem5_devfreq_profile = {
        .polling_ms     = 0,
        .target         = gem5_devfreq_target,
        .get_dev_status = gem5_devfreq_get_dev_status,
        .get_cur_freq   = gem5_devfreq_get_cur_freq
};

int gem5_devfreq_probe(struct platform_device *pdev,
                        struct gem5_devfreq_domain_data *domain_data,
                        const struct of_device_id of_match[])
{
        const struct of_device_id *match;
        struct clk *clk;
        struct device *dev = &pdev->dev;
        struct gem5_devfreq_device *ddev = NULL;
        int ret;

        ddev = devm_kzalloc(dev, sizeof(*ddev), GFP_KERNEL);
        if (!ddev)
                return -ENOMEM;

        match = of_match_device(of_match, dev);
        ddev->pdata = match->data;

        clk = clk_get_sys(domain_data->clock_name, NULL);
        if (IS_ERR_OR_NULL(clk)) {
                dev_err(dev, "%s: failed to get clock '%s'\n", __func__,
                        domain_data->clock_name);
                return -ENOENT;
        }

        ddev->clk = clk;

        ret = clk_energy_ctrl_fill_opp_table(clk, dev);
        if (ret) {
                dev_err(dev, "%s: failed to fill OPP table '%s'\n", __func__,
                        domain_data->clock_name);
                return -ENOENT;
        }

        gem5_devfreq_profile.polling_ms = ddev->pdata->polling_ms;

        ddev->devfreq = devm_devfreq_add_device(dev, &gem5_devfreq_profile,
                                                ddev->pdata->governor,
                                                ddev->pdata->governor_data);
        if (IS_ERR_OR_NULL(ddev->devfreq)) {
                dev_err(dev, "%s: failed to create devfreq device with clock'%s', "
                        "err: %ld\n",
                        __func__, domain_data->clock_name, PTR_ERR(ddev->devfreq));
                return -ENODEV;
        }

        platform_set_drvdata(pdev, ddev);
        dev_info(dev, "device with clock '%s' is successfully initialized",
                domain_data->clock_name);

        return 0;
};

int gem5_devfreq_remove(struct platform_device *pdev)
{
        struct gem5_devfreq_device *ddev;
        ddev = platform_get_drvdata(pdev);

        platform_set_drvdata(pdev, NULL);
        devfreq_remove_device(ddev->devfreq);

        return 0;
}

int gem5_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
        struct platform_device *pdev =
                container_of(dev, struct platform_device, dev);
	struct gem5_devfreq_device *ddev = platform_get_drvdata(pdev);
        struct dev_pm_opp *opp = NULL;

        if (!ddev->clk) {
                dev_err(dev, "%s: no clock for operation\n", __func__);
                return -ENODEV;
        }

        rcu_read_lock();
        opp = devfreq_recommended_opp(dev, freq, flags);
        rcu_read_unlock();

        if (IS_ERR_OR_NULL(opp)) {
                dev_err(dev, "%s: failed to get OPP for freq %ld, "
                        "err: %ld, flags: %u\n",
                        __func__, *freq, PTR_ERR(opp), flags);
                return PTR_ERR(opp);
        }

        return clk_set_rate(ddev->clk, *freq);
}

/* Don't support device status */
int gem5_devfreq_get_dev_status(struct device *dev, struct devfreq_dev_status *stat)
{
        return 0;
}

int gem5_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
        struct platform_device *pdev =
                container_of(dev, struct platform_device, dev);
	struct gem5_devfreq_device *ddev = platform_get_drvdata(pdev);

        if (!ddev->clk) {
                dev_err(dev, "%s: no clock for operation\n", __func__);
                return -ENODEV;
        }

        *freq = clk_get_rate(ddev->clk);
        if (unlikely(*freq == 0)) {
                return -EINVAL;
        }

        return 0;
}
