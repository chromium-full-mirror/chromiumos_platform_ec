# Copyright 2022 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

zephyr_include_directories(.)

zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT      led.c)
zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_GPIO led_gpio.c)
zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_PWM  led_pwm.c)

if (CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR)
  include(${ZEPHYR_PIGWEED_MODULE_DIR}/pw_build/pigweed.cmake)
  include(${ZEPHYR_PIGWEED_MODULE_DIR}/pw_protobuf_compiler/proto.cmake)

  set(pw_protobuf_compiler_GENERATE_PROTOS_ARGS "--no-experimental-editions"
      CACHE STRING "Args to generate_protos.py" FORCE)

  # Number of effects per animation (the maximal number of LED presented).
  set(LIGHTBAR_MAX_CUSTOM_EFFECT 8 CACHE STRING "")

  # Total number of segments (keyframes) across all effects in an animation.
  set(LIGHTBAR_MAX_CUSTOM_SEGMENT 72 CACHE STRING "")

  configure_file(
    include/led_lb_host_constant.h.in
    include/led_lb_host_constant.h
    @ONLY
  )

  configure_file(proto/lb.options.in proto/lb.options @ONLY)
  file(
    COPY
      "${CMAKE_CURRENT_SOURCE_DIR}/proto/lb.proto"
    DESTINATION
      "${CMAKE_CURRENT_BINARY_DIR}/proto"
  )

  pw_proto_library(cros_lb.proto
    SOURCES
      "${CMAKE_CURRENT_BINARY_DIR}/proto/lb.proto"
    INPUTS
      "${CMAKE_CURRENT_BINARY_DIR}/proto/lb.options"
    STRIP_PREFIX
      "${CMAKE_CURRENT_BINARY_DIR}"
    PREFIX
      "pw_lightbar"
    DEPS
      pw_third_party.nanopb.proto
  )

  pw_add_library(led_lb_host_program STATIC
    SOURCES
      led_lb_host_program.c
    PRIVATE_DEPS
      cros_lb.proto.nanopb
      zephyr_interface
      cros_ec_interface
  )
  # Depends on the generated headers for heap_constants.h, may be redundant
  # since the one above depends on zephyr_interface but let's lay on the safe
  # side since the error is a build time race condition that can be hard to
  # reproduce.
  add_dependencies(led_lb_host_program zephyr_generated_headers)

  target_include_directories(led_lb_host_program
    PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/include)

  zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)
  zephyr_library_link_libraries_ifdef(CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR
                                      led_lb_host_program)

  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR
                               led_lightbar.c)
  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_CBI_UFSC_LIGHTBAR
                               led_lb_ufsc_selector.c)
  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LB_CONSOLE_CMD led_lb_cmd.c)
  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR
                               led_lb_host_cmd.c)
  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR
                               led_lb_policy_alt.c)
  zephyr_library_sources_ifdef(CONFIG_PLATFORM_EC_LED_DT_LIGHTBAR led_lb_util.c)
endif()
