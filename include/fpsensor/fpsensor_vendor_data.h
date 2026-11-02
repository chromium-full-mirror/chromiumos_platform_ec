/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef PLATFORM_EC_INCLUDE_FPSENSOR_FPSENSOR_VENDOR_DATA_H_
#define PLATFORM_EC_INCLUDE_FPSENSOR_FPSENSOR_VENDOR_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write-back cache for vendor specific fingerprint sensor data kept in the
 * fingerprint_sensor_vendor_data flash partition.
 *
 * The whole partition is mirrored in RAM by fp_vendor_data_init(). Reads,
 * writes and erases then operate on the RAM copy only; modified flash sectors
 * are written back by fp_vendor_data_sync().
 *
 * The API is not thread-safe: all functions must be called from the
 * fingerprint task.
 */

/**
 * @brief Loads the vendor data from persistent storage into the buffer.
 *
 * Must be called once before any other fp_vendor_data_*() function; until
 * it succeeds they fail with -ENODEV. Pending modifications are discarded.
 *
 * @return 0 on success, negative error code otherwise.
 */
int fp_vendor_data_init(void);

/**
 * @brief Reads vendor data from the buffer.
 *
 * @param offset Byte offset into the vendor data.
 * @param size Number of bytes to read.
 * @param data Destination buffer.
 * @return 0 on success, negative error code otherwise.
 */
int fp_vendor_data_read(uint32_t offset, size_t size, void *data);

/**
 * @brief Writes vendor data to the buffer.
 *
 * @param offset Byte offset into the vendor data.
 * @param size Number of bytes to write.
 * @param data Source buffer.
 * @return 0 on success, negative error code otherwise.
 */
int fp_vendor_data_write(uint32_t offset, size_t size, const void *data);

/**
 * @brief Erases vendor data in the buffer.
 *
 * @param offset Byte offset into the vendor data.
 * @param size Number of bytes to erase.
 * @return 0 on success, negative error code otherwise.
 */
int fp_vendor_data_erase(uint32_t offset, size_t size);

/**
 * @brief Check whether the buffer has modifications not yet written back.
 *
 * @return true if fp_vendor_data_sync() has work to do, false otherwise.
 */
bool fp_vendor_data_pending(void);

/**
 * @brief Synchronize content of the buffer with persistent storage.
 *
 * Only the flash sectors modified since the last successful sync are erased
 * and programmed. On failure the remaining modifications stay pending.
 *
 * @return 0 on success, negative error code otherwise.
 */
int fp_vendor_data_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_EC_INCLUDE_FPSENSOR_FPSENSOR_VENDOR_DATA_H_ */
