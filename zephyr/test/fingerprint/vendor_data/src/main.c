/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "fpsensor/fpsensor_vendor_data.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/ztest.h>

#define VD_ADDR DT_REG_ADDR(DT_NODELABEL(fingerprint_sensor_vendor_data))
#define VD_SIZE DT_REG_SIZE(DT_NODELABEL(fingerprint_sensor_vendor_data))
#define SECTOR_SIZE CONFIG_FLASH_ERASE_SIZE
#define SECTORS (VD_SIZE / SECTOR_SIZE)

BUILD_ASSERT(SECTORS >= 4, "Tests assume at least 4 sectors");

static const struct device *const flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

static uint8_t erase_value;
static uint8_t sector_buf[SECTOR_SIZE];

/* Deterministic, sector-dependent content so neighbours are distinguishable. */
static uint8_t pattern(size_t sector, size_t i)
{
	return (uint8_t)(sector * 37 + i);
}

static void fill_pattern(uint8_t *buf, size_t sector, uint8_t salt)
{
	for (size_t i = 0; i < SECTOR_SIZE; i++) {
		buf[i] = pattern(sector, i) ^ salt;
	}
}

/* Write a whole sector directly to the simulated flash, bypassing the cache. */
static void flash_fill_sector(size_t sector, uint8_t salt)
{
	off_t addr = VD_ADDR + sector * SECTOR_SIZE;

	fill_pattern(sector_buf, sector, salt);
	zassert_ok(flash_erase(flash_dev, addr, SECTOR_SIZE));
	zassert_ok(flash_write(flash_dev, addr, sector_buf, SECTOR_SIZE));
}

/* Read a whole sector directly from the simulated flash. */
static void flash_fetch_sector(size_t sector, uint8_t *buf)
{
	off_t addr = VD_ADDR + sector * SECTOR_SIZE;

	zassert_ok(flash_read(flash_dev, addr, buf, SECTOR_SIZE));
}

static void assert_sector_in_flash_matches(size_t sector, uint8_t salt)
{
	uint8_t expected[SECTOR_SIZE];

	fill_pattern(expected, sector, salt);
	flash_fetch_sector(sector, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE,
			  "flash sector %zu content mismatch", sector);
}

static void *suite_setup(void)
{
	zassert_true(device_is_ready(flash_dev));
	erase_value = flash_get_parameters(flash_dev)->erase_value;

	return NULL;
}

static void test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	for (size_t s = 0; s < SECTORS; s++) {
		flash_fill_sector(s, 0);
	}
	/* Mirror the fresh flash content and drop pending modifications. */
	zassert_ok(fp_vendor_data_init());
}

ZTEST_SUITE(fp_vendor_data, NULL, suite_setup, test_before, NULL, NULL);

/* --- Initialization ------------------------------------------------------ */

ZTEST(fp_vendor_data, test_init_loads_flash_content)
{
	uint8_t expected[SECTOR_SIZE];

	/* Change flash behind the cache's back and re-init. */
	for (size_t s = 0; s < SECTORS; s++) {
		flash_fill_sector(s, 0xa5);
	}
	zassert_ok(fp_vendor_data_init());

	for (size_t s = 0; s < SECTORS; s++) {
		fill_pattern(expected, s, 0xa5);
		zassert_ok(fp_vendor_data_read(s * SECTOR_SIZE, SECTOR_SIZE,
					       sector_buf));
		zassert_mem_equal(sector_buf, expected, SECTOR_SIZE,
				  "sector %zu", s);
	}
}

ZTEST(fp_vendor_data, test_init_discards_pending_modifications)
{
	const uint8_t data[] = { 0x12, 0x34 };

	zassert_ok(fp_vendor_data_write(0, sizeof(data), data));
	zassert_true(fp_vendor_data_pending());

	zassert_ok(fp_vendor_data_init());
	zassert_false(fp_vendor_data_pending());

	/* The buffer holds the flash content again. */
	zassert_ok(fp_vendor_data_read(0, SECTOR_SIZE, sector_buf));
	zassert_equal(sector_buf[0], pattern(0, 0));
	zassert_equal(sector_buf[1], pattern(0, 1));

	/* And nothing reaches flash on sync. */
	zassert_ok(fp_vendor_data_sync());
	assert_sector_in_flash_matches(0, 0);
}

/* --- Argument validation ------------------------------------------------- */

