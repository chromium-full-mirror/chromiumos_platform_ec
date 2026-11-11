/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_cbi.h"
#include "drivers/led.h"
#include "led_common.h"
#include "led_lightbar.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/shell/shell.h>
#include <zephyr/ztest.h>

static int mock_strip_update_rgb(const struct device *dev,
				 struct led_rgb *pixels, size_t num_pixels)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pixels);
	ARG_UNUSED(num_pixels);
	return 0;
}

static int mock_strip_update_channels(const struct device *dev,
				      uint8_t *channels, size_t num_channels)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channels);
	ARG_UNUSED(num_channels);
	return 0;
}

static const struct led_strip_driver_api mock_strip_api = {
	.update_rgb = mock_strip_update_rgb,
	.update_channels = mock_strip_update_channels,
};

DEVICE_DT_DEFINE(DT_NODELABEL(mock_strip), NULL, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &mock_strip_api);

int __weak extpower_is_present(void)
{
	return 1;
}

static int reset_policy_calls;

void __weak led_auto_control(enum ec_led_id led_id, int enable)
{
	ARG_UNUSED(led_id);
	ARG_UNUSED(enable);
}

int __weak led_auto_control_is_enabled(enum ec_led_id led_id)
{
	ARG_UNUSED(led_id);
	return 1;
}

void __weak reset_policy_patterns(enum ec_led_id led_id)
{
	if (led_id == EC_LED_ID_LIGHTBAR_LED) {
		reset_policy_calls++;
	}
}

#if DT_HAS_COMPAT_STATUS_OKAY(cros_ec_cbi_ufsc_lightbar_selector)
void lb_ufsc_selector_init(void);

static enum cbi_ufsc_value_id active_ufsc_match_id = CBI_UFSC_VALUE_COUNT;

bool __weak cros_cbi_ufsc_check_match(enum cbi_ufsc_value_id value_id)
{
	return value_id == active_ufsc_match_id;
}
#endif

#define EXPECTED_NUM_SEGMENTS \
	DT_PROP_LEN(DT_INST(0, cros_ec_led_lightbar), led_segments)
#define DYNAMIC_COLOR_IDX_BASE \
	DT_CHILD_NUM(DT_CHILD(DT_INST(0, cros_ec_led_lightbar), lightbar0))
#define WHITE_DIM_SRGB DT_PROP(DT_NODELABEL(lightbar_white_dim), srgb)

extern const struct led_driver_t PINS_NODE(DT_INST(0, cros_ec_led_lightbar));

static bool mock_takeover_active;
static bool mock_ec_busy;
static const struct led_pattern_node_t *mock_custom_pattern;

void lb_set_takeover(bool enable)
{
	mock_takeover_active = enable;
}

bool lb_is_takeover_active(void)
{
	return mock_takeover_active;
}

bool lb_is_ec_busy(void)
{
	return mock_ec_busy;
}

bool lb_is_custom_pattern(const struct led_pattern_node_t *pattern)
{
	return pattern == mock_custom_pattern;
}

ZTEST(lightbar, test_get_num_segments)
{
	zassert_equal(lb_get_num_segments(), EXPECTED_NUM_SEGMENTS,
		      "Expected %d segments from devicetree",
		      EXPECTED_NUM_SEGMENTS);
}

ZTEST(lightbar, test_lookup_pins_node_srgb)
{
	struct led_rgb dim_white = {
		.r = (WHITE_DIM_SRGB >> 16) & 0xFF,
		.g = (WHITE_DIM_SRGB >> 8) & 0xFF,
		.b = WHITE_DIM_SRGB & 0xFF,
	};
	int num_segments = lb_get_num_segments();
	int idx;

	idx = lb_lookup_pins_node_srgb(&dim_white, SEGMENT_ID_ALL);
	zassert_true(idx >= 0 && idx < DYNAMIC_COLOR_IDX_BASE,
		     "Expected built-in color_idx for SEGMENT_ID_ALL, got %d",
		     idx);

	for (int8_t seg = 0; seg < num_segments; seg++) {
		idx = lb_lookup_pins_node_srgb(&dim_white, seg);
		zassert_true(
			idx >= 0 && idx < DYNAMIC_COLOR_IDX_BASE,
			"Expected built-in color_idx for segment %d, got %d",
			seg, idx);
	}
	zassert_equal(lb_lookup_pins_node_srgb(&dim_white, num_segments), -1,
		      "Expected out-of-bounds segment %d to fail",
		      num_segments);
}

