/*
 *  linux/drivers/devfreq/gem5/gem5_ddrfreq.c
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

#define pr_fmt(fmt) "gem5_ddrfreq: " fmt

#include "linux/export.h"
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

static struct gem5_devfreq_pdata ddrfreq_data = {
        .polling_ms     = 0,
        .governor       = DEVFREQ_GOV_USERSPACE,
        .governor_data  = NULL
};

static struct gem5_devfreq_domain_data ddrfreq_domain = {
        .domain_id              = 0x400,
        .clock_name             = "clk-gem5-domain.400",
        .compatible_name        = "gem5-clock-domain,id400"
};

static const struct of_device_id ddrfreq_of_match[] = {
        {
                .compatible     = "gem5-clock-domain,id400",
                .data           = &ddrfreq_data
        }
};
MODULE_DEVICE_TABLE(of, ddrfreq_of_match);

static int gem5_ddrfreq_probe(struct platform_device *pdev)
{
        return gem5_devfreq_probe(pdev, &ddrfreq_domain, ddrfreq_of_match);
}

static int gem5_ddrfreq_remove(struct platform_device *pdev)
{
        return gem5_devfreq_remove(pdev);
}

static struct platform_driver gem5_ddrfreq_driver = {
        .probe  = gem5_ddrfreq_probe,
        .remove = gem5_ddrfreq_remove,
        .driver = {
                .name           = "gem5_ddrfreq",
                .owner          = THIS_MODULE,
                .of_match_table = of_match_ptr(ddrfreq_of_match)
        }
};

module_platform_driver(gem5_ddrfreq_driver);

MODULE_AUTHOR("Roman Glaz <vokerlee@gmail.com>");
MODULE_DESCRIPTION("Gem5 devfreq ddrfreq driver");
MODULE_LICENSE("GPL v2");
