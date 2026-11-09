/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery_fuel_gauge.h"
#include "charge_state.h"
#include "charger_test.h"

#include <zephyr/ztest.h>

/* Test external variable defined in charge_state_v2 */
extern int charge_prevent_power_on_automatic_power_on;

extern const struct batt_conf_embed *battery_conf;

struct charge_state_prevent_power_on_fixture {
	struct charge_state_data charge_state_backup;
	const struct batt_conf_embed *battery_conf;
	int automatic_power_on;
};

static const struct batt_conf_embed BATT_CONF = {
	.config = {
		.batt_info = {
			.discharging_max_c = 50,
			.discharging_min_c = 5,
		},
	},
};

static const struct batt_conf_embed UPDATED_BATT_CONF = {
	.config = {
		.batt_info = {
			.discharging_max_c = 60,
			.discharging_min_c = 10,
		},
	},
};

static void *setup(void)
{
	static struct charge_state_prevent_power_on_fixture fixture;

	return &fixture;
}

static void before(void *f)
{
	struct charge_state_prevent_power_on_fixture *fixture = f;

	/* Backup the current state */
	fixture->charge_state_backup = *charge_get_status();
	fixture->automatic_power_on =
		charge_prevent_power_on_automatic_power_on;
	fixture->battery_conf = battery_conf;

	/* Reset the automatic_power_on global */
	charge_prevent_power_on_automatic_power_on = 1;

	/*
	 * Set the battery temperature to a comfortable 20C,
	 * it tolerates 5 to 50 degrees.
	 */
	battery_conf = &BATT_CONF;
	charge_get_status()->batt.temperature = 2931;
}

static void after(void *f)
{
	struct charge_state_prevent_power_on_fixture *fixture = f;

	/* Restore the state from 'before' */
	*charge_get_status() = fixture->charge_state_backup;
	charge_prevent_power_on_automatic_power_on =
		fixture->automatic_power_on;
	battery_conf = fixture->battery_conf;
}

ZTEST_SUITE(charge_state_prevent_power_on, charger_predicate_post_main, setup,
	    before, after, NULL);

ZTEST(charge_state_prevent_power_on, test_allow_power_on)
{
	struct batt_params *params = &charge_get_status()->batt;

	/* Force a call to refresh the battery parameters */
	params->is_present = BP_NOT_SURE;
	/* Set the charge state to be high enough */
	params->state_of_charge =
		CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON;

	/* Verify that we can power on when the power button was pressed */
	zassert_false(charge_prevent_power_on(true));
}

ZTEST(charge_state_prevent_power_on, test_low_charge)
{
	struct batt_params *params = &charge_get_status()->batt;

	/* Force a low charge state */
	params->state_of_charge =
		CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON - 1;

	/* Verify that we cannot power on during an automatic power-on */
	zassert_true(charge_prevent_power_on(false));
}

ZTEST(charge_state_prevent_power_on, test_consuming_full_input_current)
{
	struct batt_params *params = &charge_get_status()->batt;

	params->state_of_charge = 50;
	zassert_true(charge_is_consuming_full_input_current());

	params->state_of_charge = 0;
	zassert_false(charge_is_consuming_full_input_current());

	params->state_of_charge = 100;
	zassert_false(charge_is_consuming_full_input_current());
}

ZTEST(charge_state_prevent_power_on, test_extreme_temperature)
{
	struct batt_params *params = &charge_get_status()->batt;

	/* Very hot, not safe to use. */
	params->temperature = 3500;
	/* Automatic and user-requested power-on are both blocked. */
	zassert_true(charge_prevent_power_on(true));
	zassert_true(charge_prevent_power_on(false));

	/* Below freezing, the battery won't operate well. */
	params->temperature = 2700;
	zassert_true(charge_prevent_power_on(true));
	zassert_true(charge_prevent_power_on(false));
}

