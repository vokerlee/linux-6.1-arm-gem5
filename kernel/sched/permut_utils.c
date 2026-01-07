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
#include <linux/perf_event.h>
#include <linux/kernel.h>
#include <linux/percpu-defs.h>
#include <linux/module.h>
#include <linux/permut.h>

u64 permut_data_of_pinned_event(struct permut_pinned_events *counter, int event_id)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids) {
		if (event_id == event_ids[index])
			return counter->data[index];
	}

	return 0;
}

u64 permut_calc_delta(u64 term_l, u64 term_r)
{
	if (term_l < term_r) { /* overflow */
		u64 tmp = PERMUT_EVENT_OVERFLOW - term_r;
		return term_l + tmp;
	} else {
		return term_l - term_r;
	}
}

void permut_calc_events_delta(struct permut_event_count *term_l,
			      struct permut_event_count *term_r,
			      struct permut_event_count *delta)
{
	int *pinned_event_ids = permut_get_pinned_event_ids();
	int *flex_event_ids = permut_get_flex_event_ids();
	int index, event_id;

	for_each_permut_pinned_event(index, pinned_event_ids) {
		delta->pinned_data.data[index] =
			permut_calc_delta(term_l->pinned_data.data[index],
					  term_r->pinned_data.data[index]);
	}

	for_each_permut_flex_event(index, flex_event_ids) {
		event_id = flex_event_ids[index];
		if (event_id == PERMUT_EVENT_GROUP_TERM)
			continue;

		delta->flex_data.data[index] =
			permut_calc_delta(term_l->flex_data.data[index],
					  term_r->flex_data.data[index]);
	}
}

void permut_clear_pinned_events(struct permut_pinned_events *term)
{
	memset(term, 0, sizeof(*term));
}

void permut_split_pinned_events(struct permut_pinned_events *term_l,
				struct permut_pinned_events *term_r,
				u64 ratio)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids) {
		term_l->data[index] = term_r->data[index] * PERMUT_SCALE_VALUE / ratio;
		term_r->data[index] = term_r->data[index] - term_l->data[index];
	}
}

void permut_merge_pinned_events(struct permut_pinned_events *term_l,
		        	struct permut_pinned_events *term_r)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids)
		term_l->data[index] += term_r->data[index];
}

void permut_assign_pinned_events(struct permut_pinned_events *term_l,
				 struct permut_pinned_events *term_r)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids)
		term_l->data[index] = term_r->data[index];
}

u64 permut_pinned_data_of_event(struct permut_pinned_events *data, int cpu, int event_id)
{
	int *event_ids = permut_get_pinned_event_ids();
	int index;

	for_each_permut_pinned_event(index, event_ids) {
		if (event_id == event_ids[index])
			return data->data[index];
	}

	return 0;
}
