/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_LED_LB_UTIL_H
#define __CROS_EC_LED_LB_UTIL_H

#include "led_common.h"

#include <zephyr/drivers/led_strip.h>

struct lb_calibration_point {
	struct led_rgb srgb;
	struct led_rgb color;
};

/**
 * Perform Normalized Inverse Distance Weighting (IDW) conversion.
 *
 * @param srgb   Input sRGB value.
 * @param color  Pointer to store the resulting hardware RGB color.
 * @param points Pointer to the array of golden calibration points.
 * @param count  Number of points.
 * @return EC_SUCCESS on success, error code otherwise.
 */
int lb_calculate_idw(const struct led_rgb srgb, struct led_rgb *color,
		     const struct lb_calibration_point *points, size_t count);

#endif /* __CROS_EC_LED_LB_UTIL_H */
