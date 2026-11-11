/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cros_cbi.h"
#include "drivers/led.h"
#include "hooks.h"
#include "led_common.h"
#include "led_lb_util.h"
#include "led_lightbar.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
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
static bool mock_auto_control_enabled = true;

void __weak led_auto_control(enum ec_led_id led_id, int enable)
{
	if (led_id == EC_LED_ID_LIGHTBAR_LED) {
		mock_auto_control_enabled = (enable != 0);
	}
}

int __weak led_auto_control_is_enabled(enum ec_led_id led_id)
{
	ARG_UNUSED(led_id);
	return mock_auto_control_enabled ? 1 : 0;
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
#define HAS_LED_WHITE DT_NODE_HAS_PROP(DT_NODELABEL(lightbar_white), led_color)

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
}

static void lightbar_after(void *fixture)
{
	const struct led_rgb zero_rgb = { 0, 0, 0 };

	ARG_UNUSED(fixture);

	mock_takeover_active = false;
	mock_ec_busy = false;
	mock_custom_pattern = NULL;
	mock_auto_control_enabled = true;
#if DT_HAS_COMPAT_STATUS_OKAY(cros_ec_lightbar_golden_sets)
	lb_select_golden_set(0);
#endif
	lb_set_brightness_pct(lb_data.brightness_default_pct);
	lb_set_and_apply_rgb(zero_rgb, SEGMENT_ID_ALL);
}

ZTEST(lightbar, test_shell_commands)
{
	char cmd[64];
	struct led_rgb dim_white_srgb = {
		.r = (WHITE_DIM_SRGB >> 16) & 0xFF,
		.g = (WHITE_DIM_SRGB >> 8) & 0xFF,
		.b = WHITE_DIM_SRGB & 0xFF,
	};
	const struct led_rgb *dim_white_hw =
		lb_srgb_to_hw_rgb(&dim_white_srgb, SEGMENT_ID_ALL);

	zassert_not_null(dim_white_hw);

	/* Verify 'lb count'. */
	zassert_ok(shell_execute_cmd(NULL, "lb count"));

	/* Verify 'lb set' with valid and invalid arguments. */
	zassert_ok(shell_execute_cmd(NULL, "lb set 10 20 30"));
	zassert_false(mock_auto_control_enabled);
	zassert_equal(lightbar_buf[0].r, 10);
	zassert_equal(lightbar_buf[0].g, 20);
	zassert_equal(lightbar_buf[0].b, 30);
	zassert_equal(lightbar_buf[lb_data.num_leds - 1].r, 10);
	zassert_equal(lightbar_buf[lb_data.num_leds - 1].g, 20);
	zassert_equal(lightbar_buf[lb_data.num_leds - 1].b, 30);
	zassert_not_equal(shell_execute_cmd(NULL, "lb set 10 20"), 0);

	/* Verify 'lb set_srgb' with valid, unmapped, and invalid arguments. */
	mock_auto_control_enabled = true;
	snprintf(cmd, sizeof(cmd), "lb set_srgb %d %d %d", dim_white_srgb.r,
		 dim_white_srgb.g, dim_white_srgb.b);
	zassert_ok(shell_execute_cmd(NULL, cmd));
	zassert_false(mock_auto_control_enabled);
	zassert_equal(lightbar_buf[0].r, dim_white_hw->r);
	zassert_equal(lightbar_buf[0].g, dim_white_hw->g);
	zassert_equal(lightbar_buf[0].b, dim_white_hw->b);
	zassert_not_equal(shell_execute_cmd(NULL, "lb set_srgb 1 2 3"), 0);
	zassert_not_equal(shell_execute_cmd(NULL, "lb set_srgb 1 2"), 0);

	/* Verify 'lb set_single' and 'lb get'. */
	zassert_ok(shell_execute_cmd(NULL, "lb set_single 2 11 22 33"));
	zassert_equal(lightbar_buf[2].r, 11);
	zassert_equal(lightbar_buf[2].g, 22);
	zassert_equal(lightbar_buf[2].b, 33);
	zassert_equal(lightbar_buf[0].r, dim_white_hw->r);
	zassert_ok(shell_execute_cmd(NULL, "lb get"));
	snprintf(cmd, sizeof(cmd), "lb set_single %u 1 2 3", lb_data.num_leds);
	zassert_not_equal(shell_execute_cmd(NULL, cmd), 0);
	zassert_not_equal(shell_execute_cmd(NULL, "lb set_single 0 1 2"), 0);

	/* Verify 'lb br' get, set, and out-of-range rejection. */
	zassert_ok(shell_execute_cmd(NULL, "lb br"));
	zassert_ok(shell_execute_cmd(NULL, "lb br 50"));
	zassert_equal(lb_get_brightness_pct(), 50);
	zassert_not_equal(shell_execute_cmd(NULL, "lb br 101"), 0);
	zassert_ok(shell_execute_cmd(NULL, "lb br 100"));
	zassert_equal(lb_get_brightness_pct(), 100);

	/* Verify 'lb auto' across disabled, host, enabled, and invalid args. */
	zassert_ok(shell_execute_cmd(NULL, "lb auto off"));
	zassert_false(mock_auto_control_enabled);
	zassert_false(lb_is_takeover_active());
	zassert_ok(shell_execute_cmd(NULL, "lb auto"));

	zassert_ok(shell_execute_cmd(NULL, "lb auto host"));
	zassert_true(lb_is_takeover_active());
	zassert_true(mock_auto_control_enabled);
	zassert_ok(shell_execute_cmd(NULL, "lb auto"));

	zassert_ok(shell_execute_cmd(NULL, "lb auto on"));
	zassert_false(lb_is_takeover_active());
	zassert_true(mock_auto_control_enabled);
	zassert_ok(shell_execute_cmd(NULL, "lb auto"));

	zassert_not_equal(shell_execute_cmd(NULL, "lb auto invalid"), 0);
}

