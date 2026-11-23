/* Copyright 2024 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "battery.h"
#include "charge_state.h"
#include "charger.h"
#include "common.h"
#include "console.h"
#include "driver/charger/rt9490.h"
#include "extpower.h"
#include "hooks.h"
#include "ite_hooks.h"
#include "system.h"
#include "temp_sensor/temp_sensor.h"
#include "util.h"

#include <zephyr/drivers/bbram.h>

#define TEMP_BUFF_SIZE 60
#define KEEP_TIME 5
#define TEMP_NOT_READY -1
#define TARGET_UPPER 50
#define TARGET_LOWER 45

#define BBRAM_DISABLE_LIMIT_OFFSET 0x11
#define BBRAM_DISABLE_LIMIT_MAGIC 0x5A

BUILD_ASSERT(IS_ENABLED(CONFIG_BOARD_VELUZA) || IS_ENABLED(CONFIG_TEST));

enum limit_state {
	LIMIT_NONE = 0,
	LIMIT_IDLE,
	LIMIT_DISCHARGE,
};

/* Persistent flag loaded from BBRAM: once true, 50% limit is disabled */
static bool bbram_limit_disabled;

#ifdef CONFIG_BBRAM
static const struct device *const bbram_dev =
	COND_CODE_1(DT_HAS_CHOSEN(cros_ec_bbram),
		    DEVICE_DT_GET(DT_CHOSEN(cros_ec_bbram)), NULL);
#endif

static int set_limit_disabled_bbram(bool disable)
{
#ifdef CONFIG_BBRAM
	uint8_t val = disable ? BBRAM_DISABLE_LIMIT_MAGIC : 0x00;
	int rv;

	if (!bbram_dev || !device_is_ready(bbram_dev))
		return -ENODEV;

	rv = bbram_write(bbram_dev, BBRAM_DISABLE_LIMIT_OFFSET, 1, &val);
	if (rv == 0)
		bbram_limit_disabled = disable;

	return rv;
#else
	(void)disable;
	return -ENODEV;
#endif
}

static void limit_bbram_init(void)
{
#ifdef CONFIG_BBRAM
	uint8_t val = 0;

	if (!bbram_dev || !device_is_ready(bbram_dev))
		return;

	if (bbram_read(bbram_dev, BBRAM_DISABLE_LIMIT_OFFSET, 1, &val) ==
	    0) {
		bbram_limit_disabled =
			(val == BBRAM_DISABLE_LIMIT_MAGIC);
	}
#endif
}
DECLARE_HOOK(HOOK_INIT, limit_bbram_init, HOOK_PRIO_DEFAULT);

__override bool board_charge_control_normal(int8_t *lower, int8_t *upper)
{
	if (*lower == 101 && *upper == 102) {
		if (set_limit_disabled_bbram(true) != 0) {
			cprints(CC_CHARGER,
				"chargecontrol normal %d %d failed: unable to update BBRAM",
				*lower, *upper);
			return false;
		}
		cprints(CC_CHARGER,
			"chargecontrol normal %d %d received: disabled 50%% limit in BBRAM",
			*lower, *upper);
		return true;
	}

	return false;
}

/* calculate current average temperature */
static int average_tempature(void)
{
	static int temp_history_buffer[TEMP_BUFF_SIZE];
	static int buff_ptr;
	static int temp_sum;
	static int past_temp;
	static int avg_temp;
	int cur_temp, t;
	int rv;

	rv = temp_sensor_read(TEMP_SENSOR_ID(DT_NODELABEL(temp_charger)), &t);
	if (rv)
		return TEMP_NOT_READY;

	cur_temp = K_TO_C(t);
	past_temp = temp_history_buffer[buff_ptr];
	temp_history_buffer[buff_ptr] = cur_temp;
	temp_sum = temp_sum + temp_history_buffer[buff_ptr] - past_temp;
	buff_ptr++;
	if (buff_ptr >= TEMP_BUFF_SIZE) {
		buff_ptr = 0;
	}
	/* Calculate per minute temperature.
	 * It's expected low temperature when the first 60 seconds.
	 */
	avg_temp = temp_sum / TEMP_BUFF_SIZE;
	return avg_temp;
}
static int current_level;

struct current_table_struct {
	int temperature;
	int current;
};

/* Limit charging current table : 2554/1400/365
 * note this should be in descending order.
 */
static const struct current_table_struct current_table[] = {
	{ 0, 2554 },
	{ 55, 1400 },
	{ 57, 365 },
};

#define CURRENT_LEVELS ARRAY_SIZE(current_table)

/*
 * When the current average temperature is over the current_table setting
 * and the temperature is kept at 5 seconds, it will follow the thermal
 * table drop charging current to avoid skin over temperature.
 */
