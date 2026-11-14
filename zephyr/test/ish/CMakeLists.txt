# Copyright 2024 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS "${ZEPHYR_BASE}")
project(ish)

add_subdirectory(${PLATFORM_EC}/zephyr/test/test_utils test_utils)

if(CONFIG_TEST_ISH_HECI)
	target_include_directories(app PRIVATE
		${PLATFORM_EC}/zephyr/include/host_service
	)
	target_sources(app PRIVATE
		src/test_host_command_heci.c
		${PLATFORM_EC}/zephyr/shim/src/host_command_heci.c
		${PLATFORM_EC}/zephyr/shim/src/ish_system.c
	)
else()
	target_sources(app PRIVATE src/nb_mode.c)
endif()