ZTEST(lightbar, test_led_driver_api)
{
	const struct led_driver_api *api =
		PINS_NODE(DT_INST(0, cros_ec_led_lightbar)).api;
	uint8_t br[EC_LED_COLOR_COUNT];
	struct led_rgb readback;

	/* Verify enum-based color node lookup. */
#if HAS_LED_WHITE
	zassert_not_null(lb_lookup_pins_node(LED_WHITE));
#else
	zassert_is_null(lb_lookup_pins_node(LED_WHITE));
#endif
	zassert_is_null(lb_lookup_pins_node(LED_COLOR_COUNT));

	/* Verify get_brightness_range for mismatched and matching LED IDs. */
	memset(br, 0xA5, sizeof(br));
	api->get_brightness_range(EC_LED_ID_BATTERY_LED, br);
	zassert_equal(br[EC_LED_COLOR_WHITE], 0xA5);

	api->get_brightness_range(EC_LED_ID_LIGHTBAR_LED, br);
#if HAS_LED_WHITE
	zassert_equal(br[EC_LED_COLOR_WHITE], 100);
#else
	zassert_equal(br[EC_LED_COLOR_WHITE], 0);
#endif
	zassert_equal(br[EC_LED_COLOR_AMBER], 0);

	/* Verify set_color for matching, mismatched, and LED_OFF. */
	api->set_color(LED_WHITE, EC_LED_ID_LIGHTBAR_LED, 50);
	zassert_equal(lb_get_brightness_pct(), 50);
#if HAS_LED_WHITE
	zassert_true(lightbar_buf[0].r > 0);
	zassert_true(lightbar_buf[0].g > 0);
	zassert_true(lightbar_buf[0].b > 0);
#endif

	api->set_color(LED_WHITE, EC_LED_ID_BATTERY_LED, 25);
	zassert_equal(lb_get_brightness_pct(), 50);

	api->set_color(LED_OFF, EC_LED_ID_LIGHTBAR_LED, 100);
	zassert_equal(lb_get_brightness_pct(), 100);
	zassert_equal(lightbar_buf[0].r, 0);
	zassert_equal(lightbar_buf[0].g, 0);
	zassert_equal(lightbar_buf[0].b, 0);

	/* Verify set_brightness error, priority selection, and all-zero off. */
	memset(br, 0, sizeof(br));
	zassert_equal(api->set_brightness(EC_LED_ID_BATTERY_LED, br),
		      EC_ERROR_INVAL);

	br[EC_LED_COLOR_RED] = 40;
	br[EC_LED_COLOR_WHITE] = 80;
	zassert_equal(api->set_brightness(EC_LED_ID_LIGHTBAR_LED, br),
		      EC_SUCCESS);
	zassert_equal(lb_get_brightness_pct(), 80);
#if HAS_LED_WHITE
	zassert_true(lightbar_buf[0].r > 0);
	zassert_true(lightbar_buf[0].g > 0);
	zassert_true(lightbar_buf[0].b > 0);
#else
	lb_set_and_apply_rgb((struct led_rgb){ 10, 20, 30 }, SEGMENT_ID_ALL);
#endif

	memset(br, 0, sizeof(br));
	zassert_equal(api->set_brightness(EC_LED_ID_LIGHTBAR_LED, br),
		      EC_SUCCESS);
	zassert_equal(lightbar_buf[0].r, 0);
	zassert_equal(lightbar_buf[0].g, 0);
	zassert_equal(lightbar_buf[0].b, 0);

	/* Verify lb_get_rgb and lb_set_brightness_pct clamping. */
	lb_set_and_apply_rgb((struct led_rgb){ 100, 120, 140 }, SEGMENT_ID_ALL);
	lb_get_rgb(&readback);
	zassert_equal(readback.r, 100);
	zassert_equal(readback.g, 120);
	zassert_equal(readback.b, 140);

	lb_set_brightness_pct(150);
	zassert_equal(lb_get_brightness_pct(), 100);
	zassert_equal(lightbar_buf[0].r, 100);

	lb_set_brightness_pct(50);
	zassert_equal(lb_get_brightness_pct(), 50);
	zassert_equal(lightbar_buf[0].r, 50);

	/* Verify chipset resume hook restores default brightness. */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(lb_get_brightness_pct(), lb_data.brightness_default_pct);
}

