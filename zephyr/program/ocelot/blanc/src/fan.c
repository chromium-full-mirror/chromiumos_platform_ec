/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * blanc custom fan control (CONFIG_PLATFORM_EC_CUSTOM_FAN_CONTROL).
 *
 * The fan curve lives in fan.dtsi as the `fan_steps` node so that the thermal
 * team can update it without touching C code.  This file only implements the
 * level walk: hysteresis via temp-on/temp-off, plus the "toggle lasting time"
 * the table requires.
 *
 * thermal_control() calls board_override_fan_control() from HOOK_SECOND, i.e.
 * exactly once per second, so a "2 s lasting time" is two consecutive samples
 * asking for the same level - no timer is needed.
 */

#include "chipset.h"
#include "common.h"
#include "console.h"
#include "fan.h"
#include "temp_sensor/temp_sensor.h"
#include "thermal.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_THERMAL, format, ##args)

/* Criteria-1 uses the CPU sensor only; other sensors stay at 0xff in DTS. */
#define TEMP_CPU TEMP_SENSOR_ID(DT_NODELABEL(temp_sensor_cpu))

/* A new level must be requested this many consecutive times before applying. */
#define FAN_DWELL_COUNT 2

/* Ignore samples outside this range (sensor read failure leaves garbage). */
#define TEMP_MIN_C (-40)
#define TEMP_MAX_C 125

struct fan_step {
	int on[TEMP_SENSOR_COUNT];
	int off[TEMP_SENSOR_COUNT];
	uint16_t rpm[FAN_CH_COUNT];
};

#define FAN_TABLE_ENTRY(nd)                     \
	{                                       \
		.on = DT_PROP(nd, temp_on),     \
		.off = DT_PROP(nd, temp_off),   \
		.rpm = DT_PROP(nd, rpm_target), \
	},

static const struct fan_step fan_step_table[] = { DT_FOREACH_CHILD(
	DT_NODELABEL(fan_steps), FAN_TABLE_ENTRY) };

#define NUM_FAN_LEVELS ARRAY_SIZE(fan_step_table)

static int current_level;
static int pending_level = -1;
static int pending_count;

static int level_after(int level, int temp)
{
	int want = level;

	/*
	 * Jump straight to the level the temperature asks for instead of
	 * walking one level per second: the table is a threshold table, so at
	 * 24 C the fan belongs at 2700 rpm immediately, not 6 s later.
	 *
	 * Rising: the highest level whose temp-on is satisfied.
	 * Falling: the lowest level whose temp-off is satisfied.
	 * Inside the hysteresis band neither loop moves, so the level is kept.
	 */
	while (want < NUM_FAN_LEVELS - 1 &&
	       temp >= fan_step_table[want].on[TEMP_CPU])
		want++;

	while (want > 0 && temp <= fan_step_table[want].off[TEMP_CPU])
		want--;

	return want;
}

static int fan_table_to_rpm(int fan, int *temp)
{
	int t = temp[TEMP_CPU];
	int want;

	if (t < TEMP_MIN_C || t > TEMP_MAX_C) {
		/* Unreliable sample: hold the current level. */
		return fan_step_table[current_level].rpm[fan];
	}

	want = level_after(current_level, t);

	if (want != pending_level) {
		pending_level = want;
		pending_count = 1;
	} else if (++pending_count >= FAN_DWELL_COUNT) {
		if (want != current_level) {
			CPRINTS("fan level %d -> %d (%d C)", current_level,
				want, t);
			current_level = want;
		}
		pending_count = 0;
	}

	return fan_step_table[current_level].rpm[fan];
}

void board_override_fan_control(int fan, int *temp)
{
	/*
	 * In common/fan.c pwm_fan_stop() will turn off fan
	 * when chipset suspend or shutdown.
	 */
	if (!chipset_in_state(CHIPSET_STATE_ON)) {
		/* Drop the dwell counter so resume does not use stale state. */
		pending_level = -1;
		pending_count = 0;
		return;
	}

	fan_set_rpm_mode(fan, 1);
	fan_set_rpm_target(fan, fan_table_to_rpm(fan, temp));
}