static void current_update(void)
{
	int temp;
	static uint8_t uptime;
	static uint8_t dntime;

	temp = average_tempature();
	if (temp == TEMP_NOT_READY)
		return;
#ifndef CONFIG_TEST
	if (led_pwr_get_state() == LED_PWRS_DISCHARGE) {
		current_level = 0;
		uptime = 0;
		dntime = 0;
		return;
	}
#endif

	if ((current_level + 1) < CURRENT_LEVELS &&
	    temp >= current_table[current_level + 1].temperature) {
		/* Increase level */
		dntime = 0;
		if (uptime < KEEP_TIME) {
			uptime++;
		} else {
			uptime = 0;
			current_level++;
		}
	} else if (current_level != 0 &&
		   temp < current_table[current_level].temperature) {
		/* Decrease level */
		uptime = 0;
		if (dntime < KEEP_TIME) {
			dntime++;
		} else {
			dntime = 0;
			current_level--;
		}
	} else {
		uptime = 0;
		dntime = 0;
	}
}
DECLARE_HOOK(HOOK_SECOND, current_update, HOOK_PRIO_DEFAULT);

static void battery_limit_override(struct charge_state_data *curr)
{
	static enum limit_state limit_state = LIMIT_NONE;
	static bool prev_disch;
	int display_soc;

	/*
	 * Bypass 50% limit once disabled via chargecontrol normal
	 * 101 102 (BBRAM flag is set), AC is unplugged, or SoC is invalid.
	 */
	if (bbram_limit_disabled || !curr->ac ||
	    (curr->batt.is_present != BP_YES) ||
	    !(curr->batt.flags & BATT_FLAG_RESPONSIVE) ||
	    (curr->batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE)) {
		if (prev_disch) {
			charger_discharge_on_ac(0);
			prev_disch = false;
		}
		limit_state = LIMIT_NONE;
		return;
	}

	/* Align with OS displayed SoC */
	display_soc = charge_get_display_charge() / 10;

	if (display_soc > TARGET_UPPER) {
		limit_state = LIMIT_DISCHARGE;
	} else if (display_soc <= TARGET_LOWER) {
		limit_state = LIMIT_NONE;
	} else if (display_soc == TARGET_UPPER ||
		   limit_state == LIMIT_DISCHARGE) {
		limit_state = LIMIT_IDLE;
	}

	if (limit_state == LIMIT_DISCHARGE) {
		charger_discharge_on_ac(1);
		prev_disch = true;
		curr->batt.flags &= ~BATT_FLAG_WANT_CHARGE;
		curr->state = ST_DISCHARGE;
		curr->requested_current = 0;
		curr->requested_voltage = 0;
	} else {
		if (prev_disch) {
			charger_discharge_on_ac(0);
			prev_disch = false;
		}
		if (limit_state == LIMIT_IDLE) {
			curr->batt.flags &= ~BATT_FLAG_WANT_CHARGE;
			curr->state = ST_IDLE;
			curr->requested_current = 0;
			curr->requested_voltage = 0;
		}
	}
}

int charger_profile_override(struct charge_state_data *curr)
{
	/*
	 * Precharge must be executed when communication is failed on
	 * dead battery.
	 */
	if (!(curr->batt.flags & BATT_FLAG_RESPONSIVE)) {
		battery_limit_override(curr);
		return 0;
	}

	if (curr->requested_current > current_table[current_level].current)
		curr->requested_current = current_table[current_level].current;

	battery_limit_override(curr);

	return 0;
}

enum ec_status charger_profile_override_get_param(uint32_t param,
						  uint32_t *value)
{
	return EC_RES_INVALID_PARAM;
}

enum ec_status charger_profile_override_set_param(uint32_t param,
						  uint32_t value)
{
	return EC_RES_INVALID_PARAM;
}

static int command_chg_limit(int argc, const char **argv)
{
	if (argc == 1) {
		ccprintf("bbram_limit_disabled: %d\n",
			 bbram_limit_disabled);
		return EC_SUCCESS;
	}

	if (argc != 2)
		return EC_ERROR_PARAM_COUNT;

	if (!strcasecmp(argv[1], "clear")) {
		if (set_limit_disabled_bbram(false) != 0) {
			ccprintf("Failed to clear BBRAM flag\n");
			return EC_ERROR_UNKNOWN;
		}
		ccprintf("Cleared BBRAM 50%% limit disable flag\n");
		return EC_SUCCESS;
	}

	return EC_ERROR_PARAM1;
}
DECLARE_CONSOLE_COMMAND(chglimit, command_chg_limit, "[clear]",
			"Get 50% charging limit status or clear BBRAM flag");

void board_rt9490_adc_enalbe(void)
{
	rt9490_enable_adc(CHARGER_SOLO, 1);
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, board_rt9490_adc_enalbe, HOOK_PRIO_DEFAULT);

void board_rt9490_adc_disable(void)
{
	rt9490_enable_adc(CHARGER_SOLO, extpower_is_present());
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, board_rt9490_adc_disable,
	     HOOK_PRIO_DEFAULT);

__override void board_rt9490_adc_control(void)
{
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		rt9490_enable_adc(CHARGER_SOLO, extpower_is_present());
}
