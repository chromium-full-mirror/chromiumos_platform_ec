/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Lightbar LED control.
 */

#define DT_DRV_COMPAT cros_ec_led_lightbar
#define GOLDEN_SETS_COMPAT cros_ec_lightbar_golden_sets
#define HAS_GOLDEN_SETS DT_HAS_COMPAT_STATUS_OKAY(GOLDEN_SETS_COMPAT)

#include "drivers/led.h"
#include "ec_commands.h"
#include "extpower.h"
#include "hooks.h"
#include "led_common.h"
#include "led_lb_host_constant.h"
#include "led_lb_util.h"
#include "led_lightbar.h"
#include "task.h"
#include "util.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/led_colors_util.h>

LOG_MODULE_REGISTER(lightbar_led, LOG_LEVEL_INF);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Exactly one instance of cros-ec-led-lightbar should be defined.");
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(GOLDEN_SETS_COMPAT) <= 1,
	     "At most one cros-ec,lightbar-golden-sets node may be enabled.");

#define LIGHTBAR_NODE DT_INST(0, DT_DRV_COMPAT)

BUILD_ASSERT(DT_CHILD_NUM_STATUS_OKAY(LIGHTBAR_NODE) == 1,
	     "Lightbar node should have a single child");

#define GET_NODE_ID(node_id) node_id
#define LIGHTBAR0_NODE DT_FOREACH_CHILD_SEP(LIGHTBAR_NODE, GET_NODE_ID, ())
#define STRIP_NODE DT_PHANDLE(LIGHTBAR0_NODE, strip)
#define STRIP_LENGTH DT_PROP(STRIP_NODE, chain_length)
#define RAMPUP_DELAY_MS DT_PROP_OR(LIGHTBAR_NODE, ramp_up_delay_ms, 0)

BUILD_ASSERT(STRIP_LENGTH <= 16, "Driver only supports up to 16 LEDs");

static const uint16_t lb_segments[] =
	DT_PROP_OR(LIGHTBAR_NODE, led_segments, {});

BUILD_ASSERT(ARRAY_SIZE(lb_segments) <= LIGHTBAR_MAX_CUSTOM_EFFECT,
	     "Too many lightbar segments");

/*
 * Use 9.6 fixed-point math (1 bit sign, 9 bits integer, 6 bits fractional).
 * This allows the full 0-255 color range and leaves a headroom to prevent
 * overflow.
 */
#define FIXED_POINT_SHIFT 6
#define MAX_BRIGHTNESS 100

#define CAP_NONE 0
#define CAP_WHITE_AC 1
#define CAP_WHITE_DIM 2

/*
 * Brightness Cap (L_cap) Calculation (http://go/lb_br_cap):
 *   R_max = Max_Power_Constrained_RGB / White_RGB
 *   R_actual = Dim_White_RGB / White_RGB
 *   L_cap = MIN(100 * R_max / R_actual, 100)
 *         = MIN(100 * Max_Power_Constrained_RGB / Dim_White_RGB, 100)
 *
 * We use the sum of RGB components to calculate ratios without precision loss.
 */
#define MAX_POWER_SUM                                                      \
	SUM_RGB(DT_PROP_OR(LIGHTBAR_NODE, max_power_constrained_white_rgb, \
			   0xFFFFFF))

#define SET_MAX_POWER_SUM(set_node)                                            \
	COND_CODE_1(                                                           \
		DT_NODE_HAS_PROP(set_node, max_power_constrained_white_rgb),   \
		(SUM_RGB(DT_PROP(set_node, max_power_constrained_white_rgb))), \
		(MAX_POWER_SUM))

#define SET_WHITE_BATT_SUM(set_node) \
	SUM_RGB(DT_PROP(DT_CHILD(set_node, white_dim), color))
#define SET_WHITE_AC_SUM(set_node) \
	SUM_RGB(DT_PROP(DT_CHILD(set_node, white), color))

/* Calculate caps based on ratios. Battery Cap: 100mW */
#define SET_BATT_BRIGHTNESS_CAP_PCT(set_node)                                 \
	MIN(100 * SET_MAX_POWER_SUM(set_node) / SET_WHITE_BATT_SUM(set_node), \
	    100)

/* AC Cap: 250mW (2.5x 100mW) */
#define SET_AC_BRIGHTNESS_CAP_PCT(set_node) \
	MIN(250 * SET_MAX_POWER_SUM(set_node) / SET_WHITE_AC_SUM(set_node), 100)

#define GET_NODE_CAP_TYPE(node_id) \
	DT_STRING_TOKEN_OR(node_id, brightness_cap, CAP_NONE)

#define COLOR_NODE(id) DT_CAT(color_data_, id)
#define COLOR_SET_NODE(set_id) DT_CAT(lb_color_set_, set_id)

