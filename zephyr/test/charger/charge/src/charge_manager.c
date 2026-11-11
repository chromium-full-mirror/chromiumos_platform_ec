/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "charge_manager.h"
#include "charger_test.h"
#include "drivers/ucsi_v3.h"
#include "ec_commands.h"
#include "emul/emul_pdc.h"
#include "host_command.h"
#include "usbc/pdc_power_mgmt.h"

#include <string.h>

#include <zephyr/ztest.h>

#define RTS5453P_NODE DT_NODELABEL(pdc_emul1)
static const struct emul *emul = EMUL_DT_GET(RTS5453P_NODE);

ZTEST_SUITE(charge_manager, charger_predicate_post_main, NULL, NULL, NULL,
	    NULL);

/**
 * Test the default implementation of board_fill_source_power_info(). The fill
 * function should reset all the power info values. If the test binary overrides
 * board_fill_source_power_info(), then this test can be removed.
 */
ZTEST_USER(charge_manager, test_default_fill_power_info)
{
	struct ec_response_usb_pd_power_info info = {
		.meas = {
			.voltage_now = 10,
			.voltage_max = 10,
			.current_max = 10,
			.current_lim = 10,
		},
		.max_power = 10,
	};

	board_fill_source_power_info(0, &info);
	zassert_equal(info.meas.voltage_now, 0);
	zassert_equal(info.meas.voltage_max, 0);
	zassert_equal(info.meas.current_max, 0);
	zassert_equal(info.meas.current_lim, 0);
	zassert_equal(info.max_power, 0);
}

/**
 * Test the default implementation of board_charge_port_is_connected(). This
 * function should always return 1 regardless of input.
 */
ZTEST_USER(charge_manager, test_default_charge_port_is_connected)
{
	zassert_true(board_charge_port_is_connected(-1));
	zassert_true(board_charge_port_is_connected(0));
	zassert_true(board_charge_port_is_connected(1));
	zassert_true(board_charge_port_is_connected(500));
}

ZTEST_USER(charge_manager, test_default_charge_port_is_sink)
{
	zassert_true(board_charge_port_is_sink(-1));
	zassert_true(board_charge_port_is_sink(0));
	zassert_true(board_charge_port_is_sink(1));
	zassert_true(board_charge_port_is_sink(500));
}

ZTEST_USER(charge_manager, test_has_no_active_charge_port)
{
	union connector_status_t connector_status = {};

	charge_manager_leave_safe_mode();
	zassert_false(charge_manager_has_active_charge_port());

	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	pdc_power_mgmt_wait_for_sync(0, -1);
	zassert_true(charge_manager_has_active_charge_port());

	emul_pdc_disconnect(emul);
	pdc_power_mgmt_wait_for_sync(0, -1);
	zassert_false(charge_manager_has_active_charge_port());
}

ZTEST_USER(charge_manager, test_pd_power_info_charging_port)
{
	union connector_status_t connector_status = {};
	struct ec_params_usb_pd_power_info params = {
		.port = PD_POWER_CHARGING_PORT,
	};
	struct ec_response_usb_pd_power_info response;

	charge_manager_leave_safe_mode();
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	pdc_power_mgmt_wait_for_sync(0, -1);

	memset(&response, 0xff, sizeof(response));
	zassert_ok(ec_cmd_usb_pd_power_info(NULL, &params, &response));
	zassert_equal(response.role, USB_PD_PORT_POWER_SINK);
	zassert_equal(response.reserved1, 0);
	zassert_true(response.max_power > 0);

	emul_pdc_disconnect(emul);
	pdc_power_mgmt_wait_for_sync(0, -1);
}

ZTEST_USER(charge_manager, test_available_current)
{
	struct charge_port_info charge = {
		.current = 500,
		.voltage = 5000,
	};

	charge_manager_update_charge(CHARGE_SUPPLIER_TYPEC, 0, &charge);
	zassert_equal(
		charge_manager_get_available_current(CHARGE_SUPPLIER_TYPEC, 0),
		500);

	charge.current = 0;
	charge.voltage = 0;
	charge_manager_update_charge(CHARGE_SUPPLIER_TYPEC, 0, &charge);
	zassert_equal(
		charge_manager_get_available_current(CHARGE_SUPPLIER_TYPEC, 0),
		0);

	/* Valid boundary suppliers (0 and CHARGE_SUPPLIER_COUNT - 1). */
	charge.current = 500;
	charge.voltage = 5000;
	charge_manager_update_charge(CHARGE_SUPPLIER_PD, 0, &charge);
	charge_manager_update_charge(CHARGE_SUPPLIER_COUNT - 1, 0, &charge);
	zassert_equal(charge_manager_get_available_current(CHARGE_SUPPLIER_PD,
							   0),
		      500);
	zassert_equal(charge_manager_get_available_current(
			      CHARGE_SUPPLIER_COUNT - 1, 0),
		      500);

	charge.current = 0;
	charge.voltage = 0;
	charge_manager_update_charge(CHARGE_SUPPLIER_PD, 0, &charge);
	charge_manager_update_charge(CHARGE_SUPPLIER_COUNT - 1, 0, &charge);
	zassert_equal(
		charge_manager_get_available_current(CHARGE_SUPPLIER_PD, 0), 0);
	zassert_equal(charge_manager_get_available_current(
			      CHARGE_SUPPLIER_COUNT - 1, 0),
		      0);

	/* Out-of-bounds supplier updates should be safely ignored. */
	charge.current = 500;
	charge.voltage = 5000;
	charge_manager_update_charge(CHARGE_SUPPLIER_NONE, 0, &charge);
	charge_manager_update_charge(CHARGE_SUPPLIER_COUNT, 0, &charge);

	/* An invalid supplier or port has no current. */
	zassert_equal(charge_manager_get_available_current(CHARGE_SUPPLIER_NONE,
							   0),
		      0);
	zassert_equal(
		charge_manager_get_available_current(CHARGE_SUPPLIER_COUNT, 0),
		0);
	zassert_equal(
		charge_manager_get_available_current(CHARGE_SUPPLIER_TYPEC, -1),
		0);
	zassert_equal(charge_manager_get_available_current(
			      CHARGE_SUPPLIER_TYPEC, CHARGE_PORT_COUNT),
		      0);
}

extern struct k_mutex cm_refresh;

ZTEST_USER(charge_manager, test_charge_manager_refresh_mutex_retry)
{
	union connector_status_t connector_status = {};

	charge_manager_leave_safe_mode();
	zassert_false(charge_manager_has_active_charge_port());

	/* Acquire cm_refresh mutex to simulate lock contention from another
	 * task */
	k_mutex_lock(&cm_refresh, K_FOREVER);

	/* Connect partner which requests charge manager refresh */
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);

	/* Give sysworkq time to run charge_manager_refresh and reschedule */
	k_msleep(10);

	/*
	 * While mutex is held, charge_manager_refresh should reschedule
	 * without setting active port or blocking sysworkq.
	 */
	zassert_false(charge_manager_has_active_charge_port());

	/* Release mutex: rescheduled deferred refresh should now proceed */
	k_mutex_unlock(&cm_refresh);

	/* Wait for sync and verify active charge port is set */
	pdc_power_mgmt_wait_for_sync(0, -1);
	zassert_true(charge_manager_has_active_charge_port());

	/* Disconnect */
	emul_pdc_disconnect(emul);
	pdc_power_mgmt_wait_for_sync(0, -1);
	zassert_false(charge_manager_has_active_charge_port());
}
