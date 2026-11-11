/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __CROS_EC_LED_LB_HOST_PROGRAM_H
#define __CROS_EC_LED_LB_HOST_PROGRAM_H

#include "ec_commands.h"

#include <stddef.h>
#include <stdint.h>

/* TODO(gwendal): Can be optimized by calculating the largest possible protobuf.
 */
#define EC_LB_PROG_EX_LEN (1536)

#define LB_CONTROL_MASK 0xffffff
#define LB_CONTROL_BASE 0x90091e
#define LB_CONTROL_SHIFT 24

enum ec_status host_program_sequence(const uint8_t *custom_sequence_buffer,
				     size_t custom_sequence_buffer_size);

#endif /* __CROS_EC_LED_LB_HOST_PROGRAM_H */
