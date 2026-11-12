# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS "${ZEPHYR_BASE}")
project(ocelot)

zephyr_include_directories("${PLATFORM_EC_PROGRAM_DIR}/ocelot/include")

add_subdirectory(${PLATFORM_EC}/zephyr/test/test_utils test_utils)

target_sources_ifdef(CONFIG_TEST_OCELOT_RVP_BOARD_ID app PRIVATE
	${PLATFORM_EC_PROGRAM_DIR}/ocelot/src/rvp_board_id.c
	src/rvp_board_id_test.c
)

if(CONFIG_TEST_OCELOT_BLANC_BOARD)
	# CHARGER_SOLO and CONFIG_PLATFORM_EC_CHARGER_BQ25720 are defined here
	# rather than through Kconfig so the charger driver is not built: the
	# test provides its own chg_chips[] table.
	target_compile_definitions(app PRIVATE
		CHARGER_SOLO=0
		CONFIG_PLATFORM_EC_CHARGER_BQ25720=1)

	target_sources(app PRIVATE
		${PLATFORM_EC_PROGRAM_DIR}/ocelot/blanc/src/board.c
		src/blanc_board_test.c)
endif()

set_compiler_property(APPEND PROPERTY coverage -O0)