ZTEST(lightbar, test_idw_calculation)
{
	const struct lb_calibration_point points[] = {
		{ .srgb = { 0, 0, 0 }, .color = { 0xFF, 0xFF, 0xFF } },
		{ .srgb = { 0xFF, 0xFF, 0xFF }, .color = { 0xFF, 0xC0, 0x80 } },
		{ .srgb = { 0xFF, 0x00, 0x00 }, .color = { 0xFF, 0x20, 0x10 } },
	};
	struct led_rgb out = { 0xAA, 0xBB, 0xCC };

	/* Zero-input sRGB outputs black. */
	zassert_equal(lb_calculate_idw((struct led_rgb){ 0, 0, 0 }, &out,
				       points, ARRAY_SIZE(points)),
		      EC_SUCCESS);
	zassert_equal(out.r, 0);
	zassert_equal(out.g, 0);
	zassert_equal(out.b, 0);

	/* Scaled white snaps directly to the white calibration point. */
	zassert_equal(lb_calculate_idw((struct led_rgb){ 0x7F, 0x7F, 0x7F },
				       &out, points, ARRAY_SIZE(points)),
		      EC_SUCCESS);
	zassert_equal(out.r, 127);
	zassert_equal(out.g, 95);
	zassert_equal(out.b, 63);

	/* Off-hue input interpolates across valid points and skips zero ref. */
	zassert_equal(lb_calculate_idw((struct led_rgb){ 0xC0, 0x40, 0x40 },
				       &out, points, ARRAY_SIZE(points)),
		      EC_SUCCESS);
	zassert_true(out.r > out.g);
	zassert_true(out.g > out.b);
	zassert_true(out.b > 0);

	/* Empty calibration table returns EC_ERROR_INVAL. */
	zassert_equal(lb_calculate_idw((struct led_rgb){ 0x80, 0x40, 0x20 },
				       &out, points, 0),
		      EC_ERROR_INVAL);
}

