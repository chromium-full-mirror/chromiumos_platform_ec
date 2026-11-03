/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Tests for the deferred write back of fingerprint sensor vendor data.
 *
 * The sensor library modifies vendor data from fp_task (here: from the mocked
 * enroll_step callback). fp_task must write it back to flash only after it
 * has been idle for CONFIG_PLATFORM_EC_FP_VENDOR_DATA_SYNC_DELAY_MS, never
 * while the sensor waits for a finger or an enrollment session is running,
 * and any fp_task activity must restart the countdown.
 */

#include "mock_fingerprint_algorithm.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <drivers/fingerprint.h>
#include <drivers/fingerprint_sim.h>
#include <ec_commands.h>
#include <ec_tasks.h>
#include <emul/emul_flash.h>
#include <fingerprint/fingerprint_alg.h>
#include <fpsensor/fpsensor_state.h>
#include <fpsensor/fpsensor_vendor_data.h>
#include <host_command.h>

FAKE_VALUE_FUNC(int, mkbp_send_event, uint8_t);

#define fp_sim DEVICE_DT_GET(DT_CHOSEN(cros_fp_fingerprint_sensor))
#define IMAGE_SIZE                                                 \
	MAX_FROM_LIST(LISTIFY(NUM_IMAGE_CAPTURE_TYPES,             \
			      FINGERPRINT_SENSOR_FRAME_SIZE, (, ), \
			      DT_CHOSEN(cros_fp_fingerprint_sensor)))
static uint8_t image_buffer[IMAGE_SIZE];

#define VD_ADDR DT_REG_ADDR(DT_NODELABEL(fingerprint_sensor_vendor_data))
#define VD_SIZE DT_REG_SIZE(DT_NODELABEL(fingerprint_sensor_vendor_data))

#define SYNC_DELAY_MS CONFIG_PLATFORM_EC_FP_VENDOR_DATA_SYNC_DELAY_MS
/*
 * Margin for the work queue and fp_task to run after the timer expires, and
 * for the k_msleep(1) calls in the helpers, which round up to the 10 ms
 * tick of native_sim.
 */
#define SYNC_MARGIN_MS 100

static const struct device *const flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

static const uint8_t test_data[] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x02 };
static const uint32_t test_offset = 100;

/* Enroll progress reported by the mocked enroll_step. */
static int enroll_percent;

/*
 * Mocked enroll step that behaves like a sensor library updating its
 * calibration data: it writes to the vendor data cache from fp_task.
 */
static int
enroll_step_writes_vendor_data(const struct fingerprint_algorithm *const alg,
			       const uint8_t *const image, int *percent)
{
	zassert_ok(fp_vendor_data_write(test_offset, sizeof(test_data),
					test_data));
	*percent = enroll_percent;

	return 0;
}

static bool flash_has_test_data(void)
{
	uint8_t buf[sizeof(test_data)];

	zassert_ok(
		flash_read(flash_dev, VD_ADDR + test_offset, buf, sizeof(buf)));

	return memcmp(buf, test_data, sizeof(buf)) == 0;
}

static void set_fp_mode(uint32_t mode)
{
	struct ec_params_fp_mode params = { .mode = mode };
	struct ec_response_fp_mode response;

	zassert_ok(ec_cmd_fp_mode(NULL, &params, &response));

	/* Give opportunity for fpsensor task to change mode. */
	k_msleep(1);
}

static uint32_t get_fp_mode(void)
{
	struct ec_params_fp_mode params = { .mode = FP_MODE_DONT_CHANGE };
	struct ec_response_fp_mode response;

	zassert_ok(ec_cmd_fp_mode(NULL, &params, &response));

	return response.mode;
}

/* Simulate a finger touch processed by fp_task in the current mode. */
static void touch_sensor(void)
{
	struct fingerprint_sensor_state state;

	fingerprint_get_state(fp_sim, &state);
	state.finger_state = FINGERPRINT_FINGER_STATE_PRESENT;
	fingerprint_set_state(fp_sim, &state);

	fingerprint_run_callback(fp_sim);

	/* Give opportunity for fpsensor task to process the event. */
	k_msleep(1);
}