/* Configuration for a specific color node */
struct lb_node_conf {
	uint8_t base_color_idx;
	struct led_rgb srgb;
	int8_t segment_id;
	uint8_t cap_type;
};

#define VALIDATE_SEGMENT_ID(node_id)                                       \
	COND_CODE_1(                                                       \
		DT_NODE_HAS_PROP(LIGHTBAR_NODE, led_segments),             \
		(BUILD_ASSERT(                                             \
			(DT_PROP_OR(node_id, segment_id, SEGMENT_ID_ALL) < \
			 DT_PROP_LEN(LIGHTBAR_NODE, led_segments)),        \
			"segment-id in " #node_id " is out of bounds")),   \
		(BUILD_ASSERT(                                             \
			DT_PROP_OR(node_id, segment_id, SEGMENT_ID_ALL) == \
				SEGMENT_ID_ALL,                            \
			"segment-id in " #node_id                          \
			" must be -1/omitted when led-segments is missing")));

#define HAS_LAST_SEGMENT_ID(node_id)                           \
	|| (DT_PROP_OR(node_id, segment_id, SEGMENT_ID_ALL) == \
	    ((int)ARRAY_SIZE(lb_segments) - 1))

#define COUNT_BASE_COLOR(node_id) \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, segment_id), (), (+1))

#define BASE_COLOR_COUNT (0 DT_FOREACH_CHILD(LIGHTBAR0_NODE, COUNT_BASE_COLOR))

BUILD_ASSERT(BASE_COLOR_COUNT <= UINT8_MAX,
	     "BASE_COLOR_COUNT must fit in uint8_t");

#define COUNT_PRECEDING_BASE_COLOR(child_id, target_id)                     \
	+((!DT_NODE_HAS_PROP(child_id, segment_id) &&                       \
	   (DT_NODE_CHILD_IDX(child_id) <= DT_NODE_CHILD_IDX(target_id))) ? \
		  1 :                                                       \
		  0)

#define GET_BASE_COLOR_IDX(node_id)                                            \
	((0 DT_FOREACH_CHILD_VARGS(LIGHTBAR0_NODE, COUNT_PRECEDING_BASE_COLOR, \
				   node_id)) -                                 \
	 1)

#define VALIDATE_BASE_COLOR_IDX(node_id)               \
	BUILD_ASSERT(GET_BASE_COLOR_IDX(node_id) >= 0, \
		     "Segment node " #node_id          \
		     " must be preceded by a global color node");

BUILD_ASSERT(SET_WHITE_BATT_SUM(LIGHTBAR0_NODE) > 0,
	     "Capped color sum must be > 0 in lightbar0");
BUILD_ASSERT(SET_WHITE_AC_SUM(LIGHTBAR0_NODE) > 0,
	     "Capped color sum must be > 0 in lightbar0");
DT_FOREACH_CHILD(LIGHTBAR0_NODE, VALIDATE_SEGMENT_ID)
DT_FOREACH_CHILD(LIGHTBAR0_NODE, VALIDATE_BASE_COLOR_IDX)
BUILD_ASSERT(ARRAY_SIZE(lb_segments) == 0 ||
		     (0 DT_FOREACH_CHILD(LIGHTBAR0_NODE, HAS_LAST_SEGMENT_ID)),
	     "Max segment-id in lightbar0 must match led-segments length");

struct lb_color_set {
	struct led_rgb colors[BASE_COLOR_COUNT];
	uint8_t batt_brightness_cap_pct;
	uint8_t ac_brightness_cap_pct;
};

#define GEN_SET_COLOR(node_id)                                            \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, segment_id), (),            \
		    ({                                                    \
			     .r = (DT_PROP(node_id, color) >> 16) & 0xFF, \
			     .g = (DT_PROP(node_id, color) >> 8) & 0xFF,  \
			     .b = DT_PROP(node_id, color) & 0xFF,         \
		     }, ))

#define GEN_COLOR_SET(set_id)                                               \
	static const struct lb_color_set COLOR_SET_NODE(set_id) = {         \
		.colors = { DT_FOREACH_CHILD(set_id, GEN_SET_COLOR) },      \
		.batt_brightness_cap_pct =                                  \
			SET_BATT_BRIGHTNESS_CAP_PCT(set_id),                \
		.ac_brightness_cap_pct = SET_AC_BRIGHTNESS_CAP_PCT(set_id), \
	};

GEN_COLOR_SET(LIGHTBAR0_NODE)

#if HAS_GOLDEN_SETS
#define GOLDEN_SETS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(GOLDEN_SETS_COMPAT)

#define VALIDATE_NO_SEGMENT_ID(node_id)                      \
	BUILD_ASSERT(!DT_NODE_HAS_PROP(node_id, segment_id), \
		     "Golden set node " #node_id             \
		     " must not specify segment-id");

#define VALIDATE_GOLDEN_SET(set_node)                               \
	BUILD_ASSERT(SET_WHITE_BATT_SUM(set_node) > 0,              \
		     "Capped color sum must be > 0 in " #set_node); \
	BUILD_ASSERT(SET_WHITE_AC_SUM(set_node) > 0,                \
		     "Capped color sum must be > 0 in " #set_node); \
	DT_FOREACH_CHILD(set_node, VALIDATE_NO_SEGMENT_ID)          \
	BUILD_ASSERT(DT_CHILD_NUM(set_node) == BASE_COLOR_COUNT,    \
		     "Color set " #set_node " must match base color count");

DT_FOREACH_CHILD_STATUS_OKAY(GOLDEN_SETS_NODE, VALIDATE_GOLDEN_SET)
DT_FOREACH_CHILD_STATUS_OKAY(GOLDEN_SETS_NODE, GEN_COLOR_SET)

#define COLOR_SET_PTR(set_id) &COLOR_SET_NODE(set_id),
#define COLOR_SET_NAME(set_id) DT_NODE_FULL_NAME(set_id),

static const struct lb_color_set *const lb_color_sets[] = {
	&COLOR_SET_NODE(LIGHTBAR0_NODE),
	DT_FOREACH_CHILD_STATUS_OKAY(GOLDEN_SETS_NODE, COLOR_SET_PTR)
};

static const char *const lb_color_set_names[] = {
	DT_NODE_FULL_NAME(LIGHTBAR0_NODE),
	DT_FOREACH_CHILD_STATUS_OKAY(GOLDEN_SETS_NODE, COLOR_SET_NAME)
};
#endif /* HAS_GOLDEN_SETS */

static const struct lb_color_set *active_color_set =
	&COLOR_SET_NODE(LIGHTBAR0_NODE);

/* Create configuration structs for each color child node. */
#define GEN_COLORS(node_id)                                                    \
	{                                                                      \
		.base_color_idx = GET_BASE_COLOR_IDX(node_id),                 \
		.srgb.r = (DT_PROP_OR(node_id, srgb, 0) >> 16) & 0xFF,         \
		.srgb.g = (DT_PROP_OR(node_id, srgb, 0) >> 8) & 0xFF,          \
		.srgb.b = DT_PROP_OR(node_id, srgb, 0) & 0xFF,                 \
		.segment_id = DT_PROP_OR(node_id, segment_id, SEGMENT_ID_ALL), \
		.cap_type = GET_NODE_CAP_TYPE(node_id),                        \
	};

#define GEN_COLOR_NODES(id) \
	static const struct lb_node_conf COLOR_NODE(id) = GEN_COLORS(id);

DT_FOREACH_CHILD(LIGHTBAR0_NODE, GEN_COLOR_NODES)

#define COLOR_POINTER(id) (void *)&COLOR_NODE(id)

static void lb_set_color_with_pattern(void *p);
static void lb_asynchronous_apply_color(bool has_transitions);
static void lb_set_color(enum led_color color, enum ec_led_id led_id,
			 uint8_t brightness);
static void lb_get_brightness_range(enum ec_led_id led_id,
				    uint8_t *brightness_range);
static int lb_set_brightness(enum ec_led_id led_id, const uint8_t *brightness);

static const struct led_driver_api lb_led_driver_api = {
	.asynchronous_apply_color = lb_asynchronous_apply_color,
	.set_color_with_pattern = lb_set_color_with_pattern,
	.set_color = lb_set_color,
	.get_brightness_range = lb_get_brightness_range,
	.set_brightness = lb_set_brightness,
};

/* Generate one handle for the driver instance */
const struct led_driver_t PINS_NODE(DT_DRV_INST(0)) = {
	.led_id_mask = GET_DRIVER_ID_MASK(0),
	.api = &lb_led_driver_api,
};

/*
 * Create a led_pins_node_t struct for each color child node
 * of the lightbar devicetree node.
 */
#define SET_PIN_NODE(node_id)                                          \
	{                                                              \
		.led_color = GET_PROP(node_id, led_color),             \
		.led_id = DT_STRING_TOKEN(DT_PARENT(node_id), led_id), \
		.color_idx = DT_NODE_CHILD_IDX(node_id),               \
		.pins = COLOR_POINTER(node_id),                        \
		.pins_count = 0,                                       \
	}

#define GEN_PINS_NODES(id) \
	const struct led_pins_node_t PINS_NODE(id) = SET_PIN_NODE(id);

DT_FOREACH_CHILD(LIGHTBAR0_NODE, GEN_PINS_NODES)

/*
 * Create an array of pointers to the led_pins_node_t structs. This
 * is used by the common LED code to find color definitions.
 */
#define PINS_NODE_PTR(node_id) &PINS_NODE(node_id),

static const struct led_pins_node_t *const lb_pins_node[] = { DT_FOREACH_CHILD(
	LIGHTBAR0_NODE, PINS_NODE_PTR) };

static const int pins_node_count = ARRAY_SIZE(lb_pins_node);

BUILD_ASSERT(ARRAY_SIZE(lb_pins_node) > 0,
	     "Lightbar must have at least one color node");

struct led_rgb lightbar_buf[STRIP_LENGTH];

/* Data for the lightbar instance */
const struct lb_data_t lb_data = {
	.strip = DEVICE_DT_GET(STRIP_NODE),
	.num_leds = STRIP_LENGTH,
	.enable_gpio = GPIO_DT_SPEC_GET_OR(LIGHTBAR_NODE, enable_gpios, { 0 }),
	.brightness_default_pct = DT_PROP_OR(
		LIGHTBAR_NODE, default_brightness_percent, MAX_BRIGHTNESS),
	.brightness_min_pct =
		DT_PROP_OR(LIGHTBAR_NODE, min_brightness_percent, 0),
};

static uint16_t lb_get_segment_mask(int8_t segment_id)
{
	if (segment_id == SEGMENT_ID_ALL) {
		return UINT16_MAX; /* All 1s for All LEDs */
	}
	/* Ignore invalid IDs */
	if (segment_id < 0 || segment_id >= ARRAY_SIZE(lb_segments)) {
		return 0;
	}
	return lb_segments[segment_id];
}

int lb_get_num_segments(void)
{
	return ARRAY_SIZE(lb_segments);
}

/*
 * State for individual LED's color and transitions.
 * The color components are stored as 23.8 fixed-point values to allow
 * for fractional increments during transitions.
 */
struct lb_led_state {
	struct {
		int16_t r, g, b; /* 9.6 signed accumulator */
	} color;
	int16_t r_step, g_step, b_step; /* 9.6 signed increment */
	uint8_t cap_type;
};

/* State for the entire lightbar's brightness. */
struct lightbar_state {
	uint8_t brightness;
	struct lb_led_state leds[STRIP_LENGTH];
};

static struct lightbar_state lb_state = {
	.brightness = lb_data.brightness_default_pct
};

#define DYNAMIC_COLOR_IDX_BASE ARRAY_SIZE(lb_pins_node)
#define IS_DYNAMIC_COLOR_IDX(idx) ((idx) >= DYNAMIC_COLOR_IDX_BASE)

/*
 * Dynamic conversion cache size: worst-case scenario is every custom segment
 * needs conversion, capped so that the maximum color_idx does not exceed 255.
 */
#define LB_CONVERSION_CACHE_SIZE             \
	MIN(LIGHTBAR_MAX_CUSTOM_SEGMENT * 2, \
	    (UINT8_MAX - DYNAMIC_COLOR_IDX_BASE + 1))

BUILD_ASSERT((DYNAMIC_COLOR_IDX_BASE + LB_CONVERSION_CACHE_SIZE - 1) <=
		     UINT8_MAX,
	     "Highest dynamic color_idx must fit within uint8_t");

struct lb_cache_entry {
	struct led_pins_node_t node;
	struct lb_node_conf conf;
	struct led_rgb color;
};

static struct lb_cache_entry lb_conversion_cache[LB_CONVERSION_CACHE_SIZE];
static int cache_head;

static struct led_rgb lb_get_node_hw_color(const struct led_pins_node_t *node)
{
	const struct lb_node_conf *conf = node->pins;

	if (!IS_DYNAMIC_COLOR_IDX(node->color_idx)) {
		return active_color_set->colors[conf->base_color_idx];
	}
	return lb_conversion_cache[node->color_idx - DYNAMIC_COLOR_IDX_BASE]
		.color;
}

static void lb_apply_current_color(void)
{
	bool changed = false;
	uint8_t global_brightness = lb_state.brightness;
	uint8_t capped_ac = 0;
	uint8_t capped_dim = 0;
	bool cap_calculated = false;

	for (int i = 0; i < lb_data.num_leds; i++) {
		const struct lb_led_state *led_state = &lb_state.leds[i];
		struct led_rgb current_color;
		int32_t val;
		uint8_t brightness = global_brightness;

		if (led_state->cap_type != CAP_NONE) {
			if (!cap_calculated) {
				capped_ac = min(
					global_brightness,
					active_color_set->ac_brightness_cap_pct);
				capped_dim =
					min(global_brightness,
					    active_color_set
						    ->batt_brightness_cap_pct);
				cap_calculated = true;
			}
			brightness = (led_state->cap_type == CAP_WHITE_AC) ?
					     capped_ac :
					     capped_dim;
		}

		/* Downsample and apply brightness */
		val = (((int32_t)led_state->color.r * brightness) /
		       MAX_BRIGHTNESS) >>
		      FIXED_POINT_SHIFT;
		current_color.r = clamp(val, 0, 255);
		val = (((int32_t)led_state->color.g * brightness) /
		       MAX_BRIGHTNESS) >>
		      FIXED_POINT_SHIFT;
		current_color.g = clamp(val, 0, 255);
		val = (((int32_t)led_state->color.b * brightness) /
		       MAX_BRIGHTNESS) >>
		      FIXED_POINT_SHIFT;
		current_color.b = clamp(val, 0, 255);

		if (lightbar_buf[i].r != current_color.r ||
		    lightbar_buf[i].g != current_color.g ||
		    lightbar_buf[i].b != current_color.b) {
			lightbar_buf[i] = current_color;
			changed = true;
		}
	}

	/* Apply the color only if it changed */
	if (changed) {
		lightbar_update_strip();
	}
}

int lb_get_num_golden_sets(void)
{
#if HAS_GOLDEN_SETS
	return ARRAY_SIZE(lb_color_sets);
#else
	return 0;
#endif
}

int lb_get_active_golden_set(void)
{
#if HAS_GOLDEN_SETS
	for (size_t i = 0; i < ARRAY_SIZE(lb_color_sets); i++) {
		if (active_color_set == lb_color_sets[i]) {
			return i;
		}
	}
#endif
	return -1;
}

const char *lb_get_golden_set_name(size_t idx)
{
#if HAS_GOLDEN_SETS
	if (idx < ARRAY_SIZE(lb_color_set_names)) {
		return lb_color_set_names[idx];
	}
#else
	ARG_UNUSED(idx);
#endif
	return NULL;
}

int lb_select_golden_set(size_t idx)
{
#if HAS_GOLDEN_SETS
	if (idx >= ARRAY_SIZE(lb_color_sets)) {
		return EC_ERROR_INVAL;
	}
	active_color_set = lb_color_sets[idx];
	memset(lb_conversion_cache, 0, sizeof(lb_conversion_cache));
	cache_head = 0;
	lb_apply_current_color();
	return EC_SUCCESS;
#else
	ARG_UNUSED(idx);
	return EC_ERROR_UNIMPLEMENTED;
#endif
}

static void lb_set_target_rgb(struct led_rgb rgb, int8_t segment_id)
{
	uint32_t mask = lb_get_segment_mask(segment_id);

	for (int i = 0; i < lb_data.num_leds; i++) {
		if (!(mask & BIT(i)))
			continue;

		struct lb_led_state *led_state = &lb_state.leds[i];

		led_state->color.r = ((int16_t)rgb.r) << FIXED_POINT_SHIFT;
		led_state->color.g = ((int16_t)rgb.g) << FIXED_POINT_SHIFT;
		led_state->color.b = ((int16_t)rgb.b) << FIXED_POINT_SHIFT;
		led_state->r_step = 0;
		led_state->g_step = 0;
		led_state->b_step = 0;
		led_state->cap_type = CAP_NONE;
	}
}

static int lb_srgb_to_hw_color(const struct led_rgb srgb, struct led_rgb *color)
{
	/*
	 * Fixed buffer based on the max number of supported LED colors
	 *
	 * TODO(b:462434336): Generate the calibration points table as a static
	 * const array at build time via Devicetree.
	 */
	struct lb_calibration_point points[EC_LED_COLOR_COUNT];
	size_t active_count = 0;

	for (int i = 0; i < pins_node_count; i++) {
		const struct led_pins_node_t *node = lb_pins_node[i];

		/* Skip non-Golden colors */
		if (node->led_color == LED_OFF)
			continue;

		/* Safety check to prevent overflow */
		if (active_count >= EC_LED_COLOR_COUNT) {
			LOG_ERR("Too many golden colors defined");
			break;
		}

		const struct lb_node_conf *conf =
			(const struct lb_node_conf *)node->pins;

		points[active_count].srgb = conf->srgb;
		points[active_count].color = lb_get_node_hw_color(node);
		active_count++;
	}

	/* Calculate the mapped color. */
	int rv = lb_calculate_idw(srgb, color, points, active_count);

	if (rv != EC_SUCCESS) {
		LOG_ERR("IDW failed for sRGB(%d,%d,%d)", srgb.r, srgb.g,
			srgb.b);
	}

	return rv;
}

int lb_lookup_pins_node_srgb(const struct led_rgb *srgb, int8_t segment_id)
{
	struct lb_cache_entry *target;

	if (segment_id != SEGMENT_ID_ALL &&
	    (segment_id < 0 || segment_id >= lb_get_num_segments())) {
		return -1;
	}

	/* Lookup in built-in Golden colors */
	for (int i = 0; i < pins_node_count; i++) {
		const struct led_pins_node_t *node = lb_pins_node[i];
		const struct lb_node_conf *conf =
			(const struct lb_node_conf *)node->pins;
		if (conf->srgb.r == srgb->r && conf->srgb.g == srgb->g &&
		    conf->srgb.b == srgb->b && conf->segment_id == segment_id)
			return node->color_idx;
	}

	/* Lookup in conversion cache */
	for (int i = 0; i < LB_CONVERSION_CACHE_SIZE; i++) {
		if (lb_conversion_cache[i].node.pins != NULL) {
			if (lb_conversion_cache[i].conf.srgb.r == srgb->r &&
			    lb_conversion_cache[i].conf.srgb.g == srgb->g &&
			    lb_conversion_cache[i].conf.srgb.b == srgb->b &&
			    lb_conversion_cache[i].conf.segment_id ==
				    segment_id) {
				return lb_conversion_cache[i].node.color_idx;
			}
		}
	}

	/* Cache miss: insert at current head */
	target = &lb_conversion_cache[cache_head];

	memset(target, 0, sizeof(struct lb_cache_entry));

	if (lb_srgb_to_hw_color(*srgb, &target->color) != EC_SUCCESS) {
		return -1;
	}
	LOG_INF("sRGB(%d,%d,%d) seg %d -> HW(%d,%d,%d)", srgb->r, srgb->g,
		srgb->b, segment_id, target->color.r, target->color.g,
		target->color.b);
	target->conf.srgb = *srgb;
	target->conf.segment_id = segment_id;
	target->node.pins = &target->conf;
	target->node.led_id = lb_pins_node[0]->led_id;
	target->node.color_idx = DYNAMIC_COLOR_IDX_BASE + cache_head;

	int color_idx = target->node.color_idx;

	/* Advance head */
	cache_head = (cache_head + 1) % LB_CONVERSION_CACHE_SIZE;

	return color_idx;
}

const struct led_rgb *lb_srgb_to_hw_rgb(const struct led_rgb *srgb,
					int8_t segment_id)
{
	for (int i = 0; i < pins_node_count; i++) {
		const struct led_pins_node_t *node = lb_pins_node[i];
		const struct lb_node_conf *conf =
			(const struct lb_node_conf *)node->pins;
		if (conf->srgb.r == srgb->r && conf->srgb.g == srgb->g &&
		    conf->srgb.b == srgb->b && conf->segment_id == segment_id) {
			return &active_color_set->colors[conf->base_color_idx];
		}
	}
	return NULL;
}

const struct led_pins_node_t *lb_lookup_pins_node(enum led_color color)
{
	for (int i = 0; i < pins_node_count; i++) {
		const struct led_pins_node_t *node = lb_pins_node[i];
		if (node->led_color == color) {
			return node;
		}
	}
	return NULL;
}

static void lb_set_target_enum(enum led_color color)
{
	struct led_rgb rgb = { 0, 0, 0 };

	if (color != LED_OFF) {
		const struct led_pins_node_t *node = lb_lookup_pins_node(color);
		if (node) {
			rgb = lb_get_node_hw_color(node);
		}
	}
	lb_set_target_rgb(rgb, SEGMENT_ID_ALL);
}

/*
 * The pins_node array flattens all color nodes across all LEDs. Since color_idx
 * (DT_NODE_CHILD_IDX) is only unique within a specific LED parent, should match
 * both led_id and color_idx to find the correct hardware pins. This run-time
 * lookup is for optimizing flash usage instead of storing 32-bit pointers.
 */
static const struct led_pins_node_t *lb_find_pins_node(enum ec_led_id led_id,
						       uint8_t color_idx)
{
	if (!IS_DYNAMIC_COLOR_IDX(color_idx)) {
		for (int i = 0; i < pins_node_count; i++) {
			if (lb_pins_node[i]->led_id == led_id &&
			    lb_pins_node[i]->color_idx == color_idx) {
				return lb_pins_node[i];
			}
		}
		return NULL;
	}

	size_t slot = color_idx - DYNAMIC_COLOR_IDX_BASE;

	if (slot < LB_CONVERSION_CACHE_SIZE &&
	    lb_conversion_cache[slot].node.pins != NULL) {
		return &lb_conversion_cache[slot].node;
	}

	return NULL;
}

static void lb_set_color_with_pattern(void *p)
{
	struct led_pattern_node_t *pattern = (struct led_pattern_node_t *)p;

	/*
	 * When userspace takeover is active and no EC one-shot animation is
	 * busy, suppress built-in Devicetree policy patterns so 1-iteration
	 * host animations hold their final frame.
	 */
	if (lb_is_takeover_active() && !lb_is_ec_busy() &&
	    !lb_is_custom_pattern(pattern)) {
		for (int i = 0; i < lb_data.num_leds; i++) {
			lb_state.leds[i].r_step = 0;
			lb_state.leds[i].g_step = 0;
			lb_state.leds[i].b_step = 0;
		}
		return;
	}

	uint8_t next_idx = pattern->pattern_color[pattern->cur_color].color_idx;
	const struct led_pins_node_t *next_color_node =
		lb_find_pins_node(pattern->led_id, next_idx);

	uint8_t prev_color_idx =
		(pattern->cur_color + pattern->pattern_len - 1) %
		pattern->pattern_len;
	uint8_t prev_idx = pattern->pattern_color[prev_color_idx].color_idx;
	const struct led_pins_node_t *prev_color_node =
		lb_find_pins_node(pattern->led_id, prev_idx);

	if (!next_color_node || !prev_color_node) {
		return;
	}

	const struct lb_node_conf *next_conf =
		(struct lb_node_conf *)next_color_node->pins;
	int32_t duration_ms =
		pattern->pattern_color[pattern->cur_color].duration_ms;
	const struct lb_node_conf *prev_conf =
		(struct lb_node_conf *)prev_color_node->pins;

	struct led_rgb prev_color = lb_get_node_hw_color(prev_color_node);
	struct led_rgb next_color = lb_get_node_hw_color(next_color_node);

	enum led_transition transition_type = pattern->transition;
	if (duration_ms == 0) {
		transition_type = LED_TRANSITION_STEP;
	}

	/*
	 * To prevent bright flashes during fades, the cap is enforced for the
	 * entire duration of the transition if either side requires it. If a
	 * transition mixes both capped colors, we resolve the conflict by
	 * defaulting to the AC cap to keep the power draw safely within budget.
	 */
	uint8_t prev_cap = prev_conf->cap_type;
	uint8_t next_cap = next_conf->cap_type;

	uint8_t use_cap = prev_cap ? prev_cap : next_cap;
	if (prev_cap && next_cap && (prev_cap != next_cap)) {
		use_cap = CAP_WHITE_AC;
	}

	struct lb_led_state new_state;
	new_state.cap_type = use_cap;

	switch (transition_type) {
	case LED_TRANSITION_LINEAR:
		/*
		 * For linear transitions, calculate the initial color based on
		 * the elapsed time, and determine the color step for each tick
		 * of the transition.
		 */
		int32_t elapsed_ms = pattern->elapsed_ms;

		new_state.color.r =
			((int32_t)(next_color.r - prev_color.r)
			 << FIXED_POINT_SHIFT) *
				elapsed_ms / duration_ms +
			((int32_t)prev_color.r << FIXED_POINT_SHIFT);
		new_state.color.g =
			((int32_t)(next_color.g - prev_color.g)
			 << FIXED_POINT_SHIFT) *
				elapsed_ms / duration_ms +
			((int32_t)prev_color.g << FIXED_POINT_SHIFT);
		new_state.color.b =
			((int32_t)(next_color.b - prev_color.b)
			 << FIXED_POINT_SHIFT) *
				elapsed_ms / duration_ms +
			((int32_t)prev_color.b << FIXED_POINT_SHIFT);
		new_state.r_step = DIV_ROUND_NEAREST(
			((int32_t)(next_color.r - prev_color.r)
			 << FIXED_POINT_SHIFT) *
				LED_ANIMATION_TICK_MS,
			duration_ms);
		new_state.g_step = DIV_ROUND_NEAREST(
			((int32_t)(next_color.g - prev_color.g)
			 << FIXED_POINT_SHIFT) *
				LED_ANIMATION_TICK_MS,
			duration_ms);
		new_state.b_step = DIV_ROUND_NEAREST(
			((int32_t)(next_color.b - prev_color.b)
			 << FIXED_POINT_SHIFT) *
				LED_ANIMATION_TICK_MS,
			duration_ms);
		break;

	/* Default blinking or solid color */
	case LED_TRANSITION_STEP:
	default:
		new_state.color.r = next_color.r << FIXED_POINT_SHIFT;
		new_state.color.g = next_color.g << FIXED_POINT_SHIFT;
		new_state.color.b = next_color.b << FIXED_POINT_SHIFT;
		new_state.r_step = 0;
		new_state.g_step = 0;
		new_state.b_step = 0;
		break;
	}

	/* Apply to all LEDs that match the mask */
	uint16_t mask = lb_get_segment_mask(next_conf->segment_id);
	for (int i = 0; i < lb_data.num_leds; i++) {
		if (mask & BIT(i)) {
			lb_state.leds[i] = new_state;
		}
	}
}

static void lb_get_brightness_range(enum ec_led_id led_id,
				    uint8_t *brightness_range)
{
	if (led_id != lb_pins_node[0]->led_id) {
		return;
	}

	memset(brightness_range, 0, EC_LED_COLOR_COUNT);

	for (int i = 0; i < pins_node_count; i++) {
		int color_idx = lb_pins_node[i]->led_color - 1;

		if (color_idx >= 0 && color_idx < EC_LED_COLOR_COUNT) {
			brightness_range[color_idx] = MAX_BRIGHTNESS;
		}
	}
}

static void lb_set_color(enum led_color color, enum ec_led_id led_id,
			 uint8_t brightness)
{
	if (lb_pins_node[0]->led_id == led_id) {
		lb_set_target_enum(color);

		/* Apply requested brightness */
		lb_state.brightness = brightness;
	}

	lb_asynchronous_apply_color(false);
}

void lb_set_and_apply_rgb(struct led_rgb new_color, int8_t segment_id)
{
	lb_set_target_rgb(new_color, segment_id);
	lb_apply_current_color();
}

void lb_get_rgb(struct led_rgb *new_color)
{
	/* TODO(b/462434336): The RGB color needs an inverse conversion */

	/* Return state of first LED */
	new_color->r = lb_state.leds[0].color.r >> FIXED_POINT_SHIFT;
	new_color->g = lb_state.leds[0].color.g >> FIXED_POINT_SHIFT;
	new_color->b = lb_state.leds[0].color.b >> FIXED_POINT_SHIFT;
}

uint8_t lb_get_brightness_pct(void)
{
	return lb_state.brightness;
}

void lb_set_brightness_pct(uint8_t brightness)
{
	lb_state.brightness = CLAMP(brightness, 0, 100);
	lb_apply_current_color();
}

static int lb_set_brightness(enum ec_led_id led_id, const uint8_t *brightness)
{
	bool color_set = false;

	if (led_id != lb_pins_node[0]->led_id) {
		return EC_ERROR_INVAL;
	}

	/*
	 * Iterate backwards, so we pick the highest priority color
	 * (highest enum value) if more than one is set, as the lightbar
	 * can only display one color at a time.
	 */
	for (int i = EC_LED_COLOR_COUNT - 1; i >= 0; i--) {
		if (brightness[i] != 0) {
			/* led_color enum is index + 1 */
			lb_set_target_enum(i + 1);

			/* Apply specific brightness for this color */
			lb_state.brightness = brightness[i];

			color_set = true;
			break;
		}
	}

	if (!color_set) {
		lb_set_target_enum(LED_OFF);
	}

	lb_asynchronous_apply_color(false);
	return EC_SUCCESS;
}

static void lb_asynchronous_apply_color(bool has_transitions)
{
	/* Apply current color and brightness */
	lb_apply_current_color();

	/* Progress to next color for transition */
	if (has_transitions) {
		for (int i = 0; i < lb_data.num_leds; i++) {
			struct lb_led_state *led_state = &lb_state.leds[i];

			led_state->color.r += led_state->r_step;
			led_state->color.g += led_state->g_step;
			led_state->color.b += led_state->b_step;
		}
	}
}

void lightbar_update_strip(void)
{
	static int prev_is_on = -1;
	bool is_on = false;

	if (lb_data.enable_gpio.port) {
		bool actual_is_on = (gpio_pin_get_dt(&lb_data.enable_gpio) > 0);

		if (prev_is_on != -1 && prev_is_on != actual_is_on) {
			LOG_ERR("LB enable GPIO modified outside: exp %d, got %d",
				prev_is_on, actual_is_on);
		}
		prev_is_on = actual_is_on;
	}

	for (uint32_t i = 0; i < lb_data.num_leds; i++) {
		if (lightbar_buf[i].r != 0 || lightbar_buf[i].g != 0 ||
		    lightbar_buf[i].b != 0) {
			is_on = true;
			break;
		}
	}

	if (lb_data.enable_gpio.port) {
		if (is_on && !prev_is_on) {
			gpio_pin_set_dt(&lb_data.enable_gpio, 1);
			k_msleep(RAMPUP_DELAY_MS);
		}
	}

	led_strip_update_rgb(lb_data.strip, lightbar_buf, lb_data.num_leds);

	if (lb_data.enable_gpio.port) {
		if (!is_on && prev_is_on) {
			gpio_pin_set_dt(&lb_data.enable_gpio, 0);
		}
		prev_is_on = is_on;
	}
}

static int init_lightbar(void)
{
	if (lb_data.enable_gpio.port) {
		if (!gpio_is_ready_dt(&lb_data.enable_gpio)) {
			LOG_ERR("Enable GPIO not ready");
			return -ENODEV;
		}
		gpio_pin_configure_dt(&lb_data.enable_gpio,
				      GPIO_OUTPUT_INACTIVE);
	}
	return 0;
}
SYS_INIT(init_lightbar, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static void lb_restore_default_brightness(void)
{
	lb_state.brightness = lb_data.brightness_default_pct;
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, lb_restore_default_brightness,
	     HOOK_PRIO_DEFAULT);
