# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS "${ZEPHYR_BASE}")
project(lightbar_test)

set(LIGHTBAR_MAX_CUSTOM_EFFECT 12)
set(LIGHTBAR_MAX_CUSTOM_SEGMENT 80)
configure_file(
    ${PLATFORM_EC}/zephyr/shim/src/led_driver/include/led_lb_host_constant.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/led_lb_host_constant.h
    @ONLY
)

zephyr_include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${PLATFORM_EC}/zephyr/shim/src/led_driver
)

target_sources(app PRIVATE
    src/test_lightbar.c
    ${PLATFORM_EC}/zephyr/shim/src/led_driver/led_lightbar.c
    ${PLATFORM_EC}/zephyr/shim/src/led_driver/led_lb_cmd.c
    ${PLATFORM_EC}/zephyr/shim/src/led_driver/led_lb_util.c
)

if(CONFIG_DT_HAS_CROS_EC_CBI_UFSC_LIGHTBAR_SELECTOR_ENABLED)
    target_sources(app PRIVATE
        ${PLATFORM_EC}/zephyr/shim/src/led_driver/led_lb_ufsc_selector.c
    )
    target_compile_definitions(app PRIVATE
        CONFIG_PLATFORM_EC_CBI_UFSC_LIGHTBAR=1
    )
endif()

target_compile_definitions(app PRIVATE
    CONFIG_PLATFORM_EC_LED_ANIMATION_TICK_MS=30
)

target_compile_options(app PRIVATE
    -Wno-c23-extensions
)