ZTEST(fp_vendor_data, test_read_rejects_out_of_bounds)
{
	uint8_t buf[4];

	zassert_equal(fp_vendor_data_read(VD_SIZE, 1, buf), -EINVAL);
	zassert_equal(fp_vendor_data_read(VD_SIZE - 1, 2, buf), -EINVAL);
	zassert_equal(fp_vendor_data_read(1, VD_SIZE, buf), -EINVAL);
	/* offset + size wraps around to a small value. */
	zassert_equal(fp_vendor_data_read(UINT32_MAX, 2, buf), -EINVAL);
	zassert_equal(fp_vendor_data_read(2, SIZE_MAX, buf), -EINVAL);
}

ZTEST(fp_vendor_data, test_write_and_erase_reject_out_of_bounds)
{
	uint8_t buf[4] = { 0 };

	zassert_equal(fp_vendor_data_write(VD_SIZE, 1, buf), -EINVAL);
	zassert_equal(fp_vendor_data_write(UINT32_MAX, 2, buf), -EINVAL);
	zassert_equal(fp_vendor_data_erase(VD_SIZE, 1), -EINVAL);
	zassert_equal(fp_vendor_data_erase(UINT32_MAX, 2), -EINVAL);
}

ZTEST(fp_vendor_data, test_read_and_write_reject_null_buffer)
{
	zassert_equal(fp_vendor_data_read(0, 1, NULL), -EINVAL);
	zassert_equal(fp_vendor_data_write(0, 1, NULL), -EINVAL);
	/* Even a zero-length transfer must not accept NULL. */
	zassert_equal(fp_vendor_data_read(0, 0, NULL), -EINVAL);
	zassert_equal(fp_vendor_data_write(0, 0, NULL), -EINVAL);
}

ZTEST(fp_vendor_data, test_zero_size_at_end_is_ok)
{
	uint8_t buf[1];

	zassert_ok(fp_vendor_data_read(VD_SIZE, 0, buf));
	zassert_ok(fp_vendor_data_write(VD_SIZE, 0, buf));
	zassert_ok(fp_vendor_data_erase(VD_SIZE, 0));
	zassert_ok(fp_vendor_data_sync());
}

/* --- Reading ------------------------------------------------------------- */

ZTEST(fp_vendor_data, test_read_returns_flash_content)
{
	uint8_t expected[SECTOR_SIZE];

	for (size_t s = 0; s < SECTORS; s++) {
		fill_pattern(expected, s, 0);
		zassert_ok(fp_vendor_data_read(s * SECTOR_SIZE, SECTOR_SIZE,
					       sector_buf));
		zassert_mem_equal(sector_buf, expected, SECTOR_SIZE,
				  "sector %zu", s);
	}
}

ZTEST(fp_vendor_data, test_read_across_sector_boundary)
{
	uint8_t buf[8];
	uint8_t expected[8];
	uint32_t offset = SECTOR_SIZE - 4;

	for (size_t i = 0; i < 4; i++) {
		expected[i] = pattern(0, SECTOR_SIZE - 4 + i);
		expected[4 + i] = pattern(1, i);
	}

	zassert_ok(fp_vendor_data_read(offset, sizeof(buf), buf));
	zassert_mem_equal(buf, expected, sizeof(buf));
}

ZTEST(fp_vendor_data, test_read_last_bytes_of_region)
{
	uint8_t buf[4];
	uint8_t expected[4];

	for (size_t i = 0; i < 4; i++) {
		expected[i] = pattern(SECTORS - 1, SECTOR_SIZE - 4 + i);
	}

	zassert_ok(fp_vendor_data_read(VD_SIZE - 4, 4, buf));
	zassert_mem_equal(buf, expected, sizeof(buf));
}

/* --- Write-back cache semantics ----------------------------------------- */

ZTEST(fp_vendor_data, test_write_is_visible_before_sync)
{
	const uint8_t data[] = { 0xde, 0xad, 0xbe, 0xef };
	uint8_t buf[sizeof(data)];

	zassert_ok(fp_vendor_data_write(100, sizeof(data), data));
	zassert_ok(fp_vendor_data_read(100, sizeof(buf), buf));
	zassert_mem_equal(buf, data, sizeof(data));
}

/*
 * A modified sector must keep its pending modification across subsequent
 * reads of the same and other sectors.
 */
