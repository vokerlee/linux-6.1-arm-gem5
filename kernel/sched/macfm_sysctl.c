/*
 * SPDX-License-Identifier: GPL-2.0
 * Memory Aware CPU Frequency Manager (MACFM) is to control
 * CPU frequency based on memory architecture awareness via PMU
 * events.
 *
 * Copyright (C) 2024 Roman Glaz
 * Author: Roman Glaz <vokerlee@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/macfm.h>

unsigned int sysctl_macfm_enable = 0;
unsigned int sysctl_macfm_windows_num = MACFM_DEFAULT_WINDOWS_NUM;

struct macfm_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj,
			struct kobj_attribute *kattr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *kattr,
			 const char *buf, size_t count);
	int *value;
};

static ssize_t macfm_show(struct kobject *kobj,
			  struct kobj_attribute *kattr, char *buf)
{
	struct macfm_attr *macfm_attr;
	int temp;

	macfm_attr = container_of(&kattr->attr, struct macfm_attr, attr);
	temp = *(macfm_attr->value);

	return sprintf(buf, "%d\n", temp);
}

static ssize_t macfm_store(struct kobject *kobj, struct kobj_attribute *kattr,
			   const char *buf, size_t count)
{
	struct macfm_attr *macfm_attr;
	ssize_t ret;
	int temp;

	ret = count;
	macfm_attr = container_of(&kattr->attr, struct macfm_attr, attr);

	if (kstrtoint(buf, 10, &temp))
		return -EINVAL;

	if (temp < 0)
		ret = -EINVAL;
	else
		*(macfm_attr->value) = temp;

	return ret;
}

static ssize_t macfm_enable_show(struct kobject *kobj,
				 struct kobj_attribute *kattr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", sysctl_macfm_enable);
}

static ssize_t macfm_enable_store(struct kobject *kobj,
				  struct kobj_attribute *kattr, const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	if (kstrtouint(buf, 10, &val))
		return -EINVAL;

	if (val) {
		ret = macfm_start();
		if (!ret) {
			on_each_cpu(macfm_set_window_start, NULL, 1);
		}
	}
	else
		ret = macfm_stop();

	return ret ?: count;
}

/*
 * Note:
 * The field 'value' of macfm_attr point to int, don't use type shorter than int.
 */
static struct macfm_attr attrs[] = {
	{
		.attr = { .name = "enabled", .mode = 0640, },
		.show = macfm_enable_show,
		.store = macfm_enable_store,
		.value = NULL,
	},
	{
		.attr = { .name = "windows_num", .mode = 0640, },
		.show = macfm_show,
		.store = macfm_store,
		.value = &sysctl_macfm_windows_num,
	}
};

struct macfm_data_struct {
	struct attribute_group attr_group;
	struct attribute *attributes[ARRAY_SIZE(attrs) + 1];
} macfm_data;

static int macfm_attr_init(void)
{
	int i, nr_attr;

	nr_attr = ARRAY_SIZE(attrs);
	for (i = 0; i < nr_attr; i++)
		macfm_data.attributes[i] = &attrs[i].attr;
	macfm_data.attributes[i] = NULL;

	macfm_data.attr_group.name = "macfm";
	macfm_data.attr_group.attrs = macfm_data.attributes;

	return sysfs_create_group(kernel_kobj, &macfm_data.attr_group);
}
late_initcall(macfm_attr_init);
