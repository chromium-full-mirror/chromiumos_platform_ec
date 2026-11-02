/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "drivers/cros_flash.h"
#include "fpsensor/fpsensor_vendor_data.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>

#include <bitset>
#include <cstring>
#include <ranges>

#ifndef CONFIG_FLASH_ERASE_SIZE
#error "CONFIG_FLASH_ERASE_SIZE is not defined"
#endif

namespace
{
constexpr const struct device *const flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
constexpr const struct device *const cros_flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(cros_ec_flash_controller));

constexpr off_t VENDOR_DATA_OFF =
	DT_REG_ADDR(DT_NODELABEL(fingerprint_sensor_vendor_data));
constexpr size_t VENDOR_DATA_SIZE =
	DT_REG_SIZE(DT_NODELABEL(fingerprint_sensor_vendor_data));
constexpr size_t VENDOR_DATA_SECTOR_SIZE = CONFIG_FLASH_ERASE_SIZE;

BUILD_ASSERT(VENDOR_DATA_SIZE > 0, "Vendor data partition must not be empty");
BUILD_ASSERT(VENDOR_DATA_SIZE % VENDOR_DATA_SECTOR_SIZE == 0,
	     "Vendor data partition size must be a multiple of the erase size");
BUILD_ASSERT(VENDOR_DATA_OFF % VENDOR_DATA_SECTOR_SIZE == 0,
	     "Vendor data partition must start on an erase sector boundary");

constexpr size_t VENDOR_DATA_SECTORS =
	VENDOR_DATA_SIZE / VENDOR_DATA_SECTOR_SIZE;

using SectorMask = std::bitset<VENDOR_DATA_SECTORS>;

/* Mask of the sectors overlapping [offset, offset + size). */
SectorMask sectors(uint32_t offset, size_t size)
{
	SectorMask mask;

	if (size == 0) {
		return mask;
	}

	size_t begin = offset / VENDOR_DATA_SECTOR_SIZE;
	size_t end = (offset + size - 1) / VENDOR_DATA_SECTOR_SIZE;

	for (size_t s = begin; s <= end; s++) {
		mask.set(s);
	}

	return mask;
}

/*
 * Iterate over the indices of the sectors set in the mask. The mask is taken
 * by value and captured by value on purpose: the view may outlive a temporary
 * passed in and the caller may modify the original mask while iterating.
 */
auto sectors_iter(SectorMask mask)
{
	return std::views::iota(size_t{ 0 }, VENDOR_DATA_SECTORS) |
	       std::views::filter([mask](size_t i) { return mask[i]; });
}

/* Overflow-safe check that [offset, offset + size) lies within the buffer. */
constexpr bool in_bounds(uint32_t offset, size_t size)
{
	return size <= VENDOR_DATA_SIZE && offset <= VENDOR_DATA_SIZE - size;
}

/* Set once vendor_data mirrors the flash content. */
bool initialized;
/* Modified sectors bitmask. */
SectorMask modified;
/* Vendor data buffer. */
alignas(CONFIG_PLATFORM_EC_FP_VENDOR_DATA_ALIGNMENT) uint8_t
	vendor_data[VENDOR_DATA_SIZE];
}

int fp_vendor_data_init(void)
{
	int rc;

	initialized = false;
	modified.reset();

	if (!device_is_ready(flash_dev) || !device_is_ready(cros_flash_dev)) {
		return -ENODEV;
	}

	rc = flash_read(flash_dev, VENDOR_DATA_OFF, vendor_data,
			VENDOR_DATA_SIZE);
	if (rc != 0) {
		return rc;
	}

	initialized = true;

	return 0;
}

int fp_vendor_data_read(uint32_t offset, size_t size, void *data)
{
	if (data == nullptr || !in_bounds(offset, size))
		return -EINVAL;

	if (!initialized)
		return -ENODEV;

	memcpy(data, vendor_data + offset, size);

	return 0;
}

int fp_vendor_data_write(uint32_t offset, size_t size, const void *data)
{
	if (data == nullptr || !in_bounds(offset, size))
		return -EINVAL;

	if (!initialized)
		return -ENODEV;

	memcpy(vendor_data + offset, data, size);
	modified |= sectors(offset, size);

	return 0;
}

int fp_vendor_data_erase(uint32_t offset, size_t size)
{
	if (!in_bounds(offset, size))
		return -EINVAL;

	if (!initialized)
		return -ENODEV;

	const uint8_t erase_value =
		flash_get_parameters(flash_dev)->erase_value;

	memset(vendor_data + offset, erase_value, size);
	modified |= sectors(offset, size);

	return 0;
}

bool fp_vendor_data_pending(void)
{
	return modified.any();
}

int fp_vendor_data_sync(void)
{
	int rc;

	if (!initialized) {
		return -ENODEV;
	}

	if (modified.none()) {
		return 0;
	}

	for (size_t s : sectors_iter(modified)) {
		off_t sector_offset = s * VENDOR_DATA_SECTOR_SIZE;

#ifdef CONFIG_PLATFORM_EC_MAPPED_STORAGE
		/*
		 * The sensor library tends to rewrite its whole data set even
		 * if little or nothing changed. With memory mapped flash the
		 * sector can be compared in place; skip the erase/program
		 * cycle if its content is already there.
		 */
		const uint8_t *flash = reinterpret_cast<const uint8_t *>(
			CONFIG_FLASH_BASE_ADDRESS + VENDOR_DATA_OFF +
			sector_offset);

		if (memcmp(vendor_data + sector_offset, flash,
			   VENDOR_DATA_SECTOR_SIZE) == 0) {
			modified.reset(s);
			continue;
		}
#endif

		rc = cros_flash_physical_erase(cros_flash_dev,
					       VENDOR_DATA_OFF + sector_offset,
					       VENDOR_DATA_SECTOR_SIZE);
		if (rc != 0) {
			return rc < 0 ? rc : -EIO;
		}

		rc = cros_flash_physical_write(
			cros_flash_dev, VENDOR_DATA_OFF + sector_offset,
			VENDOR_DATA_SECTOR_SIZE,
			reinterpret_cast<const char *>(vendor_data +
						       sector_offset));
		if (rc != 0) {
			return rc < 0 ? rc : -EIO;
		}

		modified.reset(s);
	}

	return 0;
}
