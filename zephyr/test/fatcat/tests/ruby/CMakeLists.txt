# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

zephyr_include_directories(src)

target_sources(app PRIVATE
	src/fan.c
	${PLATFORM_EC_PROGRAM_DIR}/fatcat/ruby/src/fan.c
)
