/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adsp_comms.h"
#include "battery.h"
#include "battery_smart.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "chipset.h"
#include "common.h"
#include "ec_commands.h"
#include "extpower.h"
#include "hooks.h"
#include "util.h"

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_DECLARE(adsp_comms, LOG_LEVEL_INF);

static int active_charge_port = CHARGE_PORT_NONE;
static enum led_pwr_state active_charge_state = LED_PWRS_IDLE;
static int active_battery_status;

int charge_manager_get_active_charge_port_no_lock(void)
{
	return active_charge_port;
}

int charge_manager_get_active_charge_port(void)
{
	return charge_manager_get_active_charge_port_no_lock();
}

enum led_pwr_state led_pwr_get_state(void)
{
	/*
	 * Workaround (b:549816337): On boot/reset without AC, ADSP initially
	 * reports LED_PWRS_IDLE before evaluating the charger state. Enforce
	 * DISCHARGE using EC's physical AC detection as ground truth. The
	 * proper fix should come from ADSP firmware.
	 */
	if (!extpower_is_present()) {
		if (active_charge_state == LED_PWRS_ERROR) {
			return LED_PWRS_ERROR;
		}
		return LED_PWRS_DISCHARGE;
	}

	return active_charge_state;
}

static int command_chgstate(const struct shell *sh, size_t argc, char **argv)
{
	const char *state_str = "idle";
	int soc = battery_get_fake_soc();
	enum battery_present bp = battery_is_present();
	const char *pres_str =
		(bp == BP_YES) ? "YES" : (bp == BP_NO ? "NO" : "NOT_SURE");
	int batt_is_charging = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (active_charge_state == LED_PWRS_CHARGE ||
	    active_charge_state == LED_PWRS_CHARGE_NEAR_FULL) {
		state_str = "charge";
		batt_is_charging = 1;
	} else if (active_charge_state == LED_PWRS_DISCHARGE ||
		   active_charge_state == LED_PWRS_DISCHARGE_FULL) {
		state_str = "discharge";
	}

	shell_print(sh, "state = %s", state_str);
	shell_print(sh, "ac = %d", extpower_is_present());
	shell_print(sh, "batt_is_charging = %d", batt_is_charging);
	shell_print(sh, "batt.*:");
	shell_print(sh, "\tstate_of_charge = %d%%", soc);
	shell_print(sh, "\tis_present = %s", pres_str);

	if (IS_ENABLED(CONFIG_PLATFORM_EC_CHARGE_CONTROL_PERSIST_TO_BBRAM)) {
		int8_t lower, upper;
		uint8_t flags;

		if (charge_control_load_from_bbram(&lower, &upper, &flags) ==
			    EC_SUCCESS &&
		    lower != CHARGE_CONTROL_SUSTAINER_DISABLED &&
		    upper != CHARGE_CONTROL_SUSTAINER_DISABLED) {
			shell_print(sh, "Battery sustainer = on (%d%% ~ %d%%)",
				    lower, upper);
		} else {
			shell_print(sh, "Battery sustainer = off");
		}
	}

	return 0;
}

static int cmd_chgstate_sustain(const struct shell *sh, size_t argc,
				char **argv)
{
	char *e;
	int lower, upper;
	int rv;

	lower = strtoi(argv[1], &e, 0);
	if (*e)
		return -EINVAL;
	upper = strtoi(argv[2], &e, 0);
	if (*e)
		return -EINVAL;

	if (lower == CHARGE_CONTROL_SUSTAINER_DISABLED ||
	    upper == CHARGE_CONTROL_SUSTAINER_DISABLED) {
		rv = charge_control_save_to_bbram(
			CHARGE_CONTROL_SUSTAINER_DISABLED,
			CHARGE_CONTROL_SUSTAINER_DISABLED, 0);
	} else if (0 <= lower && lower <= upper && upper <= 100) {
		rv = charge_control_save_to_bbram(
			lower, upper,
			(lower < upper) ? EC_CHARGE_CONTROL_FLAG_NO_IDLE : 0);
	} else {
		return -EINVAL;
	}
	if (rv)
		return -EINVAL;

	return command_chgstate(sh, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_chgstate_cmds,
	SHELL_COND_CMD_ARG(
		IS_ENABLED(CONFIG_PLATFORM_EC_CHARGE_CONTROL_PERSIST_TO_BBRAM),
		sustain, NULL,
		SHELL_HELP("Set battery sustain limits", "<lower> <upper>"),
		cmd_chgstate_sustain, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_ARG_REGISTER(chgstate, &sub_chgstate_cmds,
		       "Get/set charge state machine status", command_chgstate,
		       1, 0);

#ifdef CONFIG_BATTERY_STATUS_CUSTOM
test_mockable int battery_status(int *status)
{
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF)) {
		return sb_read(SB_BATTERY_STATUS, status);
	}

	*status = active_battery_status;
	return EC_SUCCESS;
}
#endif

static void adsp_power_state_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	LOG_INF("ADSP: Power State: 0x%04x", data);
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_DEFAULT, ADSP_POWER_STATE_REG_VAL,
		       adsp_power_state_cb);

