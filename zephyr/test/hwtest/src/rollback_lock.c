/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "flash.h"
#include "mpu.h"

#include <zephyr/fff.h>
#include <zephyr/ztest.h>

/*
 * Forward declarations for static functions exported for testing via
 * test_export_static.
 */
uint32_t unlock_rollback(void);
void lock_rollback(uint32_t key);

FAKE_VOID_FUNC(crec_flash_lock_mapped_storage, int);
FAKE_VALUE_FUNC(int, mpu_lock_rollback, bool);

static void rollback_lock_before(void *fixture)
{
	ARG_UNUSED(fixture);
	RESET_FAKE(crec_flash_lock_mapped_storage);
	RESET_FAKE(mpu_lock_rollback);
	FFF_RESET_HISTORY();
}

ZTEST_SUITE(rollback_lock, NULL, NULL, rollback_lock_before, NULL, NULL);

ZTEST(rollback_lock, test_lock_matching)
{
	uint32_t key = unlock_rollback();
	unsigned int unlock_flash_calls =
		crec_flash_lock_mapped_storage_fake.call_count;
	unsigned int unlock_mpu_calls = mpu_lock_rollback_fake.call_count;

	lock_rollback(key);

	zassert_equal(unlock_flash_calls, 1,
		      "Expected 1 mapped storage lock transition on unlock");
	zassert_equal(unlock_mpu_calls, 1,
		      "Expected 1 MPU lock transition on unlock");
	zassert_equal(crec_flash_lock_mapped_storage_fake.call_count, 2,
		      "Expected 2 mapped storage lock transitions after lock");
	zassert_equal(crec_flash_lock_mapped_storage_fake.arg0_history[0], true,
		      "First transition should be an acquire (true)");
	zassert_equal(crec_flash_lock_mapped_storage_fake.arg0_history[1],
		      false, "Second transition should be a release (false)");
	zassert_equal(mpu_lock_rollback_fake.call_count, 2,
		      "Expected 2 MPU lock transitions after lock");
	zassert_equal(mpu_lock_rollback_fake.arg0_history[0], false,
		      "First MPU transition should be unlock (false)");
	zassert_equal(mpu_lock_rollback_fake.arg0_history[1], true,
		      "Second MPU transition should be lock (true)");
	zassert_equal(fff.call_history[0],
		      (void *)crec_flash_lock_mapped_storage,
		      "Expected flash lock acquire before MPU unlock");
	zassert_equal(fff.call_history[1], (void *)mpu_lock_rollback,
		      "Expected MPU unlock second");
	zassert_equal(fff.call_history[2], (void *)mpu_lock_rollback,
		      "Expected MPU lock third");
	zassert_equal(fff.call_history[3],
		      (void *)crec_flash_lock_mapped_storage,
		      "Expected flash lock release after MPU lock");
}
