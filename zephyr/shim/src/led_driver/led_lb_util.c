/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Lightbar utility.
 */

#include "led_lb_util.h"
#include "util.h"

#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * Normalization scaling factor: 2^10 = 1024
 * Provides ~0.1% precision for chromaticity. Distance calculations
 * (dr^2 + dg^2 + db^2) fit in int32_t.
 */
#define LB_NORM_SCALE 1024

/*
 * IDW weight scalar: 2^18 = 262,144
 * A weight of (2^18 / dist_sq) ensures that sum_ref_brightness (weight * 765)
 * only reaches ~200 million per color. For 6 colors, the total (~1.2 billion)
 * stays safely within the int32_t limit (2.1 billion), even if color overlap.
 */
#define LB_WEIGHT_SCALAR 262144

/*
 * Threshold for "Exact Hue Match" in 1024-scaled chromaticity space.
 * 32 covers a +/- 1 bit jitter in raw RGB components (4^2 + 4^2).
 */
#define LB_HUE_SNAP_THRESHOLD 32

#define RGB_MAX_VAL UINT8_MAX

/* Indices for RGB component loops */
enum { RGB_R = 0, RGB_G, RGB_B, RGB_COUNT };

/*
 * Scales color components by a multiplier and divides by a divisor.
 * Used for both normalization and denormalization.
 */
static void scale_rgb(int32_t *out, const struct led_rgb *in, int32_t mul,
		      int32_t div)
{
	/* Treat the struct as an array of 3 bytes (R, G, B) */
	const uint8_t *in_arr = (const uint8_t *)in;

	for (int i = 0; i < RGB_COUNT; i++) {
		out[i] = ((int32_t)in_arr[i] * mul) / div;
	}
}

/**
 * Perform Normalized Inverse Distance Weighting (IDW) conversion.
 *
 * The algorithm separates chromaticity (hue) from brightness to ensure accurate
 * interpolation across different intensity levels. It calculates the weighted
 * average of the Golden colors based on proximity in the normalized space,
 * then scales the result to match the input's relative brightness.
 */
int lb_calculate_idw(const struct led_rgb srgb, struct led_rgb *color,
		     const struct lb_calibration_point *points, size_t count)
{
	union {
		struct led_rgb *rgb;
		uint8_t *arr;
	} out = { .rgb = color };
	int32_t sum_color[RGB_COUNT] = { 0 };
	int32_t s_norm[RGB_COUNT];
	int32_t sum_ref_brightness = 0;
	int32_t total_weight = 0;
	int32_t input_sum = srgb.r + srgb.g + srgb.b;

	*(out.rgb) = (struct led_rgb){ 0, 0, 0 };

	if (input_sum == 0) {
		return EC_SUCCESS;
	}

	/* Calculate normalized input coordinates (chromaticity) */
	scale_rgb(s_norm, &srgb, LB_NORM_SCALE, input_sum);

	for (int i = 0; i < count; i++) {
		const struct lb_calibration_point *point = &points[i];
		int32_t ref_sum = point->srgb.r + point->srgb.g + point->srgb.b;

		/* Skip invalid reference values */
		if (ref_sum == 0) {
			continue;
		}

		/* Calculate normalized reference coordinates */
		int32_t r_norm[RGB_COUNT];
		scale_rgb(r_norm, &point->srgb, LB_NORM_SCALE, ref_sum);

		/* Squared Euclidean distance in normalized space */
		int32_t dist_sq = 0;
		for (int j = 0; j < RGB_COUNT; j++) {
			int32_t d = s_norm[j] - r_norm[j];
			dist_sq += d * d;
		}

		/*
		 * Exact hue match: If the input hue is close enough to a
		 * golden hue, we ignore others and scale this golden value
		 * directly to match input brightness.
		 */
		if (dist_sq <= LB_HUE_SNAP_THRESHOLD) {
			int32_t snap_c[RGB_COUNT];

			scale_rgb(snap_c, &point->color, input_sum, ref_sum);
			for (int j = 0; j < RGB_COUNT; j++) {
				out.arr[j] = min(snap_c[j], RGB_MAX_VAL);
			}
			return EC_SUCCESS;
		}

		/* Standard IDW weight: w = 1 / d^2 */
		int32_t weight = LB_WEIGHT_SCALAR / dist_sq;
		const uint8_t *ref_color = (const uint8_t *)&point->color;

		total_weight += weight;
		for (int j = 0; j < RGB_COUNT; j++) {
			sum_color[j] += weight * ref_color[j];
		}
		sum_ref_brightness += weight * ref_sum;
	}

	if (total_weight > 0) {
		/* Calculate the weighted average reference brightness */
		int32_t avg_ref_sum =
			DIV_ROUND_NEAREST(sum_ref_brightness, total_weight);

		if (avg_ref_sum > 0) {
			for (int j = 0; j < RGB_COUNT; j++) {
				/* Calculate the weighted average HW hue */
				int32_t avg_c = DIV_ROUND_NEAREST(sum_color[j],
								  total_weight);

				/* Apply brightness scaling (denormalization) */
				int32_t final_c = DIV_ROUND_NEAREST(
					avg_c * input_sum, avg_ref_sum);
				out.arr[j] = min(final_c, RGB_MAX_VAL);
			}
			return EC_SUCCESS;
		}
	}
	/*
	 * No Golden colors matched; this can happen if the Golden Table
	 * is empty or all distances were too large for the scalar.
	 */
	return EC_ERROR_INVAL;
}
