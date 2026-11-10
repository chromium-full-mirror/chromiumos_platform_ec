/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef PLATFORM_EC_ZEPHYR_TEST_FINGERPRINT_DRIVERS_EGIS660_SRC_EGIS660_PAL_TEST_HELPERS_H_
#define PLATFORM_EC_ZEPHYR_TEST_FINGERPRINT_DRIVERS_EGIS660_SRC_EGIS660_PAL_TEST_HELPERS_H_

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

/* Size of the simulated vendor data partition backing the APNS storage */
#define APNS_TEST_SIZE DT_REG_SIZE(DT_NODELABEL(fingerprint_sensor_vendor_data))

__syscall int egis660_pal_spi_write_read(uint8_t *data, size_t write_size,
					 size_t read_size,
					 bool leave_cs_asserted);
__syscall bool egis660_pal_check_irq(void);
__syscall bool egis660_pal_read_irq(void);
__syscall void egis660_pal_reset(bool state);
__syscall uint32_t egis660_pal_timebase_get_tick(void);
__syscall void egis660_pal_timebase_delay_us(uint32_t us);
__syscall void egis660_pal_timebase_delay_ms(uint32_t ms);
__syscall void *egis660_pal_malloc(uint32_t size);
__syscall void egis660_pal_free(void *data);

/* Storage test helpers — wrap the APNS storage PAL */
__syscall int egis660_pal_flash_erase(uint32_t offset, size_t size);
__syscall int egis660_pal_flash_read(uint32_t offset, size_t size, void *data);
__syscall int egis660_pal_flash_write(uint32_t offset, const void *data,
				      size_t size);

#include <zephyr/syscalls/egis660_pal_test_helpers.h>

#endif /* PLATFORM_EC_ZEPHYR_TEST_FINGERPRINT_DRIVERS_EGIS660_SRC_EGIS660_PAL_TEST_HELPERS_H_ \
	*/
