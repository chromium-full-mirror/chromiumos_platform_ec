/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Test MT6370 / RT946x BC1.2 detection and USB D+/D- mux GPIO control.
 */

#include "battery.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "charger.h"
#include "common.h"
#include "driver/charger/rt946x.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "tcpm/tcpm.h"
#include "test_util.h"
#include "usb_pd.h"

#include <string.h>

static uint8_t mock_regs[256];
static bool mock_pd_capable;
static enum tcpc_cc_voltage_status mock_cc1;
static enum tcpc_cc_voltage_status mock_cc2;

static const struct battery_info mock_batt_info = {
	.voltage_max = 4400,
	.voltage_normal = 3850,
	.voltage_min = 3000,
	.precharge_voltage = 3000,
	.precharge_current = 256,
};

const struct battery_info *battery_get_info(void)
{
	return &mock_batt_info;
}

const struct charger_config_t chg_chips[] = {
	{
		.i2c_port = I2C_PORT_CHARGER,
		.i2c_addr_flags = RT946X_ADDR_FLAGS,
		.drv = &rt946x_drv,
	},
};

static int mock_tcpm_get_cc(int port, enum tcpc_cc_voltage_status *cc1,
			    enum tcpc_cc_voltage_status *cc2)
{
	*cc1 = mock_cc1;
	*cc2 = mock_cc2;
	return EC_SUCCESS;
}

static const struct tcpm_drv mock_tcpm_drv = {
	.get_cc = &mock_tcpm_get_cc,
};

const struct tcpc_config_t tcpc_config[] = {
	[0] = {
		.drv = &mock_tcpm_drv,
	},
};

uint8_t board_get_usb_pd_port_count(void)
{
	return CONFIG_USB_PD_PORT_MAX_COUNT;
}

bool pd_capable(int port)
{
	return mock_pd_capable;
}

int pd_is_vbus_present(int port)
{
	return 0;
}

void pd_vbus_low(int port)
{
}

void charge_manager_update_charge(int supplier, int port,
				  const struct charge_port_info *charge)
{
}

int charge_get_active_chg_chip(void)
{
	return CHARGER_SOLO;
}

static int rt946x_i2c_xfer(int port, uint16_t addr_flags, const uint8_t *out,
			   int out_size, uint8_t *in, int in_size, int flags)
{
	static uint8_t reg;
	int i;

	if (port != I2C_PORT_CHARGER || addr_flags != RT946X_ADDR_FLAGS)
		return EC_ERROR_INVAL;

	if (out_size > 0 && (flags & I2C_XFER_START)) {
		reg = out[0];
		for (i = 1; i < out_size; i++)
			mock_regs[(uint8_t)(reg + i - 1)] = out[i];
	}

	if (in && in_size > 0) {
		for (i = 0; i < in_size; i++)
			in[i] = mock_regs[(uint8_t)(reg + i)];
	}

	return EC_SUCCESS;
}
DECLARE_TEST_I2C_XFER(rt946x_i2c_xfer);

void before_test(void)
{
	memset(mock_regs, 0, sizeof(mock_regs));
	mock_pd_capable = false;
	mock_cc1 = TYPEC_CC_VOLT_OPEN;
	mock_cc2 = TYPEC_CC_VOLT_OPEN;
	gpio_set_level(GPIO_BC12_DET_EN, 0);
}

test_static int test_toggle_bc12_pd_capable(void)
{
	gpio_set_level(GPIO_BC12_DET_EN, 1);
	mock_regs[RT946X_REG_DPDM1] = RT946X_MASK_USBCHGEN;
	mock_pd_capable = true;
	mock_cc1 = TYPEC_CC_VOLT_RP_3_0;

	TEST_EQ(rt946x_toggle_bc12_detection(), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 0, "%d");
	TEST_EQ(mock_regs[RT946X_REG_DPDM1] & RT946X_MASK_USBCHGEN, 0, "%d");

	return EC_SUCCESS;
}

test_static int test_toggle_bc12_non_pd_no_rp(void)
{
	mock_pd_capable = false;
	mock_cc1 = TYPEC_CC_VOLT_RD;
	mock_cc2 = TYPEC_CC_VOLT_OPEN;

	TEST_EQ(rt946x_toggle_bc12_detection(), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 0, "%d");
	TEST_EQ(mock_regs[RT946X_REG_DPDM1] & RT946X_MASK_USBCHGEN, 0, "%d");

	return EC_SUCCESS;
}

test_static int test_toggle_bc12_non_pd_rp(void)
{
	mock_pd_capable = false;
	mock_cc1 = TYPEC_CC_VOLT_RP_1_5;

	TEST_EQ(rt946x_toggle_bc12_detection(), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 1, "%d");
	TEST_EQ(mock_regs[RT946X_REG_DPDM1] & RT946X_MASK_USBCHGEN,
		RT946X_MASK_USBCHGEN, "%d");

	return EC_SUCCESS;
}

test_static int test_discharge_and_disconnect_disable_bc12(void)
{
	mock_pd_capable = false;
	mock_cc2 = TYPEC_CC_VOLT_RP_DEF;

	TEST_EQ(rt946x_toggle_bc12_detection(), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 1, "%d");

	TEST_EQ(charger_discharge_on_ac(1), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 0, "%d");
	TEST_EQ(mock_regs[RT946X_REG_DPDM1] & RT946X_MASK_USBCHGEN, 0, "%d");
	TEST_EQ(mock_regs[RT946X_REG_CHGCTRL1] & RT946X_MASK_HZ_EN,
		RT946X_MASK_HZ_EN, "%d");

	TEST_EQ(rt946x_toggle_bc12_detection(), EC_SUCCESS, "%d");
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 1, "%d");

	hook_notify(HOOK_USB_PD_DISCONNECT);
	TEST_EQ(gpio_get_level(GPIO_BC12_DET_EN), 0, "%d");
	TEST_EQ(mock_regs[RT946X_REG_DPDM1] & RT946X_MASK_USBCHGEN, 0, "%d");

	return EC_SUCCESS;
}

void run_test(int argc, const char **argv)
{
	test_reset();

	RUN_TEST(test_toggle_bc12_pd_capable);
	RUN_TEST(test_toggle_bc12_non_pd_no_rp);
	RUN_TEST(test_toggle_bc12_non_pd_rp);
	RUN_TEST(test_discharge_and_disconnect_disable_bc12);

	test_print_result();
}
