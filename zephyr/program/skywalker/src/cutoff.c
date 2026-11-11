/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "battery_fuel_gauge.h"
#include "battery_smart.h"
#include "console.h"
#include "i2c.h"

#define CPRINTS(format, args...) cprints(CC_SYSTEM, format, ##args)

/*
 * Bypass sb_cutoff_or_in_progress() which blocks all sb_read*() during
 * cutoff. We use i2c primitives directly for reads; sb_write and
 * sb_write_block are safe because they only check battery_is_cut_off(),
 * not battery_cutoff_in_progress().
 */
static enum ec_error_list battery_get_fet_status_regval(int *regval)
{
	const struct batt_conf_embed *conf = get_batt_conf();
	const struct board_batt_params *params;
	int rv;
	uint8_t data[6];

	if (!conf)
		return EC_ERROR_UNKNOWN;
	params = &conf->config;

	/* Read the status of charge/discharge FETs */
	if (params->fuel_gauge.flags & FUEL_GAUGE_FLAG_MFGACC) {
		uint16_t addr_flags = BATTERY_ADDR_FLAGS;
		int read_len = 0;

#ifdef CONFIG_SMBUS_PEC
		addr_flags |= I2C_FLAG_PEC;
#endif
		if (params->fuel_gauge.flags &
		    FUEL_GAUGE_FLAG_MFGACC_SMB_BLOCK) {
			uint8_t op_status[3] = {
				0x02,
				PARAM_OPERATION_STATUS & 0xFF,
				PARAM_OPERATION_STATUS >> 8,
			};
			rv = i2c_write_block(I2C_PORT_BATTERY, addr_flags,
					     SB_ALT_MANUFACTURER_ACCESS,
					     op_status, sizeof(op_status));
		} else {
			rv = i2c_write16(I2C_PORT_BATTERY, addr_flags,
					 SB_MANUFACTURER_ACCESS,
					 PARAM_OPERATION_STATUS);
		}
		if (rv)
			return rv;

		rv = i2c_read_sized_block(I2C_PORT_BATTERY, addr_flags,
					  SB_ALT_MANUFACTURER_ACCESS, data,
					  sizeof(data), &read_len);
		if (rv)
			return rv;
		/* Get the lowest 16bits of the OperationStatus() data */
		*regval = data[2] | data[3] << 8;
	} else {
		uint16_t addr_flags = BATTERY_ADDR_FLAGS;

#ifdef CONFIG_SMBUS_PEC
		addr_flags |= I2C_FLAG_PEC;
#endif
		rv = i2c_read16(I2C_PORT_BATTERY, addr_flags,
				params->fuel_gauge.fet.reg_addr, regval);
	}

	return rv;
}

__override bool board_battery_cutoff_succeeded(void)
{
	const struct batt_conf_embed *conf = get_batt_conf();
	const struct board_batt_params *params;
	int reg;

	if (!conf) {
		CPRINTS("cutoff: no battery config");
		return false;
	}
	params = &conf->config;

	if (battery_get_fet_status_regval(&reg)) {
		CPRINTS("cutoff: FET reg read failed");
		return false;
	}

	if ((reg & params->fuel_gauge.fet.reg_mask) ==
	    params->fuel_gauge.fet.disconnect_val) {
		CPRINTS("cutoff: D-FET off (reg=0x%04x)", reg);
		return true;
	}

	CPRINTS("cutoff: D-FET on (reg=0x%04x), re-send", reg);
	return false;
}
