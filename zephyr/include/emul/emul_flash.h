/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/**
 * @file
 *
 * @brief Backend API for Cros flash emulator
 */

#ifndef PLATFORM_EC_ZEPHYR_INCLUDE_EMUL_EMUL_FLASH_H_
#define PLATFORM_EC_ZEPHYR_INCLUDE_EMUL_EMUL_FLASH_H_

#include <ec_commands.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the protection.
 */
void cros_flash_emul_protect_reset(void);

/**
 * @brief Enable the protection.
 */
void cros_flash_emul_enable_protect(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_EC_ZEPHYR_INCLUDE_EMUL_EMUL_FLASH_H_ */
