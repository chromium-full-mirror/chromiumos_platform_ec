# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

if (CONFIG_PLATFORM_EC_GESTURE_SENSOR_DOUBLE_TAP)
	target_sources(app PRIVATE src/test_double_tap.c)
else()
	target_sources(app PRIVATE src/test_common.c src/test_interrupt.c)
endif()
