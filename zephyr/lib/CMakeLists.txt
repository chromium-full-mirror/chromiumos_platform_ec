# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

if(CONFIG_ZEPHYR_PIGWEED_MODULE)
  add_subdirectory(math)
  add_subdirectory(vsensor)
endif()

add_subdirectory(ap_power_console)