ZTEST(charge_state_prevent_power_on, test_battery_conf_update)
{
	struct batt_params *params = &charge_get_status()->batt;

	/*
	 * Initial battery_conf (BATT_CONF) has discharging_max_c = 50,
	 * discharging_min_c = 5.
	 * At 55C (3281 dK), the battery is above discharging_max_c.
	 */
	params->temperature = 3281;
	zassert_true(charge_prevent_power_on(true));

	/*
	 * Update battery_conf (modeling battery type retry completion). At 55C,
	 * UPDATED_BATT_CONF tolerates up to 60C, so power-on is allowed.
	 */
	battery_conf = &UPDATED_BATT_CONF;
	zassert_false(charge_prevent_power_on(true));

	/*
	 * At 8C (2811 dK), the battery is below UPDATED_BATT_CONF's minimum
	 * discharging limit of 10C, so power-on is prevented.
	 */
	params->temperature = 2811;
	zassert_true(charge_prevent_power_on(true));
}

ZTEST(charge_state_prevent_power_on, test_no_battery_insufficient_charger)
{
	struct batt_params *params = &charge_get_status()->batt;

	/* Set battery to not connected */
	params->is_present = false;

	/* Remove global power on prevention */
	charge_prevent_power_on_automatic_power_on = 0;

	/* since there is no battery and 0mW power on should be prevented */
	zassert_true(charge_prevent_power_on(true));
	zassert_true(charge_prevent_power_on(false));
}

#ifdef CONFIG_PLATFORM_EC_CHARGER_CUSTOM_PREVENT_POWER_ON
static int custom_prevent_power_on_return_val;

int custom_prevent_power_on(void)
{
	return custom_prevent_power_on_return_val;
}

ZTEST(charge_state_prevent_power_on, test_custom_prevent_power_on_allows)
{
	struct batt_params *params = &charge_get_status()->batt;

	params->is_present = BP_YES;
	params->state_of_charge =
		CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON;

	/* When custom_prevent_power_on returns 0, automatic power-on is allowed
	 */
	custom_prevent_power_on_return_val = 0;
	zassert_false(charge_prevent_power_on(false));
}

ZTEST(charge_state_prevent_power_on, test_custom_prevent_power_on_prevents)
{
	struct batt_params *params = &charge_get_status()->batt;

	params->is_present = BP_YES;
	params->state_of_charge =
		CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON;

	/* When custom_prevent_power_on returns 1, automatic power-on is
	 * prevented */
	custom_prevent_power_on_return_val = 1;
	zassert_true(charge_prevent_power_on(false));
}

ZTEST(charge_state_prevent_power_on,
      test_custom_prevent_power_on_user_pressed_power_button)
{
	struct batt_params *params = &charge_get_status()->batt;

	params->is_present = BP_YES;
	params->state_of_charge =
		CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON;

	/* When power button is pressed, custom_prevent_power_on is not used to
	 * block boot */
	custom_prevent_power_on_return_val = 1;
	zassert_false(charge_prevent_power_on(true));
}
#endif

#ifdef CONFIG_PLATFORM_EC_CHARGER_DISCHARGE_ON_AC_CUSTOM
static int custom_discharge_on_ac_enabled = -1;
static int custom_discharge_on_ac_rv = EC_SUCCESS;

int board_discharge_on_ac(int enable)
{
	custom_discharge_on_ac_enabled = enable;
	return custom_discharge_on_ac_rv;
}

ZTEST(charge_state_prevent_power_on, test_custom_discharge_on_ac)
{
	charge_get_status()->ac = 1;

	custom_discharge_on_ac_rv = EC_SUCCESS;
	zassert_equal(EC_SUCCESS, set_chg_ctrl_mode(CHARGE_CONTROL_DISCHARGE));
	zassert_equal(1, custom_discharge_on_ac_enabled);
	zassert_equal(CHARGE_CONTROL_DISCHARGE, get_chg_ctrl_mode());

	custom_discharge_on_ac_rv = EC_ERROR_UNIMPLEMENTED;
	zassert_equal(EC_ERROR_UNIMPLEMENTED,
		      set_chg_ctrl_mode(CHARGE_CONTROL_NORMAL));
	zassert_equal(0, custom_discharge_on_ac_enabled);
	zassert_equal(CHARGE_CONTROL_DISCHARGE, get_chg_ctrl_mode());

	custom_discharge_on_ac_rv = EC_SUCCESS;
	zassert_equal(EC_SUCCESS, set_chg_ctrl_mode(CHARGE_CONTROL_NORMAL));
	zassert_equal(0, custom_discharge_on_ac_enabled);
	zassert_equal(CHARGE_CONTROL_NORMAL, get_chg_ctrl_mode());
}
#endif