ZTEST(fp_vendor_data, test_modification_survives_subsequent_reads)
{
	const uint8_t data[] = { 0x11, 0x22, 0x33, 0x44 };
	uint8_t expected[SECTOR_SIZE];

	zassert_ok(fp_vendor_data_write(100, sizeof(data), data));

	/* Touch other parts of the same sector and other sectors. */
	zassert_ok(fp_vendor_data_read(0, 16, sector_buf));
	zassert_ok(fp_vendor_data_read(SECTOR_SIZE, 16, sector_buf));
	zassert_ok(fp_vendor_data_read(0, SECTOR_SIZE, sector_buf));

	fill_pattern(expected, 0, 0);
	memcpy(expected + 100, data, sizeof(data));
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);
}

ZTEST(fp_vendor_data, test_write_not_persisted_until_sync)
{
	const uint8_t data[] = { 0xde, 0xad, 0xbe, 0xef };
	uint8_t expected[SECTOR_SIZE];

	zassert_ok(fp_vendor_data_write(100, sizeof(data), data));

	/* Flash still has the original content. */
	assert_sector_in_flash_matches(0, 0);

	zassert_ok(fp_vendor_data_sync());

	/* Now flash has the new content, rest of the sector preserved. */
	fill_pattern(expected, 0, 0);
	memcpy(expected + 100, data, sizeof(data));
	flash_fetch_sector(0, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);

	/* And reading back after reloading from flash gives the same. */
	zassert_ok(fp_vendor_data_init());
	zassert_ok(fp_vendor_data_read(0, SECTOR_SIZE, sector_buf));
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);
}

ZTEST(fp_vendor_data, test_write_across_sector_boundary_persists_both)
{
	uint8_t data[8];
	uint8_t expected[SECTOR_SIZE];
	uint32_t offset = SECTOR_SIZE - 4;

	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = 0xa0 + i;
	}

	zassert_ok(fp_vendor_data_write(offset, sizeof(data), data));
	zassert_ok(fp_vendor_data_sync());

	fill_pattern(expected, 0, 0);
	memcpy(expected + SECTOR_SIZE - 4, data, 4);
	flash_fetch_sector(0, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE, "sector 0");

	fill_pattern(expected, 1, 0);
	memcpy(expected, data + 4, 4);
	flash_fetch_sector(1, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE, "sector 1");

	/* Untouched neighbour. */
	assert_sector_in_flash_matches(2, 0);
}

ZTEST(fp_vendor_data, test_write_at_end_of_region_persists)
{
	const uint8_t data[] = { 1, 2, 3, 4 };
	uint8_t expected[SECTOR_SIZE];

	zassert_ok(fp_vendor_data_write(VD_SIZE - sizeof(data), sizeof(data),
					data));
	zassert_ok(fp_vendor_data_sync());

	fill_pattern(expected, SECTORS - 1, 0);
	memcpy(expected + SECTOR_SIZE - sizeof(data), data, sizeof(data));
	flash_fetch_sector(SECTORS - 1, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);
}

/*
 * A write covering a whole sector plus the edges of its neighbours: the
 * neighbours keep their untouched bytes and all three sectors are written
 * back correctly.
 */
ZTEST(fp_vendor_data, test_write_spanning_partial_and_full_sectors)
{
	static uint8_t data[SECTOR_SIZE + 8];
	uint8_t expected[SECTOR_SIZE];
	uint32_t offset = SECTOR_SIZE - 4;

	for (size_t i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)(0x80 + i);
	}

	zassert_ok(fp_vendor_data_write(offset, sizeof(data), data));
	zassert_ok(fp_vendor_data_sync());

	/* Sector 0: original content, last 4 bytes replaced. */
	fill_pattern(expected, 0, 0);
	memcpy(expected + SECTOR_SIZE - 4, data, 4);
	flash_fetch_sector(0, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE, "sector 0");

	/* Sector 1: entirely new content. */
	flash_fetch_sector(1, sector_buf);
	zassert_mem_equal(sector_buf, data + 4, SECTOR_SIZE, "sector 1");

	/* Sector 2: first 4 bytes replaced, rest preserved. */
	fill_pattern(expected, 2, 0);
	memcpy(expected, data + 4 + SECTOR_SIZE, 4);
	flash_fetch_sector(2, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE, "sector 2");

	/* Untouched neighbour. */
	assert_sector_in_flash_matches(3, 0);
}