ZTEST(lightbar, test_golden_sets)
{
#if DT_HAS_COMPAT_STATUS_OKAY(cros_ec_lightbar_golden_sets)
	const struct led_rgb srgb_white = { 0xFF, 0xFF, 0xFF };
	const struct led_rgb srgb_red = { 0xEA, 0x43, 0x35 };
	const struct led_rgb srgb_green_65 = { 33, 109, 53 };
	const struct led_rgb srgb_white_dim = { 109, 109, 109 };
	const struct led_rgb srgb_custom = { 0x80, 0x40, 0x20 };
	const struct led_rgb *hw;
	int dyn_idx_0;
	int dyn_idx_1;

	zassert_equal(lb_get_num_golden_sets(), 2);
	zassert_equal(lb_get_active_golden_set(), 0);
	zassert_mem_equal(lb_get_golden_set_name(0), "lightbar0", 10);
	zassert_mem_equal(lb_get_golden_set_name(1), "pacific", 8);
	zassert_is_null(lb_get_golden_set_name(2));

	/* Verify UFSC selector hook (lb_ufsc_selector_init) */
	active_ufsc_match_id = CBI_UFSC_VALUE_ID(DT_NODELABEL(ufsc_pacific));
	lb_ufsc_selector_init();
	zassert_equal(lb_get_active_golden_set(), 1);

	active_ufsc_match_id =
		CBI_UFSC_VALUE_ID(DT_NODELABEL(ufsc_cashmere_dark));
	lb_ufsc_selector_init();
	zassert_equal(lb_get_active_golden_set(), 0);

	/* Verify primary set 0 (cashmere) pre-calculated hardware RGB colors */
	hw = lb_srgb_to_hw_rgb(&srgb_white, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 0xFF);
	zassert_equal(hw->g, 0xC7);
	zassert_equal(hw->b, 0x95);

	hw = lb_srgb_to_hw_rgb(&srgb_red, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 0xFF);
	zassert_equal(hw->g, 0x25);
	zassert_equal(hw->b, 0x0E);

	hw = lb_srgb_to_hw_rgb(&srgb_green_65, 0);
	zassert_not_null(hw);
	zassert_equal(hw->r, 44);
	zassert_equal(hw->g, 99);
	zassert_equal(hw->b, 17);

	hw = lb_srgb_to_hw_rgb(&srgb_white_dim, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 40);
	zassert_equal(hw->g, 31);
	zassert_equal(hw->b, 23);

	/* Populate dynamic conversion cache in set 0 (slot 0 + slot 1) */
	dyn_idx_0 = lb_lookup_pins_node_srgb(&srgb_custom, 0);
	zassert_equal(dyn_idx_0, DYNAMIC_COLOR_IDX_BASE);
	dyn_idx_1 = lb_lookup_pins_node_srgb(&srgb_custom, 1);
	zassert_equal(dyn_idx_1, DYNAMIC_COLOR_IDX_BASE + 1);

	/* Switch to alternative set 1 (pacific) */
	zassert_equal(lb_select_golden_set(1), EC_SUCCESS);
	zassert_equal(lb_get_active_golden_set(), 1);

	/* Verify dynamic conversion cache was cleared on set switch */
	dyn_idx_0 = lb_lookup_pins_node_srgb(&srgb_custom, 2);
	zassert_equal(dyn_idx_0, DYNAMIC_COLOR_IDX_BASE);

	hw = lb_srgb_to_hw_rgb(&srgb_white, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 0xFF);
	zassert_equal(hw->g, 0xA7);
	zassert_equal(hw->b, 0x55);

	hw = lb_srgb_to_hw_rgb(&srgb_red, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 0xFF);
	zassert_equal(hw->g, 0x21);
	zassert_equal(hw->b, 0x09);

	hw = lb_srgb_to_hw_rgb(&srgb_green_65, 0);
	zassert_not_null(hw);
	zassert_equal(hw->r, 55);
	zassert_equal(hw->g, 99);
	zassert_equal(hw->b, 11);

	hw = lb_srgb_to_hw_rgb(&srgb_white_dim, SEGMENT_ID_ALL);
	zassert_not_null(hw);
	zassert_equal(hw->r, 40);
	zassert_equal(hw->g, 26);
	zassert_equal(hw->b, 13);

	/* Out-of-range index should fail */
	zassert_equal(lb_select_golden_set(2), EC_ERROR_INVAL);

	/* Verify 'lb golden' shell command */
	reset_policy_calls = 0;
	zassert_ok(shell_execute_cmd(NULL, "lb golden"));
	zassert_ok(shell_execute_cmd(NULL, "lb golden 0"));
	zassert_equal(lb_get_active_golden_set(), 0);
	zassert_equal(reset_policy_calls, 1);

	zassert_ok(shell_execute_cmd(NULL, "lb golden 1"));
	zassert_equal(lb_get_active_golden_set(), 1);
	zassert_equal(reset_policy_calls, 2);

	zassert_not_equal(shell_execute_cmd(NULL, "lb golden 2"), 0);
	zassert_not_equal(shell_execute_cmd(NULL, "lb golden bad"), 0);

	/* Restore set 0 */
	zassert_equal(lb_select_golden_set(0), EC_SUCCESS);
#else
	zassert_equal(lb_get_num_golden_sets(), 0);
	zassert_equal(lb_get_active_golden_set(), -1);
	zassert_is_null(lb_get_golden_set_name(0));
	zassert_equal(lb_select_golden_set(0), EC_ERROR_UNIMPLEMENTED);
	zassert_ok(shell_execute_cmd(NULL, "lb golden"));
#endif
}

