/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Devicetree preprocessor macros for manipulating RGB color values.
 */

#ifndef PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LED_COLORS_UTIL_H_
#define PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LED_COLORS_UTIL_H_

/* sRGB values of the Golden colors */
#define SRGB_RED_BASE 0xEA4335
#define SRGB_GREEN_BASE 0x34A853
#define SRGB_BLUE_BASE 0x4285F4
#define SRGB_YELLOW_BASE 0xFBBC04
#define SRGB_WHITE_BASE 0xFFFFFF
#define SRGB_PURPLE_BASE 0xC97BE2
#define SRGB_OFF_BASE 0x000000

#define LIGHTBAR_OFF_BASE 0x000000

/* Extracts the R/G/B component from a 24-bit RGB value */
#define DT_COLOR_GET_R(rgb) (((rgb) >> 16) & 0xFF)
#define DT_COLOR_GET_G(rgb) (((rgb) >> 8) & 0xFF)
#define DT_COLOR_GET_B(rgb) ((rgb) & 0xFF)

#define SUM_RGB(rgb) \
	(DT_COLOR_GET_R(rgb) + DT_COLOR_GET_G(rgb) + DT_COLOR_GET_B(rgb))

/* Scales a single 8-bit color component by a percentage */
#define DT_COLOR_SCALE_COMP(component, percent) \
	(((component) * (percent)) / 100)

/* Fails build via div-by-zero if value exceeds tolerance */
#define DT_COLOR_ROUNDING_TOLERANCE 1
#define DT_COLOR_CLAMP_8BIT(v) ((v) > 255 ? 255 : (v))
#define DT_COLOR_ASSERT_8BIT(v) \
	(DT_COLOR_CLAMP_8BIT(v) / ((v) <= 255 + DT_COLOR_ROUNDING_TOLERANCE))

/* Recombines R, G, B components into a 24-bit RGB value */
#define DT_COLOR_FROM_RGB(r, g, b)                                          \
	((DT_COLOR_ASSERT_8BIT(r) << 16) | (DT_COLOR_ASSERT_8BIT(g) << 8) | \
	 (DT_COLOR_ASSERT_8BIT(b)))

/*
 * Main macro: Scales a 24-bit RGB color by a given percentage.
 * Example: SCALE_COLOR(0xFF8000, 50) -> 0x7F4000
 */
#define SCALE_COLOR(rgb, percent)                                            \
	DT_COLOR_FROM_RGB(DT_COLOR_SCALE_COMP(DT_COLOR_GET_R(rgb), percent), \
			  DT_COLOR_SCALE_COMP(DT_COLOR_GET_G(rgb), percent), \
			  DT_COLOR_SCALE_COMP(DT_COLOR_GET_B(rgb), percent))

/* Scales a single 8-bit color component upwards */
#define DT_COLOR_RESCALE_COMP(component, percent) \
	(((component) * 100) / (percent))

/*
 * Main macro: Performs an inverse scale of a 24-bit RGB color.
 * Example: RESCALE_COLOR(0x7F4000, 50) -> 0xFE8000
 */
#define RESCALE_COLOR(rgb, percent)                                            \
	DT_COLOR_FROM_RGB(DT_COLOR_RESCALE_COMP(DT_COLOR_GET_R(rgb), percent), \
			  DT_COLOR_RESCALE_COMP(DT_COLOR_GET_G(rgb), percent), \
			  DT_COLOR_RESCALE_COMP(DT_COLOR_GET_B(rgb), percent))

/* Helper to automate the rescale logic for lightbar base colors */
#define LB_BASE_RESCALE(color) \
	RESCALE_COLOR(LIGHTBAR_##color##_GOLDEN, LIGHTBAR_##color##_REF_PCT)

/* Helper to automate the rescale logic for alternative golden color sets */
#define LB_ALT_BASE_RESCALE(set, color)                  \
	RESCALE_COLOR(LIGHTBAR_##set##_##color##_GOLDEN, \
		      LIGHTBAR_##color##_REF_PCT)

/*
 * Apply a gamma correction (gamma=2.2) to a brightness percentage (0-100).
 *
 * This maps the human eye's linear perception of brightness to the actual
 * PWM duty cycle.
 *
 * Standard Gamma 2.2 uses the formula `y = x^2.2`. Since calculating floating
 * point exponents is computationally expensive for the EC, we use a cubic
 * polynomial approximation that closely fits the curve:
 *     y = 0.75 x^2 + 0.25 x^3
 *
 * To implement this using integer arithmetic for an input 'v' (0-100):
 * 1. We substitute `x = v / 100`.
 * 2. Expanding the terms and finding a common denominator (40000) gives us:
 *    y = (300 * v^2 + v^3) / 40000
 * 3. Factoring out `v^2` simplifies the numerator to: `v^2 * (300 + v)`.
 * 4. Finally, we add a rounding factor (20000) for integer division.
 *
 * Mapping examples:
 *  100 -> 100
 *  75  -> 53
 *  50  -> 22
 *  25  -> 5
 *  0   -> 0
 */
#define LED_GAMMA(v) (((v) * (v) * (300 + (v)) + 20000) / 40000)

#endif /* PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LED_COLORS_UTIL_H_ */
