/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "fan.h"
#include "hooks.h"
#include "host_command.h"
#include "temp_sensor.h"
#include "temp_sensor/temp_sensor.h"
#include "thermal.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#define TEST_TEMP0 TEMP_SENSOR_ID(DT_NODELABEL(test_temp0))
#define TEST_TEMP1 TEMP_SENSOR_ID(DT_NODELABEL(test_temp1))
#define TEST_FAN_TEMPERATURE_WINDOW_SAMPLES 11

FAKE_VOID_FUNC(fan_set_percent_needed, int, int);
FAKE_VOID_FUNC(fan_set_rpm_mode, int, int);
FAKE_VOID_FUNC(fan_set_rpm_target, int, int);
FAKE_VALUE_FUNC(int, fan_get_rpm_target, int);

static uint8_t fake_memmap[256];

uint8_t *host_get_memmap(int offset)
{
	if (offset < 0 || offset >= (int)sizeof(fake_memmap))
		return NULL;

	return fake_memmap + offset;
}

void host_set_single_event(enum host_event_code event)
{
	ARG_UNUSED(event);
}

static void set_adc_value(int sensor, uint16_t value)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));

	zassert_ok(
		adc_emul_const_value_set(adc, temp_sensors[sensor].idx, value));
}

static int get_sensor_temperature(int sensor)
{
	int temp;

	zassert_ok(temp_sensor_read(sensor, &temp));
	return temp;
}

static uint16_t find_adc_value_for_temperature(int sensor, int target_temp)
{
	uint16_t value;

	for (value = 1; value < 4096; value++) {
		int temp;

		set_adc_value(sensor, value);
		if (temp_sensor_read(sensor, &temp) == EC_SUCCESS &&
		    temp == target_temp)
			return value;
	}

	zassert_unreachable("No ADC value found for temperature %d K",
			    target_temp);
	return 0;
}

static int adc_read_error(const struct device *dev, unsigned int channel,
			  void *param, uint32_t *result)
{
	return -EINVAL;
}

static void enable_fan_curve(int sensor, int temp_off, int temp_max)
{
	thermal_params[sensor].temp_fan_off = temp_off;
	thermal_params[sensor].temp_fan_max = temp_max;
}

static void fan_before(void *data)
{
	ARG_UNUSED(data);

	memset(thermal_params, 0,
	       sizeof(struct ec_thermal_config) * TEMP_SENSOR_COUNT);
	hook_notify(HOOK_CHIPSET_SUSPEND);
	set_adc_value(TEST_TEMP0, 1000);
	set_adc_value(TEST_TEMP1, 1000);

	RESET_FAKE(fan_set_percent_needed);
	RESET_FAKE(fan_set_rpm_mode);
	RESET_FAKE(fan_set_rpm_target);
	RESET_FAKE(fan_get_rpm_target);
}

ZTEST_SUITE(fatcat_ruby_fan, NULL, NULL, fan_before, NULL, NULL);

ZTEST(fatcat_ruby_fan, test_first_sample_applies_fan_curve)
{
	int temp = get_sensor_temperature(TEST_TEMP0);

	enable_fan_curve(TEST_TEMP0, temp, temp + 1);
	set_adc_value(TEST_TEMP0,
		      find_adc_value_for_temperature(TEST_TEMP0, temp + 1));

	board_override_fan_control(0, NULL);

	zassert_equal(fan_set_percent_needed_fake.call_count, 1);
	zassert_equal(fan_set_percent_needed_fake.arg0_val, 0);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);
}

ZTEST(fatcat_ruby_fan, test_average_rounding_is_symmetric)
{
	int temp = get_sensor_temperature(TEST_TEMP0);
	uint16_t base_value = 1000;
	uint16_t higher_value =
		find_adc_value_for_temperature(TEST_TEMP0, temp + 1);
	int i;

	enable_fan_curve(TEST_TEMP0, temp, temp + 1);
	set_adc_value(TEST_TEMP0, base_value);

	for (i = 0; i < TEST_FAN_TEMPERATURE_WINDOW_SAMPLES; i++)
		board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);

	set_adc_value(TEST_TEMP0, higher_value);
	for (i = 0; i < 5; i++)
		board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);

	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	set_adc_value(TEST_TEMP0, base_value);
	for (i = 0; i < 5; i++)
		board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);
	zassert_equal(fan_set_percent_needed_fake.call_count, 23);
}

ZTEST(fatcat_ruby_fan, test_chipset_suspend_and_shutdown_reset_average)
{
	int temp = get_sensor_temperature(TEST_TEMP0);
	uint16_t higher_value =
		find_adc_value_for_temperature(TEST_TEMP0, temp + 1);

	enable_fan_curve(TEST_TEMP0, temp, temp + 1);
	set_adc_value(TEST_TEMP0, higher_value);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	hook_notify(HOOK_CHIPSET_SUSPEND);
	set_adc_value(TEST_TEMP0, 1000);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);

	set_adc_value(TEST_TEMP0, higher_value);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	hook_notify(HOOK_CHIPSET_SHUTDOWN);
	set_adc_value(TEST_TEMP0, 1000);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);
}

