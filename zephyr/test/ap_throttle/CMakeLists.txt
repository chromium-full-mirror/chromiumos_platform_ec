# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS "${ZEPHYR_BASE}")
project(ap_throttle)

add_subdirectory(${PLATFORM_EC}/zephyr/test/test_utils test_utils)

zephyr_compile_definitions(
	CONFIG_PLATFORM_EC_BATTERY_PRESENT_CUSTOM
	CONFIG_TEST_DISABLE_INLINE_CHIPSET_IN_STATE
)

target_sources(app PRIVATE src/main.c)
