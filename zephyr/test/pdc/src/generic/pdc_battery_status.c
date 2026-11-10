/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "battery_smart.h"
#include "drivers/pdc.h"
#include "emul/emul_pdc.h"
#include "fakes.h"
#include "hooks.h"
#include "test/util.h"
#include "usbc/pdc_power_mgmt.h"

#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#define PDC_NODE_PORT0 DT_NODELABEL(pdc_emul1)
#define TEST_USBC_PORT0 USBC_PORT_FROM_PDC_DRIVER_NODE(PDC_NODE_PORT0)

static const struct emul *emul = EMUL_DT_GET(PDC_NODE_PORT0);

FAKE_VALUE_FUNC(int, chipset_in_state, int);

static void *pdc_battery_status_setup(void)
{
	zassume(TEST_USBC_PORT0 < CONFIG_USB_PD_PORT_MAX_COUNT,
		"TEST_USBC_PORT0 is invalid");

	return NULL;
}

static void pdc_battery_status_reset(void *data)
{
	emul_pdc_reset(emul);
	emul_pdc_set_response_delay(emul, 0);
	emul_pdc_disconnect(emul);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	battery_static[BATT_IDX_MAIN].design_voltage = 7700; /* 7.7V */
	battery_static[BATT_IDX_MAIN].design_capacity = 5000; /* 5000 mAh */

	test_batt_params = (struct batt_params){
		.is_present = BP_YES,
		.full_capacity = 4800, /* 4800 mAh */
		.remaining_capacity = 1000, /* 1000 mAh */
		.status = STATUS_DISCHARGING,
		.flags = 0,
	};
}

ZTEST_SUITE(pdc_battery_status, NULL, pdc_battery_status_setup,
	    pdc_battery_status_reset, pdc_battery_status_reset, NULL);

ZTEST_USER(pdc_battery_status, test_battery_status_snk_connection)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;
	union battery_capability_t bcap;

	/* Connect a sink partner */
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	/* Verify emulator received correct battery status */
	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_present, 1);
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_DISCHARGING);
	/* 1000mAh * 7.7V = 7700mWh = 7.7Wh. In 0.1Wh units, this is 77. */
	zassert_equal(bstat.present_capacity, 77);

	/* Verify emulator received correct battery capability */
	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.vid, CONFIG_PLATFORM_EC_USB_VID);
	zassert_equal(bcap.pid, CONFIG_PLATFORM_EC_USB_PID);
	/* Design: 5000mAh * 7.7V = 38500mWh = 38.5Wh. In 0.1Wh units, this is
	 * 385. */
	zassert_equal(bcap.design_capacity, 385);
	/* Full charge: 4800mAh * 7.7V = 36960mWh = 36.96Wh. In 0.1Wh units,
	 * this is 370. */
	zassert_equal(bcap.last_full_charge_capacity, 370);

	/* Simulate battery remaining capacity decrease to 900 mAh */
	test_batt_params.remaining_capacity = 900;

	/* Trigger hook */
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	/* Verify emulator received correct battery status */
	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	/* 900mAh * 7.7V = 6930mWh = 6.93Wh. In 0.1Wh units, this is 69.3. */
	zassert_equal(bstat.present_capacity, 69);
}

ZTEST_USER(pdc_battery_status, test_battery_status_src_connection)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;
	union battery_capability_t bcap;

	/* Connect a source partner */
	emul_pdc_configure_src(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	/* Verify emulator received correct battery status */
	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_present, 1);
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_DISCHARGING);
	/* 1000mAh * 7.7V = 7700mWh = 7.7Wh. In 0.1Wh units, this is 77. */
	zassert_equal(bstat.present_capacity, 77);

	/* Verify emulator received correct battery capability */
	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.vid, CONFIG_PLATFORM_EC_USB_VID);
	zassert_equal(bcap.pid, CONFIG_PLATFORM_EC_USB_PID);
	/* Design: 5000mAh * 7.7V = 38500mWh = 38.5Wh. In 0.1Wh units, this is
	 * 385. */
	zassert_equal(bcap.design_capacity, 385);
	/* Full charge: 4800mAh * 7.7V = 36960mWh = 36.96Wh. In 0.1Wh units,
	 * this is 370. */
	zassert_equal(bcap.last_full_charge_capacity, 370);

	/* Simulate battery remaining capacity increase to 1100 mAh */
	test_batt_params.remaining_capacity = 1100;

	/* Trigger hook */
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	/* Verify emulator received correct battery status */
	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	/* 1100mAh * 7.7V = 8470mWh = 8.47Wh. In 0.1Wh units, this is 85. */
	zassert_equal(bstat.present_capacity, 85);
}

ZTEST_USER(pdc_battery_status, test_battery_not_present)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;
	union battery_capability_t bcap;

	test_batt_params.is_present = BP_NO;
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_present, 0);
	zassert_equal(bstat.present_capacity, BSDO_CAP_UNKNOWN);

	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.design_capacity, 0);
	zassert_equal(bcap.last_full_charge_capacity, 0);
}

ZTEST_USER(pdc_battery_status, test_battery_zero_design_voltage)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;
	union battery_capability_t bcap;

	battery_static[BATT_IDX_MAIN].design_voltage = 0;
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.present_capacity, BSDO_CAP_UNKNOWN);

	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.design_capacity, 0xffff);
	zassert_equal(bcap.last_full_charge_capacity, 0xffff);
}

ZTEST_USER(pdc_battery_status, test_battery_unknown_capacities)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;
	union battery_capability_t bcap;

	/* 1. Zero design capacity (c == 0) and bad remaining capacity flag */
	battery_static[BATT_IDX_MAIN].design_capacity = 0;
	test_batt_params.flags = BATT_FLAG_BAD_REMAINING_CAPACITY;
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.present_capacity, BSDO_CAP_UNKNOWN);

	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.design_capacity, 0xffff);
	/* Full charge: 4800mAh * 7.7V = 36960mWh = 36.96Wh -> 370 in 0.1Wh */
	zassert_equal(bcap.last_full_charge_capacity, 370);

	/* 2. Valid design capacity, bad full capacity flag */
	emul_pdc_disconnect(emul);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	battery_static[BATT_IDX_MAIN].design_capacity = 5000;
	test_batt_params.flags = BATT_FLAG_BAD_FULL_CAPACITY;
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.present_capacity, 77);

	zassert_ok(emul_pdc_get_battery_capability(emul, &bcap));
	zassert_equal(bcap.design_capacity, 385);
	zassert_equal(bcap.last_full_charge_capacity, 0xffff);
}

ZTEST_USER(pdc_battery_status, test_battery_charging_states)
{
	union connector_status_t connector_status = {};
	union battery_status_t bstat;

	/* Connect sink partner */
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	/* Verify initial state is discharging */
	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_DISCHARGING);

	/* Fully charged -> IDLE */
	test_batt_params.status = STATUS_FULLY_CHARGED;
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_IDLE);

	/* Charging (status = 0) -> CHARGING */
	test_batt_params.status = 0;
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_CHARGING);

	/* Bad status flag -> IDLE */
	test_batt_params.flags = BATT_FLAG_BAD_STATUS;
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
	zassert_ok(pdc_power_mgmt_wait_for_sync(TEST_USBC_PORT0, -1));

	zassert_ok(emul_pdc_get_battery_status(emul, &bstat));
	zassert_equal(bstat.battery_state, BSDO_BATTERY_STATE_IDLE);
}
