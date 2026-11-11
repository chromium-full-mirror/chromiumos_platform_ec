/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Lightbar animation translator.
 */

#include "drivers/led.h"
#include "ec_commands.h"
#include "led_lb_host_constant.h"
#include "led_lb_host_program.h"
#include "led_lightbar.h"
#include "pw_lightbar/proto/lb.pb.h"

#include <zephyr/logging/log.h>

#include <pb_decode.h>

LOG_MODULE_REGISTER(lightbar_hostprogram, LOG_LEVEL_INF);

/*
 * Double buffering to prevent corruption when the LED hook is interrupted by
 * the host command task.
 */
#define NUM_BUFFERS 2
static struct pattern_color_node_t
	custom_segments_buffer[NUM_BUFFERS][LIGHTBAR_MAX_CUSTOM_SEGMENT];
static int next_segment;

static struct led_pattern_node_t
	custom_effect_buffer[NUM_BUFFERS][LIGHTBAR_MAX_CUSTOM_EFFECT];
static struct custom_led_patterns_t custom_pattern_buffer[NUM_BUFFERS];

bool lb_is_custom_pattern(const struct led_pattern_node_t *pattern)
{
	const struct led_pattern_node_t *start = &custom_effect_buffer[0][0];
	const struct led_pattern_node_t *end =
		start + (NUM_BUFFERS * LIGHTBAR_MAX_CUSTOM_EFFECT);

	return pattern >= start && pattern < end;
}

struct lb_decode_context {
	int num_effects;
	int total_segments;
	bool has_control;
	uint8_t control_value;
	vendor_google_desktop_lights_LightEffect *current_effect;
};

/* Decode a single segment, will be called for each segment. */
static bool lb_decode_segment_cb(pb_istream_t *stream, const pb_field_t *field,
				 void **arg)
{
	struct lb_decode_context *ctx = (struct lb_decode_context *)*arg;
	struct led_pattern_node_t *custom_effect =
		&custom_effect_buffer[next_segment][ctx->num_effects];
	struct pattern_color_node_t *custom_segments =
		(struct pattern_color_node_t *)custom_effect->pattern_color;
	ARG_UNUSED(field);

	if (ctx->total_segments + custom_effect->pattern_len >=
	    LIGHTBAR_MAX_CUSTOM_SEGMENT) {
		LOG_ERR("Too many segments");
		return false;
	}

	vendor_google_desktop_lights_LightSegment segment =
		vendor_google_desktop_lights_LightSegment_init_default;
	if (!pb_decode(stream, vendor_google_desktop_lights_LightSegment_fields,
		       &segment)) {
		return false;
	}

	/* Check if received a "magic" control value and record it */
	if (((segment.color & LB_CONTROL_MASK) == LB_CONTROL_BASE) &&
	    (segment.color != LB_CONTROL_BASE)) {
		LOG_INF("LB control value (0x%08X) received", segment.color);
		ctx->has_control = true;
		ctx->control_value = 0xff & (segment.color >> LB_CONTROL_SHIFT);
		/* Return "false" as this is a control value, not a lighteffect
		 */
		return false;
	}

	struct led_rgb color = { .r = (segment.color >> 16) & 0xFF,
				 .g = (segment.color >> 8) & 0xFF,
				 .b = segment.color & 0xFF };
	int color_idx =
		lb_lookup_pins_node_srgb(&color, ctx->current_effect->light_id);

	if (color_idx < 0) {
		LOG_ERR("custom_segments %d color (0x%08X) light_id %d not found",
			custom_effect->pattern_len, segment.color,
			ctx->current_effect->light_id);
		return false;
	}

	custom_segments[custom_effect->pattern_len].color_idx = color_idx;
	if (segment.frame > UINT16_MAX) {
		return false;
	}
	/* Store frame for now, will multiply by period later */
	custom_segments[custom_effect->pattern_len].duration_ms = segment.frame;
	custom_effect->pattern_len++;

	return true;
}

/* Decode a single effect, will be called for each effect. */
static bool lb_decode_effect_cb(pb_istream_t *stream, const pb_field_t *field,
				void **arg)
{
	struct lb_decode_context *ctx = (struct lb_decode_context *)*arg;
	ARG_UNUSED(field);

	if (ctx->num_effects >= LIGHTBAR_MAX_CUSTOM_EFFECT) {
		LOG_ERR("Too many effects");
		return false;
	}

	struct led_pattern_node_t *custom_effect =
		&custom_effect_buffer[next_segment][ctx->num_effects];
	struct pattern_color_node_t *custom_segments =
		&custom_segments_buffer[next_segment][ctx->total_segments];

	custom_effect->pattern_color = custom_segments;
	custom_effect->pattern_len = 0;

	vendor_google_desktop_lights_LightEffect effect =
		vendor_google_desktop_lights_LightEffect_init_default;
	effect.segment.funcs.decode = lb_decode_segment_cb;
	effect.segment.arg = ctx;

	ctx->current_effect = &effect;

	if (!pb_decode(stream, vendor_google_desktop_lights_LightEffect_fields,
		       &effect))
		return false;

	if (custom_effect->pattern_len == 0) {
		LOG_ERR("No pattern defined");
		return false;
	}

	custom_effect->transition =
		(effect.type == vendor_google_desktop_lights_InterpolationType_LINEAR ?
			 LED_TRANSITION_LINEAR :
			 LED_TRANSITION_STEP);
	custom_effect->led_id = EC_LED_ID_LIGHTBAR_LED;
	if (effect.iterations > UINT8_MAX) {
		return false;
	}
	custom_effect->cycle_limit = effect.iterations;

	for (int i = 0; i < custom_effect->pattern_len; i++) {
		uint64_t duration = (uint64_t)custom_segments[i].duration_ms *
				    effect.period;

		if (duration > UINT16_MAX) {
			return false;
		}
		custom_segments[i].duration_ms = (uint16_t)duration;
	}

	ctx->total_segments += custom_effect->pattern_len;
	ctx->num_effects++;

	return true;
}

enum ec_status host_program_sequence(const uint8_t *custom_sequence_buffer,
				     size_t custom_sequence_buffer_size)
{
	struct custom_led_patterns_t *custom_pattern =
		&custom_pattern_buffer[next_segment];
	pb_istream_t stream = pb_istream_from_buffer(
		custom_sequence_buffer, custom_sequence_buffer_size);
	vendor_google_desktop_lights_LightEffects light_effects =
		vendor_google_desktop_lights_LightEffects_init_default;
	struct lb_decode_context ctx = { .num_effects = 0,
					 .total_segments = 0,
					 .has_control = false,
					 .control_value = 0 };

	light_effects.effect.funcs.decode = lb_decode_effect_cb;
	light_effects.effect.arg = &ctx;
	if (!pb_decode(&stream,
		       vendor_google_desktop_lights_LightEffects_fields,
		       &light_effects)) {
		/*
		 * If decoding stopped due to a control value, process it now
		 * that the Nanopb recursion stack frames have unwound.
		 */
		if (ctx.has_control) {
			lb_control_value_process(ctx.control_value);
			return EC_RES_SUCCESS;
		}
		LOG_ERR("Failed to decode lighteffect");
		return EC_RES_INVALID_PARAM;
	}

	custom_pattern->num_patterns = ctx.num_effects;
	custom_pattern->led_patterns = custom_effect_buffer[next_segment];
	custom_pattern->led_id = EC_LED_ID_LIGHTBAR_LED;
	lb_cancel_one_shot(custom_pattern);
	led_set_custom_patterns(custom_pattern);

	next_segment ^= 1;
	return EC_RES_SUCCESS;
}