ZTEST(lightbar, test_srgb_dynamic_cache)
{
	const struct led_driver_api *api =
		PINS_NODE(DT_INST(0, cros_ec_led_lightbar)).api;
	const struct led_rgb unknown_srgb = { 0x12, 0x34, 0x56 };
	const struct led_rgb cached_srgb = { 0x40, 0x80, 0xC0 };
	struct pattern_color_node_t step = { .duration_ms = 0 };
	struct led_pattern_node_t pattern = {
		.led_id = EC_LED_ID_LIGHTBAR_LED,
		.transition = LED_TRANSITION_STEP,
		.cycle_limit = 1,
		.pattern_len = 1,
		.cur_color = 0,
		.elapsed_ms = 0,
		.pattern_color = &step,
	};
	struct led_rgb prev;
#if HAS_LED_WHITE
	int dyn_idx;
#endif

	/* Verify negative segment ID and unknown static sRGB lookup. */
	zassert_equal(lb_lookup_pins_node_srgb(&unknown_srgb, -2), -1);
	zassert_is_null(lb_srgb_to_hw_rgb(&unknown_srgb, SEGMENT_ID_ALL));

	/* Verify single-segment and out-of-range segment RGB updates. */
	lb_set_and_apply_rgb((struct led_rgb){ 5, 6, 7 }, 0);
	zassert_equal(lightbar_buf[0].r, 5);
	zassert_equal(lightbar_buf[0].g, 6);
	zassert_equal(lightbar_buf[0].b, 7);
	zassert_equal(lightbar_buf[1].r, 0);
	lb_set_and_apply_rgb((struct led_rgb){ 8, 9, 10 },
			     lb_get_num_segments());
	zassert_equal(lightbar_buf[0].r, 5);
	zassert_equal(lightbar_buf[1].r, 0);

	/* Verify dynamic conversion cache behavior. */
	lb_set_and_apply_rgb((struct led_rgb){ 1, 2, 3 }, SEGMENT_ID_ALL);
#if HAS_LED_WHITE
	dyn_idx = lb_lookup_pins_node_srgb(&cached_srgb, 0);
	zassert_true(dyn_idx >= DYNAMIC_COLOR_IDX_BASE);
	zassert_equal(lb_lookup_pins_node_srgb(&cached_srgb, 0), dyn_idx);

	/* Apply dynamic color on segment 0 (mask 0x1) only. */
	step.color_idx = (uint8_t)dyn_idx;
	api->set_color_with_pattern(&pattern);
	api->asynchronous_apply_color(false);
	zassert_true(lightbar_buf[0].r > 1);
	zassert_true(lightbar_buf[0].g > 2);
	zassert_true(lightbar_buf[0].b > 3);
	zassert_equal(lightbar_buf[1].r, 1);
	zassert_equal(lightbar_buf[1].g, 2);
	zassert_equal(lightbar_buf[1].b, 3);
#else
	/* Without led-color golden nodes, IDW conversion fails. */
	zassert_equal(lb_lookup_pins_node_srgb(&cached_srgb, 0), -1);
#endif

	/* Unused dynamic color_idx returns early without modifying strip. */
	prev = lightbar_buf[0];
	step.color_idx = UINT8_MAX;
	api->set_color_with_pattern(&pattern);
	api->asynchronous_apply_color(false);
	zassert_equal(lightbar_buf[0].r, prev.r);
	zassert_equal(lightbar_buf[0].g, prev.g);
	zassert_equal(lightbar_buf[0].b, prev.b);

	/* Mismatched LED ID with static color_idx returns early safely. */
	step.color_idx = 0;
	pattern.led_id = EC_LED_ID_BATTERY_LED;
	api->set_color_with_pattern(&pattern);
	api->asynchronous_apply_color(false);
	zassert_equal(lightbar_buf[0].r, prev.r);
	zassert_equal(lightbar_buf[0].g, prev.g);
	zassert_equal(lightbar_buf[0].b, prev.b);
}