ZTEST(lightbar, test_takeover_pattern_suppression)
{
	const struct led_driver_api *api =
		PINS_NODE(DT_INST(0, cros_ec_led_lightbar)).api;
	struct led_rgb dim_white_srgb = {
		.r = (WHITE_DIM_SRGB >> 16) & 0xFF,
		.g = (WHITE_DIM_SRGB >> 8) & 0xFF,
		.b = WHITE_DIM_SRGB & 0xFF,
	};
	const struct led_rgb *dim_white_hw =
		lb_srgb_to_hw_rgb(&dim_white_srgb, SEGMENT_ID_ALL);
	int dim_white_idx =
		lb_lookup_pins_node_srgb(&dim_white_srgb, SEGMENT_ID_ALL);
	struct pattern_color_node_t step_color = {
		.color_idx = (uint8_t)dim_white_idx,
		.duration_ms = 0,
	};
	struct led_pattern_node_t dt_pattern = {
		.led_id = EC_LED_ID_LIGHTBAR_LED,
		.transition = LED_TRANSITION_STEP,
		.cycle_limit = 1,
		.pattern_len = 1,
		.cur_color = 0,
		.elapsed_ms = 0,
		.pattern_color = &step_color,
	};
	struct led_pattern_node_t custom_pattern = dt_pattern;
	struct led_rgb zero_rgb = { 0, 0, 0 };

	zassert_not_null(dim_white_hw);
	zassert_true(dim_white_idx >= 0);

	/* 1. Clear LEDs to off */
	lb_set_and_apply_rgb(zero_rgb, SEGMENT_ID_ALL);
	zassert_equal(lightbar_buf[0].r, 0);

	/* 2. Takeover active, not busy: DT policy pattern is suppressed */
	mock_takeover_active = true;
	mock_ec_busy = false;
	mock_custom_pattern = &custom_pattern;

	api->set_color_with_pattern(&dt_pattern);
	api->asynchronous_apply_color(false);
	zassert_equal(lightbar_buf[0].r, 0,
		      "DT policy pattern should be suppressed during takeover");

	/* 3. Takeover active, not busy: Custom host pattern is applied */
	api->set_color_with_pattern(&custom_pattern);
	api->asynchronous_apply_color(false);
	zassert_equal(lightbar_buf[0].r, dim_white_hw->r,
		      "Custom pattern should be applied during takeover");

	/* 4. Takeover active, EC busy (e.g. boot animation): DT pattern runs */
	lb_set_and_apply_rgb(zero_rgb, SEGMENT_ID_ALL);
	mock_ec_busy = true;
	api->set_color_with_pattern(&dt_pattern);
	api->asynchronous_apply_color(false);
	zassert_equal(lightbar_buf[0].r, dim_white_hw->r,
		      "DT pattern should run while EC one-shot is busy");

	/* 5. Verify 'lb auto' shell command */
	zassert_ok(shell_execute_cmd(NULL, "lb auto"));
	zassert_ok(shell_execute_cmd(NULL, "lb auto host"));
	zassert_true(lb_is_takeover_active());
	zassert_ok(shell_execute_cmd(NULL, "lb auto on"));
	zassert_false(lb_is_takeover_active());
	zassert_ok(shell_execute_cmd(NULL, "lb auto off"));
	zassert_false(lb_is_takeover_active());
	zassert_not_equal(shell_execute_cmd(NULL, "lb auto invalid"), 0);

	/* Reset mock state */
	mock_takeover_active = false;
	mock_ec_busy = false;
	mock_custom_pattern = NULL;
}

ZTEST_SUITE(lightbar, NULL, NULL, NULL, NULL, NULL);