/* Start an enrollment and process one touch that modifies vendor data. */
static void enroll_touch(int percent)
{
	enroll_percent = percent;
	mock_alg_enroll_step_fake.custom_fake = enroll_step_writes_vendor_data;

	set_fp_mode(FP_MODE_ENROLL_SESSION | FP_MODE_ENROLL_IMAGE);
	touch_sensor();

	zassert_equal(mock_alg_enroll_step_fake.call_count, 1);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_sync_happens_after_idle_delay)
{
	/* Final enrollment touch: session ends, data is dirty. */
	enroll_touch(100);
	zassert_false(get_fp_mode() & FP_MODE_ENROLL_SESSION);

	/* Shortly before the delay expires nothing was written. */
	k_msleep(SYNC_DELAY_MS - SYNC_MARGIN_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/* Once fp_task was idle long enough the data lands in flash. */
	k_msleep(2 * SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_activity_restarts_delay)
{
	enroll_touch(100);

	/*
	 * Half way through the delay the host talks to fp_task again
	 * (a mode update that leaves the sensor idle).
	 */
	k_msleep(SYNC_DELAY_MS / 2);
	set_fp_mode(0);

	/* The original deadline passes without a sync... */
	k_msleep(SYNC_DELAY_MS / 2 + SYNC_MARGIN_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/* ...the sync happens one full delay after the last activity. */
	k_msleep(SYNC_DELAY_MS / 2);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_no_sync_while_waiting_for_finger)
{
	enroll_touch(100);

	/* Host arms matching right away, as on the lock screen. */
	set_fp_mode(FP_MODE_MATCH);

	/*
	 * A flash write back could delay the finger event, so it must not
	 * happen while the sensor waits for a finger.
	 */
	k_msleep(2 * SYNC_DELAY_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/* Re-arm the timer by going idle, then check finger down detection. */
	set_fp_mode(0);
	set_fp_mode(FP_MODE_FINGER_DOWN);
	k_msleep(2 * SYNC_DELAY_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/* Once the sensor is idle again, the data is written back. */
	set_fp_mode(0);
	k_msleep(SYNC_DELAY_MS - SYNC_MARGIN_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	k_msleep(2 * SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_sync_allowed_in_deepsleep)
{
	enroll_touch(100);

	/* Deep sleep means no finger is expected; syncing is fine. */
	set_fp_mode(FP_MODE_DEEPSLEEP);

	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_no_sync_during_enroll_session)
{
	/* Intermediate enrollment touch: session is still running. */
	enroll_touch(50);
	zassert_true(get_fp_mode() & FP_MODE_ENROLL_SESSION);

	/* No write back while enrolling, no matter how long we wait. */
	k_msleep(2 * SYNC_DELAY_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/*
	 * Final touch ends the session and arms the write back. Like the
	 * host, request another capture first: fp_task clears
	 * FP_MODE_ENROLL_IMAGE after every processed touch.
	 */
	enroll_percent = 100;
	set_fp_mode(FP_MODE_ENROLL_SESSION | FP_MODE_ENROLL_IMAGE);
	touch_sensor();
	zassert_equal(mock_alg_enroll_step_fake.call_count, 2);
	zassert_false(get_fp_mode() & FP_MODE_ENROLL_SESSION);

	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_sync_after_enroll_cancelled)
{
	enroll_touch(50);

	/* Host cancels the enrollment. */
	set_fp_mode(0);
	zassert_false(get_fp_mode() & FP_MODE_ENROLL_SESSION);
	zassert_equal(mock_alg_enroll_finish_fake.call_count, 1);

	/* Whatever the library left in the cache is written back. */
	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_sync_failure_keeps_data_and_retries)
{
	enroll_touch(100);

	/* Make the flash refuse writes. */
	cros_flash_emul_enable_protect();

	/* The sync attempt fails and the data stays dirty... */
	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);
	zassert_true(fp_vendor_data_pending());
	zassert_false(flash_has_test_data());

	/* ...without retrying by itself. */
	k_msleep(2 * SYNC_DELAY_MS);
	zassert_true(fp_vendor_data_pending());

	/* The next fp_task activity re-arms the write back. */
	cros_flash_emul_protect_reset();
	set_fp_mode(0);

	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);
	zassert_false(fp_vendor_data_pending());
	zassert_true(flash_has_test_data());
}

ZTEST_USER(fpsensor_vendor_data, test_nothing_to_sync_leaves_flash_alone)
{
	uint8_t before[16];
	uint8_t after[16];

	zassert_ok(flash_read(flash_dev, VD_ADDR, before, sizeof(before)));

	/* fp_task activity without vendor data modifications. */
	set_fp_mode(FP_MODE_FINGER_DOWN);
	set_fp_mode(0);
	k_msleep(SYNC_DELAY_MS + SYNC_MARGIN_MS);

	zassert_false(fp_vendor_data_pending());
	zassert_ok(flash_read(flash_dev, VD_ADDR, after, sizeof(after)));
	zassert_mem_equal(before, after, sizeof(before));
}

static void *fpsensor_setup(void)
{
	zassert_true(device_is_ready(flash_dev));

	/* Start shimmed tasks. */
	start_ec_tasks();
	k_msleep(100);

	return NULL;
}

static void fpsensor_before(void *f)
{
	struct fingerprint_sensor_state state = {
		.bad_pixels = 0,
		.maintenance_ran = false,
		.detect_mode = false,
		.low_power_mode = false,
		.finger_state = FINGERPRINT_FINGER_STATE_NONE,
		.init_result = 0,
		.deinit_result = 0,
		.config_result = 0,
		.get_info_result = 0,
		.acquire_image_result = FINGERPRINT_SENSOR_SCAN_GOOD,
		.last_acquire_image_mode = -1,
	};
	uint32_t fp_events;

	ARG_UNUSED(f);

	/* Reset global context to a clean state. */
	fp_reset_and_clear_context();
	set_fp_mode(0);

	fingerprint_set_state(fp_sim, &state);
	memset(image_buffer, 1, IMAGE_SIZE);
	fingerprint_load_image(fp_sim, image_buffer, IMAGE_SIZE);

	RESET_FAKE(mkbp_send_event);
	RESET_FAKE(mock_alg_init);
	RESET_FAKE(mock_alg_exit);
	RESET_FAKE(mock_alg_enroll_start);
	RESET_FAKE(mock_alg_enroll_step);
	RESET_FAKE(mock_alg_enroll_finish);
	RESET_FAKE(mock_alg_match);

	enroll_percent = 0;

	/* Clear MKBP events from previous tests. */
	fp_get_next_event((uint8_t *)&fp_events);

	/* Start from a flash region that doesn't contain the test data. */
	cros_flash_emul_protect_reset();
	zassert_ok(flash_erase(flash_dev, VD_ADDR, VD_SIZE));
	zassert_ok(fp_vendor_data_init());
	zassert_false(flash_has_test_data());
}

ZTEST_SUITE(fpsensor_vendor_data, NULL, fpsensor_setup, fpsensor_before, NULL,
	    NULL);
