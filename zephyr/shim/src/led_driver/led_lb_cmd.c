/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Lightbar LED CLI.
 */

#include "drivers/led.h"
#include "led_common.h"
#include "led_lightbar.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(lightbar_cmd, LOG_LEVEL_ERR);

static int cmd_led_count(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "LED count = %d", lb_data.num_leds);
	return 0;
}

static void parse_rgb(char **argv, struct led_rgb *rgb)
{
	rgb->r = (uint8_t)strtol(argv[0], NULL, 0);
	rgb->g = (uint8_t)strtol(argv[1], NULL, 0);
	rgb->b = (uint8_t)strtol(argv[2], NULL, 0);
}

static int cmd_color_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 4) {
		shell_error(sh, "Usage: set <r> <g> <b>");
		return -EINVAL;
	}

	struct led_rgb color;

	parse_rgb(&argv[1], &color);

	/* disable policy */
	led_auto_control(EC_LED_ID_LIGHTBAR_LED, 0);
	lb_set_and_apply_rgb(color, SEGMENT_ID_ALL);

	return 0;
}

static int cmd_color_set_srgb(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 4) {
		shell_error(sh, "Usage: set_srgb <r> <g> <b>");
		return -EINVAL;
	}

	struct led_rgb srgb;

	parse_rgb(&argv[1], &srgb);

	const struct led_rgb *hw = lb_srgb_to_hw_rgb(&srgb, SEGMENT_ID_ALL);

	if (!hw) {
		shell_error(sh, "sRGB mapping not found");
		return -EINVAL;
	}

	/* disable policy */
	led_auto_control(EC_LED_ID_LIGHTBAR_LED, 0);
	lb_set_and_apply_rgb(*hw, SEGMENT_ID_ALL);

	shell_print(sh, "Mapped to HW(%d,%d,%d)", hw->r, hw->g, hw->b);

	return 0;
}

static int cmd_color_set_single(const struct shell *sh, size_t argc,
				char **argv)
{
	if (argc != 5) {
		shell_error(
			sh,
			"Usage: set_single <led> <r> <g> <b> (values 0-255)");
		return -EINVAL;
	}

	size_t led_idx = (size_t)strtol(argv[1], NULL, 0);
	if (led_idx >= lb_data.num_leds) {
		shell_error(sh, "LED index (%d) out of range (%d)", led_idx,
			    lb_data.num_leds);
		return -EINVAL;
	}

	struct led_rgb color;

	parse_rgb(&argv[2], &color);

	/* disable policy */
	led_auto_control(EC_LED_ID_LIGHTBAR_LED, 0);

	lightbar_buf[led_idx] = color;
	lightbar_update_strip();

	return 0;
}

static int cmd_color_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < lb_data.num_leds; i++) {
		struct led_rgb color;
		color = lightbar_buf[i];
		shell_print(sh, "RGB[%d]: %d %d %d", i, color.r, color.g,
			    color.b);
	}

	return 0;
}

static int cmd_led_auto(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		const char *state;

		if (!led_auto_control_is_enabled(EC_LED_ID_LIGHTBAR_LED)) {
			state = "disabled";
		} else if (lb_is_takeover_active()) {
			state = "host";
		} else {
			state = "enabled";
		}
		shell_print(sh, "Current auto control state: %s", state);
		return 0;
	}

	if (argc != 2) {
		shell_error(sh, "Usage: auto <on|off|host>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		lb_set_takeover(false);
		led_auto_control(EC_LED_ID_LIGHTBAR_LED, 1);
		shell_print(sh, "Lightbar auto control enabled.");
	} else if (strcmp(argv[1], "off") == 0) {
		led_auto_control(EC_LED_ID_LIGHTBAR_LED, 0);
		lb_set_takeover(false);
		shell_print(sh, "Lightbar auto control disabled.");
	} else if (strcmp(argv[1], "host") == 0) {
		lb_set_takeover(true);
		led_auto_control(EC_LED_ID_LIGHTBAR_LED, 1);
		shell_print(sh, "Lightbar host control enabled.");
	} else {
		shell_error(sh, "Usage: auto <on|off|host>");
		return -EINVAL;
	}

	return 0;
}

static int cmd_led_brightness(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t brightness;

	if (argc == 1) {
		shell_print(sh, "Current brightness = %d",
			    lb_get_brightness_pct());
		return 0;
	}

	if (argc != 2) {
		shell_error(sh, "Usage: br <level>");
		return -EINVAL;
	}

	brightness = (uint8_t)strtol(argv[1], NULL, 0);
	if (brightness > 100) {
		shell_error(sh, "Brightness level must be between 0 and 100.");
		return -EINVAL;
	}

	lb_set_brightness_pct(brightness);
	return 0;
}

static int cmd_golden_set(const struct shell *sh, size_t argc, char **argv)
{
	int num_sets = lb_get_num_golden_sets();

	if (num_sets <= 0) {
		shell_print(sh, "No golden color sets defined.");
		return 0;
	}

	if (argc == 1) {
		int active = lb_get_active_golden_set();

		for (int i = 0; i < num_sets; i++) {
			shell_print(sh, "%c [%d] %s", (i == active) ? '*' : ' ',
				    i, lb_get_golden_set_name(i));
		}
		return 0;
	}

	if (argc != 2) {
		shell_error(sh, "Usage: golden [set_idx]");
		return -EINVAL;
	}

	char *endptr;
	long idx = strtol(argv[1], &endptr, 0);

	if (*endptr != '\0' || idx < 0 || idx >= num_sets) {
		shell_error(sh, "Golden set index out of range (0-%d)",
			    num_sets - 1);
		return -EINVAL;
	}

	int rv = lb_select_golden_set((size_t)idx);

	if (rv != EC_SUCCESS) {
		shell_error(sh, "Failed to select golden set %ld (err %d)", idx,
			    rv);
		return -EINVAL;
	}

	reset_policy_patterns(EC_LED_ID_LIGHTBAR_LED);
	shell_print(sh, "Selected golden set [%ld] %s", idx,
		    lb_get_golden_set_name((size_t)idx));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	lb_control,
	SHELL_CMD(count, NULL, "Show the number of LEDs in the lightbar strip",
		  cmd_led_count),
	SHELL_CMD_ARG(set, NULL, "Set a raw hw color: set <r> <g> <b>",
		      cmd_color_set, 3, 3),
	SHELL_CMD_ARG(set_srgb, NULL,
		      "Set a calibrated sRGB color: set_srgb <r> <g> <b>",
		      cmd_color_set_srgb, 3, 3),
	SHELL_CMD(get, NULL, "Get the last written RAW RGB array",
		  cmd_color_get),
	SHELL_CMD_ARG(set_single, NULL,
		      "Set a single led: set_single <n> <r> <g> <b> ",
		      cmd_color_set_single, 4, 4),
	SHELL_CMD_ARG(auto, NULL, "Set auto control: auto <on|off|host>",
		      cmd_led_auto, 0, 1),
	SHELL_CMD_ARG(br, NULL, "Set brightness level: br <level>",
		      cmd_led_brightness, 0, 1),
	SHELL_CMD_ARG(golden, NULL,
		      "Get or set golden color set: golden [set_idx]",
		      cmd_golden_set, 0, 1),

	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lb, &lb_control, "Lightbar control command", NULL);