ZTEST(fp_vendor_data, test_zero_size_write_and_erase_do_not_modify)
{
	uint8_t buf[1] = { 0 };

	zassert_ok(fp_vendor_data_write(10, 0, buf));
	zassert_ok(fp_vendor_data_erase(10, 0));
	zassert_ok(fp_vendor_data_read(10, 0, buf));

	/* Nothing to write back, buffer and flash unchanged. */
	zassert_false(fp_vendor_data_pending());
	zassert_ok(fp_vendor_data_read(0, SECTOR_SIZE, sector_buf));
	zassert_equal(sector_buf[10], pattern(0, 10));
	zassert_ok(fp_vendor_data_sync());
	assert_sector_in_flash_matches(0, 0);
}

/*
 * Sync must only erase/program sectors that were actually modified. To
 * detect an unwanted rewrite, change unmodified sectors in flash behind the
 * cache's back; if sync touches them, the stale cached copy would overwrite
 * our out-of-band change.
 */
ZTEST(fp_vendor_data, test_sync_only_touches_modified_sectors)
{
	const uint8_t data[] = { 0x55 };

	/* Change sectors 0 and 2 in flash directly. */
	flash_fill_sector(0, 0xff);
	flash_fill_sector(2, 0xff);

	/* Modify sector 1 only and sync. */
	zassert_ok(fp_vendor_data_write(SECTOR_SIZE + 10, sizeof(data), data));
	zassert_ok(fp_vendor_data_sync());

	/* Out-of-band changes must still be in flash. */
	assert_sector_in_flash_matches(0, 0xff);
	assert_sector_in_flash_matches(2, 0xff);

	/* Sector 1 was written. */
	flash_fetch_sector(1, sector_buf);
	zassert_equal(sector_buf[10], data[0]);
}

/*
 * Regression: a write that ends exactly on a sector boundary must not mark
 * the following sector as modified.
 */
ZTEST(fp_vendor_data, test_sector_aligned_write_does_not_spill)
{
	uint8_t data[SECTOR_SIZE];

	/* Change sector 1 in flash behind the cache's back. */
	flash_fill_sector(1, 0xff);

	/* Write exactly sector 0: [0, SECTOR_SIZE). */
	memset(data, 0x42, sizeof(data));
	zassert_ok(fp_vendor_data_write(0, SECTOR_SIZE, data));
	zassert_ok(fp_vendor_data_sync());

	flash_fetch_sector(0, sector_buf);
	zassert_mem_equal(sector_buf, data, SECTOR_SIZE, "sector 0");
	assert_sector_in_flash_matches(1, 0xff);
}

ZTEST(fp_vendor_data, test_sync_is_idempotent)
{
	const uint8_t data[] = { 0x77 };

	zassert_ok(fp_vendor_data_write(5, sizeof(data), data));
	zassert_ok(fp_vendor_data_sync());

	/* Nothing pending: a second sync must not rewrite anything. */
	flash_fill_sector(0, 0xff);
	zassert_ok(fp_vendor_data_sync());
	assert_sector_in_flash_matches(0, 0xff);
}

/* --- Erase --------------------------------------------------------------- */

ZTEST(fp_vendor_data, test_erase_fills_with_erase_value)
{
	uint8_t expected[SECTOR_SIZE];

	zassert_ok(fp_vendor_data_erase(16, 32));

	fill_pattern(expected, 0, 0);
	memset(expected + 16, erase_value, 32);

	/* Visible through the cache before sync... */
	zassert_ok(fp_vendor_data_read(0, SECTOR_SIZE, sector_buf));
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);

	/* ...not yet in flash... */
	assert_sector_in_flash_matches(0, 0);

	/* ...and in flash after sync. */
	zassert_ok(fp_vendor_data_sync());
	flash_fetch_sector(0, sector_buf);
	zassert_mem_equal(sector_buf, expected, SECTOR_SIZE);
	assert_sector_in_flash_matches(1, 0);
}

ZTEST(fp_vendor_data, test_erase_whole_region)
{
	uint8_t expected[SECTOR_SIZE];

	memset(expected, erase_value, sizeof(expected));

	zassert_ok(fp_vendor_data_erase(0, VD_SIZE));
	zassert_ok(fp_vendor_data_sync());

	for (size_t s = 0; s < SECTORS; s++) {
		flash_fetch_sector(s, sector_buf);
		zassert_mem_equal(sector_buf, expected, SECTOR_SIZE,
				  "sector %zu", s);
	}
}