ZTEST(lightbar, test_linear_transition_caps)
{
	const struct led_driver_api *api =
		PINS_NODE(DT_INST(0, cros_ec_led_lightbar)).api;
	const struct pattern_color_node_t linear_steps[2] = {
		{ .color_idx = DT_NODE_CHILD_IDX(DT_NODELABEL(lightbar_white)),
		  .duration_ms = 300 },
		{ .color_idx =
			  DT_NODE_CHILD_IDX(DT_NODELABEL(lightbar_white_dim)),
		  .duration_ms = 300 },
	};
	struct led_pattern_node_t linear_pattern = {
		.led_id = EC_LED_ID_LIGHTBAR_LED,
		.transition = LED_TRANSITION_LINEAR,
		.cycle_limit = 1,
		.pattern_len = 2,
		.cur_color = 1,
		.elapsed_ms = 150,
		.pattern_color = linear_steps,
	};
	uint8_t first_r;

	/*
	 * Transition halfway from white (CAP_WHITE_AC) to white_dim
	 * (CAP_WHITE_DIM). Mixed caps resolve to CAP_WHITE_AC.
	 */
	api->set_color_with_pattern(&linear_pattern);
	api->asynchronous_apply_color(true);
	first_r = lightbar_buf[0].r;
#if DT_HAS_COMPAT_STATUS_OKAY(cros_ec_lightbar_golden_sets)
	/* Midpoint R=147.5 capped at 60% AC cap -> 88. */
	zassert_equal(first_r, 88);
#else
	/* Uncapped midpoint between 255 and 128 -> 191. */
	zassert_equal(first_r, 191);
#endif

	/* Subsequent tick progresses downward toward white_dim. */
	api->asynchronous_apply_color(true);
	zassert_true(lightbar_buf[0].r < first_r);
}

ZTEST(lightbar, test_enable_gpio_control)
{
#if DT_NODE_HAS_PROP(DT_INST(0, cros_ec_led_lightbar), enable_gpios)
	const struct led_rgb on_rgb = { 10, 20, 30 };
	const struct led_rgb off_rgb = { 0, 0, 0 };

	zassert_not_null(lb_data.enable_gpio.port);

	/*
	 * Enable GPIO_INPUT on gpio_emul alongside GPIO_OUTPUT so that
	 * gpio_pin_get_dt() inside the driver reads back the driven output.
	 */
	zassert_ok(gpio_pin_configure_dt(&lb_data.enable_gpio,
					 GPIO_OUTPUT_INACTIVE | GPIO_INPUT));

	/* Turning any LED on asserts enable GPIO. */
	lb_set_and_apply_rgb(on_rgb, SEGMENT_ID_ALL);
	zassert_equal(gpio_emul_output_get(lb_data.enable_gpio.port,
					   lb_data.enable_gpio.pin),
		      1);

	/* Turning all LEDs off deasserts enable GPIO. */
	lb_set_and_apply_rgb(off_rgb, SEGMENT_ID_ALL);
	zassert_equal(gpio_emul_output_get(lb_data.enable_gpio.port,
					   lb_data.enable_gpio.pin),
		      0);

	/*
	 * Simulate an external change to the enable GPIO while active and
	 * verify the next strip update recovers and re-asserts it.
	 */
	lb_set_and_apply_rgb(on_rgb, SEGMENT_ID_ALL);
	zassert_ok(gpio_emul_input_set(lb_data.enable_gpio.port,
				       lb_data.enable_gpio.pin, 0));
	zassert_equal(gpio_pin_get_dt(&lb_data.enable_gpio), 0);
	lb_set_and_apply_rgb((struct led_rgb){ 20, 30, 40 }, SEGMENT_ID_ALL);
	zassert_equal(gpio_pin_get_dt(&lb_data.enable_gpio), 1);
#else
	zassert_is_null(lb_data.enable_gpio.port);
#endif
}

ZTEST_SUITE(lightbar, NULL, NULL, NULL, lightbar_after, NULL);