ZTEST(fatcat_ruby_fan, test_sensor_read_failure_resets_average)
{
	int temp = get_sensor_temperature(TEST_TEMP0);
	uint16_t higher_value =
		find_adc_value_for_temperature(TEST_TEMP0, temp + 1);
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));

	enable_fan_curve(TEST_TEMP0, temp, temp + 1);
	set_adc_value(TEST_TEMP0, higher_value);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	zassert_ok(adc_emul_value_func_set(adc, temp_sensors[TEST_TEMP0].idx,
					   adc_read_error, NULL));
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.call_count, 1);

	set_adc_value(TEST_TEMP0, 1000);
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 0);
}

ZTEST(fatcat_ruby_fan, test_newly_configured_sensor_starts_fresh_average)
{
	int temp0 = get_sensor_temperature(TEST_TEMP0);
	int temp1 = get_sensor_temperature(TEST_TEMP1);
	uint16_t higher_value =
		find_adc_value_for_temperature(TEST_TEMP1, temp1 + 1);
	int i;

	enable_fan_curve(TEST_TEMP0, temp0, temp0 + 1);
	for (i = 0; i < TEST_FAN_TEMPERATURE_WINDOW_SAMPLES; i++)
		board_override_fan_control(0, NULL);

	enable_fan_curve(TEST_TEMP1, temp1, temp1 + 1);
	set_adc_value(TEST_TEMP1, higher_value);
	board_override_fan_control(0, NULL);

	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);
}

ZTEST(fatcat_ruby_fan, test_sensor_fail_over_30s_sets_rpm_when_fan_stopped)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
	int i;

	enable_fan_curve(TEST_TEMP0, 300, 320);
	fan_get_rpm_target_fake.return_val = 0;

	zassert_ok(adc_emul_value_func_set(adc, temp_sensors[TEST_TEMP0].idx,
					   adc_read_error, NULL));

	/* First 30 seconds of failure: should not trigger fail-safe yet */
	for (i = 0; i < 30; i++)
		board_override_fan_control(0, NULL);

	zassert_equal(fan_set_rpm_target_fake.call_count, 0);

	/* 31st second (> 30 seconds): fail-safe triggers, set RPM to 3000 */
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_rpm_target_fake.call_count, 1);
	zassert_equal(fan_set_rpm_target_fake.arg0_val, 0);
	zassert_equal(fan_set_rpm_target_fake.arg1_val, 3000);
	zassert_equal(fan_set_rpm_mode_fake.call_count, 1);
	zassert_equal(fan_set_rpm_mode_fake.arg1_val, 1);

	/* Continuing failure: fan is now at 3000 RPM, keeps current RPM */
	fan_get_rpm_target_fake.return_val = 3000;
	board_override_fan_control(0, NULL);
	zassert_equal(fan_set_rpm_target_fake.call_count, 1);
}

ZTEST(fatcat_ruby_fan, test_sensor_fail_over_30s_keeps_rotating_fan_rpm)
{
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
	int i;

	enable_fan_curve(TEST_TEMP0, 300, 320);
	/* Fan already rotating at 2500 RPM */
	fan_get_rpm_target_fake.return_val = 2500;

	zassert_ok(adc_emul_value_func_set(adc, temp_sensors[TEST_TEMP0].idx,
					   adc_read_error, NULL));

	/* 35 seconds of sensor failure */
	for (i = 0; i < 35; i++)
		board_override_fan_control(0, NULL);

	/* Fail-safe should NOT overwrite rpm to 3000; keeps current RPM */
	zassert_equal(fan_set_rpm_target_fake.call_count, 0);
}

ZTEST(fatcat_ruby_fan, test_sensor_recovery_resumes_normal_fan_control)
{
	int temp = get_sensor_temperature(TEST_TEMP0);
	uint16_t higher_value =
		find_adc_value_for_temperature(TEST_TEMP0, temp + 1);
	const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc0));
	int i;

	enable_fan_curve(TEST_TEMP0, temp, temp + 1);
	fan_get_rpm_target_fake.return_val = 0;

	zassert_ok(adc_emul_value_func_set(adc, temp_sensors[TEST_TEMP0].idx,
					   adc_read_error, NULL));

	/* Trigger fail-safe after > 30 seconds */
	for (i = 0; i < 31; i++)
		board_override_fan_control(0, NULL);

	zassert_equal(fan_set_rpm_target_fake.call_count, 1);
	zassert_equal(fan_set_rpm_target_fake.arg1_val, 3000);

	/* Sensor returns to normal with higher temp (100% cooling) */
	set_adc_value(TEST_TEMP0, higher_value);
	board_override_fan_control(0, NULL);

	/* Normal control resumed */
	zassert_equal(fan_set_percent_needed_fake.call_count, 1);
	zassert_equal(fan_set_percent_needed_fake.arg1_val, 100);

	/* Now if sensor fails again, counter should start from 0 */
	zassert_ok(adc_emul_value_func_set(adc, temp_sensors[TEST_TEMP0].idx,
					   adc_read_error, NULL));
	for (i = 0; i < 10; i++)
		board_override_fan_control(0, NULL);

	/* Still only 1 call from earlier, because 10s <= 30s */
	zassert_equal(fan_set_rpm_target_fake.call_count, 1);
}
