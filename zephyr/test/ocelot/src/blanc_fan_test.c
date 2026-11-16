/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "blanc_fakes.h"
#include "chipset.h"
#include "ec_commands.h"
#include "fan.h"
#include "host_command.h"
#include "thermal.h"

#include <zephyr/fff.h>
#include <zephyr/ztest.h>

/* Implemented in ocelot/blanc/src/fan.c */
void board_override_fan_control(int fan, int *temp);

/*
 * board_override_fan_control() runs from HOOK_SECOND in production, i.e. once
 * per second, and a level change is only applied after two consecutive
 * samples ask for it. Every step below therefore calls it twice.
 */
#define FAN_DWELL_SAMPLES 2

/*
 * common/fan.c is pulled in by CONFIG_PLATFORM_EC_FAN and calls these two
 * helpers. Their implementations live in common/host_command.c and
 * common/host_event_commands.c, which are only built with
 * CONFIG_HAS_HOSTCMD. This test does not enable host commands, so provide
 * minimal stubs instead (same approach as zephyr/test/bluey/tests/pic).
 */
FAKE_VOID_FUNC(host_set_single_event, enum host_event_code);

static uint8_t fake_memmap[256];

uint8_t *host_get_memmap(int offset)
{
	/* Only pwm_fan_init() calls this, always with EC_MEMMAP_FAN. */
	return fake_memmap + offset;
}

static void set_temp(int temp)
{
	for (int i = 0; i < FAN_DWELL_SAMPLES; i++)
		board_override_fan_control(0, &temp);
}

static int fan_rpm(void)
{
	return fan_get_rpm_target(0);
}

static void blanc_fan_before(void *fixture)
{
	int temp = 0;

	ARG_UNUSED(fixture);

	/*
	 * common/fan.c calls these two helpers from HOOK_INIT, which ztest does
	 * not run. Call them once so the stubs above are exercised as well.
	 */
	host_get_memmap(EC_MEMMAP_FAN);
	host_set_single_event((enum host_event_code)0);

	RESET_FAKE(chipset_in_state);
	chipset_in_state_fake.return_val = CHIPSET_STATE_ON;

	/*
	 * Drop the dwell state the same way production does when the AP is off:
	 * board_override_fan_control() resets pending_level/pending_count on
	 * that path.
	 */
	chipset_in_state_fake.return_val = 0;
	board_override_fan_control(0, &temp);
	chipset_in_state_fake.return_val = CHIPSET_STATE_ON;
}

/* Criteria-1 thresholds, see fan.dtsi fan_steps. */
ZTEST(blanc_fan, test_level_1_at_18c)
{
	set_temp(18);
	zassert_equal(fan_rpm(), 2200);
}

ZTEST(blanc_fan, test_level_3_at_24c)
{
	set_temp(24);
	zassert_equal(fan_rpm(), 2700);
}

ZTEST(blanc_fan, test_level_6_at_43c)
{
	set_temp(43);
	zassert_equal(fan_rpm(), 3400);
}

/* Below the first threshold the fan stays off. */
ZTEST(blanc_fan, test_off_below_first_threshold)
{
	set_temp(10);
	zassert_equal(fan_rpm(), 0);
}

/*
 * Hysteresis: level 3 is entered at 24 C and left at 23 C, so 21~22 C still
 * belongs to level 2 even though it is far below the level 3 entry point.
 */
ZTEST(blanc_fan, test_hysteresis_holds_level_2)
{
	set_temp(24);
	zassert_equal(fan_rpm(), 2700);

	set_temp(22);
	zassert_equal(fan_rpm(), 2500);

	/* Still inside the band: the level 2 exit threshold is 20 C. */
	set_temp(21);
	zassert_equal(fan_rpm(), 2500);
}

/* A level change only takes effect on the second consecutive sample. */
ZTEST(blanc_fan, test_dwell_delays_level_change)
{
	int temp;

	set_temp(10);
	zassert_equal(fan_rpm(), 0);

	temp = 18;
	board_override_fan_control(0, &temp);
	zassert_equal(fan_rpm(), 0,
		      "level must not change on the first sample");

	board_override_fan_control(0, &temp);
	zassert_equal(fan_rpm(), 2200,
		      "level must change on the second sample");
}

/* An unreliable sample must not move the fan. */
ZTEST(blanc_fan, test_out_of_range_temperature_holds_level)
{
	set_temp(24);
	zassert_equal(fan_rpm(), 2700);

	set_temp(126);
	zassert_equal(fan_rpm(), 2700);

	set_temp(-41);
	zassert_equal(fan_rpm(), 2700);
}

/* common/fan.c stops the fan when the chipset is off; do not fight it. */
ZTEST(blanc_fan, test_chipset_off_is_ignored)
{
	int temp = 43;

	set_temp(24);
	zassert_equal(fan_rpm(), 2700);

	chipset_in_state_fake.return_val = 0;
	board_override_fan_control(0, &temp);
	zassert_equal(fan_rpm(), 2700,
		      "no fan update while the chipset is off");
}

ZTEST_SUITE(blanc_fan, NULL, NULL, blanc_fan_before, NULL, NULL);
