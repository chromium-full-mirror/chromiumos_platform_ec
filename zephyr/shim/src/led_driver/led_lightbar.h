/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_LED_LIGHTBAR_H
#define __CROS_EC_LED_LIGHTBAR_H

#include "common.h"
#include "drivers/led.h"
#include "ec_commands.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>

#define SEGMENT_ID_ALL -1

struct lb_data_t {
	const struct device *strip;
	uint32_t num_leds;
	struct gpio_dt_spec enable_gpio;
	uint8_t brightness_default_pct;
	uint8_t brightness_min_pct;
};
extern const struct lb_data_t lb_data;

extern struct led_rgb lightbar_buf[];

int lb_get_num_segments(void);
int lb_get_num_golden_sets(void);
int lb_get_active_golden_set(void);
const char *lb_get_golden_set_name(size_t idx);
int lb_select_golden_set(size_t idx);
const struct led_pins_node_t *lb_lookup_pins_node(enum led_color color);
int lb_lookup_pins_node_srgb(const struct led_rgb *srgb, int8_t segment_id);
const struct led_rgb *lb_srgb_to_hw_rgb(const struct led_rgb *srgb,
					int8_t segment_id);
void lb_set_and_apply_rgb(struct led_rgb new_color, int8_t segment_id);
void lb_get_rgb(struct led_rgb *new_color);
uint8_t lb_get_brightness_pct(void);
void lb_set_brightness_pct(uint8_t brightness);
void lightbar_update_strip(void);
void lb_set_diag_policy(int policy, uint32_t duration_ms);
void lb_control_value_process(uint8_t control);
void lb_set_takeover(bool enable);
bool lb_is_takeover_active(void);
bool lb_is_ec_busy(void);
void lb_cancel_one_shot(struct custom_led_patterns_t *pattern);
uint32_t lb_get_version_flags(void);
bool lb_is_custom_pattern(const struct led_pattern_node_t *pattern);
__override_proto enum ec_status board_lightbar_custom_seq(uint8_t seq);

#endif /* __CROS_EC_LED_LIGHTBAR_H */