static void adsp_oem_magic_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	if (data == ADSP_OEM_CUSTOM_MAGIC_VAL) {
		LOG_INF("ADSP: Comms established");
	} else {
		LOG_INF("ADSP: Incorrect magic packet received (0x%04x)", data);
	}
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM, ADSP_OEM_CUSTOM_REG_MAGIC,
		       adsp_oem_magic_cb);

static void adsp_oem_version_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	if (data == ADSP_OEM_CUSTOM_VERSION_1) {
		LOG_INF("ADSP: Version 1 identified");
	} else {
		LOG_ERR("ADSP: Incorrect version received: %d (expected %d)",
			data, ADSP_OEM_CUSTOM_VERSION_1);
	}
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM, ADSP_OEM_CUSTOM_REG_VERSION,
		       adsp_oem_version_cb);

static void adsp_oem_charge_port_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	if (data == ADSP_OEM_CUSTOM_CHARGE_PORT_DISABLED) {
		active_charge_port = CHARGE_PORT_NONE;
		LOG_INF("ADSP: Charging disabled");
	} else if (data >= ADSP_OEM_CUSTOM_CHARGE_PORT_START &&
		   data <= ADSP_OEM_CUSTOM_CHARGE_PORT_COUNT) {
		active_charge_port = data - ADSP_OEM_CUSTOM_CHARGE_PORT_START;
		LOG_INF("ADSP: Charging from USB%d", active_charge_port);
	} else {
		LOG_ERR("ADSP: Invalid charge port: %d", data);
	}
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM, ADSP_OEM_CUSTOM_REG_CHARGE_PORT,
		       adsp_oem_charge_port_cb);

static void adsp_oem_charge_state_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	switch (data) {
	case ADSP_OEM_CUSTOM_CHARGE_STATE_CHARGE:
		active_charge_state = LED_PWRS_CHARGE;
		break;
	case ADSP_OEM_CUSTOM_CHARGE_STATE_DISCHARGE:
		active_charge_state = LED_PWRS_DISCHARGE;
		break;
	case ADSP_OEM_CUSTOM_CHARGE_STATE_ERROR:
		active_charge_state = LED_PWRS_ERROR;
		break;
	case ADSP_OEM_CUSTOM_CHARGE_STATE_IDLE:
		active_charge_state = LED_PWRS_IDLE;
		break;
	case ADSP_OEM_CUSTOM_CHARGE_STATE_FORCED_IDLE:
		active_charge_state = LED_PWRS_FORCED_IDLE;
		break;
	case ADSP_OEM_CUSTOM_CHARGE_STATE_NEAR_FULL:
		active_charge_state = LED_PWRS_CHARGE_NEAR_FULL;
		break;
	default:
		LOG_ERR("ADSP: Invalid charge state: %d", data);
		return;
	}
	LOG_INF("ADSP: Charge State - %d", data);
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM,
		       ADSP_OEM_CUSTOM_REG_CHARGE_STATE,
		       adsp_oem_charge_state_cb);

static void adsp_oem_battery_state_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	active_battery_status = (int)data;
	LOG_INF("ADSP: Battery State: 0x%04x", data);
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM,
		       ADSP_OEM_CUSTOM_REG_BATTERY_STATE,
		       adsp_oem_battery_state_cb);

static void adsp_oem_battery_level_cb(uint8_t fid, uint8_t addr, uint16_t data)
{
	if (data > 100) {
		LOG_ERR("ADSP: Invalid battery level: %d", data);
		return;
	}

	battery_set_fake_soc((int)data);
	LOG_INF("ADSP: Battery Level: %d%%", data);
}
ADSP_COMMS_REGISTER_CB(ADSP_FEATURE_OEM_CUSTOM,
		       ADSP_OEM_CUSTOM_REG_BATTERY_LEVEL,
		       adsp_oem_battery_level_cb);

static void adsp_comms_shutdown_reset(void)
{
	battery_set_fake_soc(-1);
	active_charge_port = CHARGE_PORT_NONE;
	active_charge_state = LED_PWRS_IDLE;
	active_battery_status = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN_COMPLETE, adsp_comms_shutdown_reset,
	     HOOK_PRIO_DEFAULT);
