/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "hooks.h"
#include "test_state.h"
#include "timer.h"
#include "usb_dp_alt_mode.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_dp_hpd_gpio.h"
#include "usbc/pdc_power_mgmt.h"
#include "usbc/usb_muxes.h"

#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

extern uint64_t svdm_hpd_deadline[CONFIG_USB_PD_PORT_MAX_COUNT];
void hylia_dp_attention(int port, uint32_t vdo_dp_status);
void hylia_set_unattached(int port);

FAKE_VALUE_FUNC(int, pdc_power_mgmt_register_board_callback,
		enum pdc_power_mgmt_board_cb_t, const void *);
FAKE_VALUE_FUNC(mux_state_t, pdc_power_mgmt_get_dp_mux_mode, int);
FAKE_VALUE_FUNC(int, ppc_is_sourcing_vbus, int);
FAKE_VOID_FUNC(pd_set_vbus_discharge, int, int);
FAKE_VALUE_FUNC(int, ppc_vbus_sink_enable, int, int);
FAKE_VALUE_FUNC(int, ppc_vbus_source_enable, int, int);
FAKE_VOID_FUNC(hdmi_hpd_interrupt, enum gpio_signal);
FAKE_VOID_FUNC(xhci_interrupt, enum gpio_signal);

static void test_hylia_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(pdc_power_mgmt_register_board_callback);
	RESET_FAKE(pdc_power_mgmt_get_dp_mux_mode);
	RESET_FAKE(ppc_is_sourcing_vbus);
	RESET_FAKE(ppc_vbus_sink_enable);
	RESET_FAKE(ppc_vbus_source_enable);
	FFF_RESET_HISTORY();

	pdc_power_mgmt_get_dp_mux_mode_fake.return_val = USB_PD_MUX_DP_ENABLED;

	for (int i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++) {
		pd_set_suspend(i, 1);
	}
	k_msleep(10);

	for (int i = 0; i < CONFIG_USB_PD_PORT_MAX_COUNT; i++) {
		svdm_hpd_deadline[i] = 0;
		svdm_set_hpd_gpio(i, 0);
		usb_mux_set(i, USB_PD_MUX_NONE, USB_SWITCH_DISCONNECT, 0);
	}
}

ZTEST(hylia_test, test_hylia_pdc_cb_init)
{
	hook_notify(HOOK_INIT);

	zassert_equal(pdc_power_mgmt_register_board_callback_fake.call_count,
		      2);
	zassert_equal(
		pdc_power_mgmt_register_board_callback_fake.arg0_history[0],
		PDC_BOARD_CB_UNATTACH);
	zassert_equal(
		pdc_power_mgmt_register_board_callback_fake.arg1_history[0],
		hylia_set_unattached);
	zassert_equal(
		pdc_power_mgmt_register_board_callback_fake.arg0_history[1],
		PDC_BOARD_CB_DP_ATTENTION);
	zassert_equal(
		pdc_power_mgmt_register_board_callback_fake.arg1_history[1],
		hylia_dp_attention);
}

ZTEST(hylia_test, test_hylia_hpd_gpio_and_unattached)
{
	svdm_set_hpd_gpio(0, 1);
	svdm_set_hpd_gpio(1, 1);
	zassert_equal(svdm_get_hpd_gpio(0), 1);
	zassert_equal(svdm_get_hpd_gpio(1), 1);

	hylia_set_unattached(0);
	zassert_equal(svdm_get_hpd_gpio(0), 0);
	zassert_equal(svdm_get_hpd_gpio(1), 1);

	hylia_set_unattached(1);
	zassert_equal(svdm_get_hpd_gpio(1), 0);
}

ZTEST(hylia_test, test_hylia_dp_attention_flow)
{
	uint32_t vdo_lvl_high = VDO_DP_STATUS(0, 1, 0, 0, 0, 1, 0, 2);
	uint32_t vdo_irq_high = VDO_DP_STATUS(1, 1, 0, 0, 0, 1, 0, 2);
	uint32_t vdo_irq_low = VDO_DP_STATUS(1, 0, 0, 0, 0, 1, 0, 2);
	uint32_t vdo_lvl_low = VDO_DP_STATUS(0, 0, 0, 0, 0, 1, 0, 2);

	/* 1. Invalid IRQ=1 with LVL=0 returns early without asserting HPD */
	hylia_dp_attention(0, vdo_irq_low);
	zassert_equal(svdm_get_hpd_gpio(0), 0);
	zassert_equal(svdm_hpd_deadline[0], 0);
	zassert_true(usb_mux_set_completed(0));

	/* 2. Normal HPD high assertion updates mux synchronously and sets HPD
	 */
	hylia_dp_attention(0, vdo_lvl_high);
	zassert_equal(svdm_get_hpd_gpio(0), 1);
	zassert_true(svdm_hpd_deadline[0] > 0);
	zassert_true(usb_mux_set_completed(0));
	zassert_equal(usb_mux_get(0),
		      USB_PD_MUX_DP_ENABLED | USB_PD_MUX_HPD_LVL);

	/* 3. HPD IRQ when cur_lvl == 1 and now < svdm_hpd_deadline sleeps then
	 * pulses */
	svdm_hpd_deadline[0] = get_time().val + 1000;
	hylia_dp_attention(0, vdo_irq_high);
	zassert_equal(svdm_get_hpd_gpio(0), 1);
	zassert_equal(usb_mux_get(0), USB_PD_MUX_DP_ENABLED |
					      USB_PD_MUX_HPD_LVL |
					      USB_PD_MUX_HPD_IRQ);

	/* 4. HPD IRQ when cur_lvl == 1 and now >= svdm_hpd_deadline pulses
	 * immediately */
	svdm_hpd_deadline[0] = 0;
	hylia_dp_attention(0, vdo_irq_high);
	zassert_equal(svdm_get_hpd_gpio(0), 1);

	/* 5. HPD low deassertion clears HPD GPIO and HPD mux state */
	hylia_dp_attention(0, vdo_lvl_low);
	zassert_equal(svdm_get_hpd_gpio(0), 0);
	zassert_equal(usb_mux_get(0), USB_PD_MUX_DP_ENABLED);
}

ZTEST_SUITE(hylia_test, rauru_predicate_post_main, NULL, test_hylia_before,
	    test_hylia_before, NULL);

ZTEST(hylia_test, test_hylia_dp_alt_mode_stubs)
{
	int port = 0;
	uint32_t payload[7];

	dp_init(port);
	zassert_false(dp_is_active(port));
	zassert_true(dp_is_idle(port));
	zassert_false(dp_entry_is_done(port));
	dp_vdm_acked(port, TCPCI_MSG_SOP, 0, NULL);
	dp_vdm_naked(port, TCPCI_MSG_SOP, 0);
	zassert_equal(MSG_SETUP_ERROR, dp_setup_next_vdm(port, NULL, payload));
	zassert_false(dp_mode_entry_allowed(port));
	zassert_equal(0, dp_get_mode_vdo(port, TCPCI_MSG_SOP));
	zassert_equal(DPAM_VERSION_20,
		      dp_resolve_dpam_version(port, TCPCI_MSG_SOP));
	zassert_equal(DP_HBR3, dp_get_cable_bit_rate(port));
}
