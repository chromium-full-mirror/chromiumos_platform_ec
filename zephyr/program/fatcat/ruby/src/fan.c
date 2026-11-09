/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "fan.h"
#include "hooks.h"
#include "temp_sensor/temp_sensor.h"
#include "thermal.h"
#include "util.h"

#define FAN_TEMPERATURE_WINDOW_SAMPLES 11
#define FAN_SENSOR_FAIL_TIMEOUT_SEC 30
#define FAN_FAILSAFE_RPM 3000

/* 11 one-second samples span a 10-second window, including both endpoints. */
static int temperature_samples[TEMP_SENSOR_COUNT]
			      [FAN_TEMPERATURE_WINDOW_SAMPLES];
static int temperature_sum[TEMP_SENSOR_COUNT];
static bool fan_sensor_configured[TEMP_SENSOR_COUNT];
static bool fan_sensor_configuration_initialized;
static int sample_count;
static int next_sample_index;
static int sensor_fail_seconds;

static void reset_fan_temperature_average(void)
{
	memset(temperature_samples, 0, sizeof(temperature_samples));
	memset(temperature_sum, 0, sizeof(temperature_sum));
	sample_count = 0;
	next_sample_index = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, reset_fan_temperature_average,
	     HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, reset_fan_temperature_average,
	     HOOK_PRIO_DEFAULT);

static void reset_fan_fail_count(void)
{
	sensor_fail_seconds = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, reset_fan_fail_count, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, reset_fan_fail_count, HOOK_PRIO_DEFAULT);

static void update_fan_sensor_configuration(void)
{
	bool configuration_changed = false;
	int i;

	for (i = 0; i < TEMP_SENSOR_COUNT; i++) {
		bool configured = thermal_params[i].temp_fan_off &&
				  thermal_params[i].temp_fan_max;

		if (fan_sensor_configuration_initialized &&
		    configured != fan_sensor_configured[i])
			configuration_changed = true;

		fan_sensor_configured[i] = configured;
	}

	fan_sensor_configuration_initialized = true;
	if (configuration_changed)
		reset_fan_temperature_average();
}

/*
 * Keep the default thermal fan curve: use the greatest requested cooling
 * percentage among all sensors configured with temp-fan-off and
 * temp-fan-max in thermal.dtsi.
 */
static int get_fan_percent(const int *temp)
{
	int fmax = 0;
	int i;

	for (i = 0; i < TEMP_SENSOR_COUNT; i++) {
		int percent;

		if (!thermal_params[i].temp_fan_off ||
		    !thermal_params[i].temp_fan_max)
			continue;

		percent = thermal_fan_percent(thermal_params[i].temp_fan_off,
					      thermal_params[i].temp_fan_max,
					      temp[i]);
		if (percent > fmax)
			fmax = percent;
	}

	return fmax;
}

/*
 * board_override_fan_control() receives only temperatures, not the read
 * status for each sensor. Read the sensors used by the Ruby fan curve again
 * so a failed read cannot enter the moving-average window.
 */
static int read_fan_temperatures(int *temp)
{
	int i;

	update_fan_sensor_configuration();

	for (i = 0; i < TEMP_SENSOR_COUNT; i++) {
		int temp_k;

		if (!thermal_params[i].temp_fan_off ||
		    !thermal_params[i].temp_fan_max)
			continue;

		if (temp_sensor_read(i, &temp_k) != EC_SUCCESS) {
			reset_fan_temperature_average();
			return -1;
		}

		temp[i] = temp_k;
	}

	return 0;
}

static void average_fan_temperature(const int *temp, int *average_temp)
{
	int i;

	for (i = 0; i < TEMP_SENSOR_COUNT; i++) {
		if (!thermal_params[i].temp_fan_off ||
		    !thermal_params[i].temp_fan_max)
			continue;

		if (sample_count == FAN_TEMPERATURE_WINDOW_SAMPLES) {
			temperature_sum[i] -=
				temperature_samples[i][next_sample_index];
		}

		temperature_samples[i][next_sample_index] = temp[i];
		temperature_sum[i] += temp[i];
	}

	next_sample_index =
		(next_sample_index + 1) % FAN_TEMPERATURE_WINDOW_SAMPLES;

	if (sample_count < FAN_TEMPERATURE_WINDOW_SAMPLES)
		sample_count++;

	for (i = 0; i < TEMP_SENSOR_COUNT; i++) {
		if (!thermal_params[i].temp_fan_off ||
		    !thermal_params[i].temp_fan_max)
			continue;

		average_temp[i] =
			DIV_ROUND_NEAREST(temperature_sum[i], sample_count);
	}
}

void board_override_fan_control(int fan, int *temp)
{
	int current_temp[TEMP_SENSOR_COUNT] = { 0 };
	int average_temp[TEMP_SENSOR_COUNT] = { 0 };
	int percent;

	ARG_UNUSED(temp);

	if (read_fan_temperatures(current_temp) < 0) {
		if (sensor_fail_seconds <= FAN_SENSOR_FAIL_TIMEOUT_SEC)
			sensor_fail_seconds++;

		if (sensor_fail_seconds > FAN_SENSOR_FAIL_TIMEOUT_SEC) {
			if (fan_get_rpm_target(fan) == 0) {
				fan_set_rpm_mode(fan, 1);
				fan_set_rpm_target(fan, FAN_FAILSAFE_RPM);
			}
		}
		return;
	}

	sensor_fail_seconds = 0;

	average_fan_temperature(current_temp, average_temp);
	percent = get_fan_percent(average_temp);
	fan_set_percent_needed(fan, percent);
}
