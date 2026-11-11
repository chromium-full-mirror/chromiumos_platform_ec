/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "cros_cbi.h"
#include "hooks.h"
#include "led_lightbar.h"

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT cros_ec_cbi_ufsc_lightbar_selector

LOG_MODULE_REGISTER(cros_cbi_ufsc_lightbar, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Expected exactly 1 UFSC lightbar selector node");

#define SELECTOR_NODE DT_DRV_INST(0)

BUILD_ASSERT(DT_PROP_LEN(SELECTOR_NODE, selections) % 2 == 0,
	     "selections property must contain pairs of phandles");

/* Validate that selections alternates <&ufsc_value &golden_color_set>. */
#define VALIDATE_SELECTION(node_id, prop, idx)                                \
	BUILD_ASSERT(                                                         \
		(((idx) % 2 == 0) &&                                          \
		 DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(node_id, prop, idx),    \
				    cros_ec_cbi_ufsc_value)) ||               \
			(((idx) % 2 == 1) &&                                  \
			 (DT_NODE_HAS_COMPAT(DT_PARENT(DT_PHANDLE_BY_IDX(     \
						     node_id, prop, idx)),    \
					     cros_ec_lightbar_golden_sets) || \
			  DT_NODE_HAS_COMPAT(DT_PARENT(DT_PHANDLE_BY_IDX(     \
						     node_id, prop, idx)),    \
					     cros_ec_led_lightbar))),         \
		"selections must alternate <&ufsc_value &golden_color_set>");

DT_FOREACH_PROP_ELEM(SELECTOR_NODE, selections, VALIDATE_SELECTION)

/*
 * Resolve a target color set node to its index in lb_color_sets[]:
 * index 0 is lightbar0, and indices 1..N are the children of
 * lightbar-golden-sets.
 */
#define GOLDEN_SET_IDX(target_node)                            \
	COND_CODE_1(DT_NODE_HAS_COMPAT(DT_PARENT(target_node), \
				       cros_ec_led_lightbar),  \
		    (0), (DT_NODE_CHILD_IDX(target_node) + 1))

/* Helper to check a specific index in the selections array. */
#define CHECK_MATCH(node_id, prop, idx, matched_ptr)                           \
	COND_CODE_1(DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(node_id, prop, idx),  \
				       cros_ec_cbi_ufsc_value),                \
		    (/* UFSC-Value node: check if it matches. */               \
		     if (!*(matched_ptr) &&                                    \
			 cros_cbi_ufsc_check_match(CBI_UFSC_VALUE_ID(          \
				 DT_PHANDLE_BY_IDX(node_id, prop, idx)))) {    \
			     size_t set_idx = GOLDEN_SET_IDX(                  \
				     DT_PHANDLE_BY_IDX(node_id, prop,          \
						       UTIL_INC(idx)));        \
			     *(matched_ptr) = true;                            \
			     lb_select_golden_set(set_idx);                    \
			     LOG_INF("Lightbar: Selected golden set [%zu] %s", \
				     set_idx,                                  \
				     DT_NODE_FULL_NAME(DT_PHANDLE_BY_IDX(      \
					     node_id, prop, UTIL_INC(idx))));  \
		     }),                                                       \
		    (/* Color set node: skip. */                               \
		     ))

test_export_static void lb_ufsc_selector_init(void)
{
	bool matched = false;

	DT_FOREACH_PROP_ELEM_VARGS(SELECTOR_NODE, selections, CHECK_MATCH,
				   &matched);
	if (!matched) {
		lb_select_golden_set(0);
		LOG_INF("Lightbar: Using primary golden set (No UFSC match)");
	}
}
DECLARE_HOOK(HOOK_INIT, lb_ufsc_selector_init, HOOK_PRIO_POST_FIRST);
