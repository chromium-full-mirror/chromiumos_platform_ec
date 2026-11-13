/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "charge_manager.h"
#include "charge_state.h"
#include "charger.h"
#include "charger_test.h"
#include "drivers/ucsi_v3.h"
#include "ec_commands.h"
#include "emul/emul_pdc.h"
#include "usbc/pdc_power_mgmt.h"

#include <zephyr/fff.h>
#include <zephyr/ztest.h>

BUILD_ASSERT(!IS_ENABLED(CONFIG_PLATFORM_EC_OCPC),
	     "This test suite does not support OCPC");

test_export_static bool charge_is_adapter_sufficient(int chgnum);

extern int extpower_present;

#define RTS5453P_NODE DT_NODELABEL(pdc_emul1)
static const struct emul *emul = EMUL_DT_GET(RTS5453P_NODE);

static void test_before(void *fixture)
{
	emul_pdc_disconnect(emul);
	pdc_power_mgmt_wait_for_sync(0, -1);
}

ZTEST_SUITE(charge_state, charger_predicate_post_main, NULL, test_before, NULL,
	    NULL);

ZTEST(charge_state, test_sufficient_adapter)
{
	union connector_status_t connector_status = {};
	enum led_pwr_state led;

	charge_manager_leave_safe_mode();

	/* Default best PDO is 20 volts */
	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_connect_partner(emul, &connector_status);
	extpower_present = 1;
	pdc_power_mgmt_wait_for_sync(0, -1);

	led = led_pwr_get_state();
	zassert_equal(led, LED_PWRS_CHARGE, "Returned led=%d, expected=%d", led,
		      LED_PWRS_CHARGE);
}

ZTEST(charge_state, test_insufficient_adapter)
{
	const struct charge_port_info undervolt = {
		.current = 1500,
		.voltage = 5000,
	};
	union connector_status_t connector_status = {};
	enum led_pwr_state led;

	uint32_t partner_pdos[] = {
		PDO_FIXED(5000, 3000, 0),
		PDO_FIXED(7200, 3000, 0),
		PDO_FIXED(12000, 3000, 0),
	};

	charge_manager_leave_safe_mode();

	emul_pdc_configure_snk(emul, &connector_status);
	emul_pdc_set_pdos(emul, SOURCE_PDO, PDO_OFFSET_0,
			  ARRAY_SIZE(partner_pdos), PARTNER_PDO, partner_pdos);
	emul_pdc_connect_partner(emul, &connector_status);
	extpower_present = 0;
	pdc_power_mgmt_wait_for_sync(0, -1);

	led = led_pwr_get_state();
	zassert_equal(led, LED_PWRS_INSUFFICIENT_ADAPTER,
		      "Returned led=%d, expected=%d", led,
		      LED_PWRS_INSUFFICIENT_ADAPTER);

	charge_manager_update_charge(CHARGE_SUPPLIER_PD, 0, NULL);
	zassert_equal(charge_manager_get_selected_charge_port(),
		      CHARGE_PORT_NONE);
	zassert_true(charge_manager_has_insufficient_adapter());

	charge_manager_update_dualrole(0, CAP_DUALROLE);
	charge_manager_update_charge(CHARGE_SUPPLIER_PD, 0, &undervolt);
	zassert_true(WAIT_FOR(!charge_manager_has_insufficient_adapter(),
			      1000000, k_msleep(10)));

	charge_manager_update_dualrole(0, CAP_DEDICATED);
	zassert_true(WAIT_FOR(charge_manager_has_insufficient_adapter(),
			      1000000, k_msleep(10)));

	zassert_ok(charge_manager_set_override(OVERRIDE_DONT_CHARGE));
	zassert_true(WAIT_FOR(!charge_manager_has_insufficient_adapter(),
			      1000000, k_msleep(10)));
	charge_manager_update_charge(CHARGE_SUPPLIER_PD, 0, NULL);
	charge_manager_update_dualrole(0, CAP_UNKNOWN);
	zassert_ok(charge_manager_set_override(OVERRIDE_OFF));
}

#define CHARGER_NODE DT_NODELABEL(charger)

ZTEST(charge_state, test_get_minimum_charging_mv)
{
	uint32_t mv = 0;
	const uint32_t expected_mv = DT_PROP(CHARGER_NODE, minimum_charging_mv);

	zassert_equal(charger_get_minimum_charging_mv(-1, &mv), EC_ERROR_INVAL);
	zassert_equal(charger_get_minimum_charging_mv(
			      board_get_charger_chip_count(), &mv),
		      EC_ERROR_INVAL);
	zassert_equal(charger_get_minimum_charging_mv(
			      charge_get_active_chg_chip(), &mv),
		      EC_SUCCESS);
	zassert_equal(expected_mv, mv);
}
