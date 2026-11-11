/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"
#include "hooks.h"
#include "led_common.h"
#include "led_onoff_states.h"
#include "lid_switch.h"
#include "util.h"

#include <zephyr/drivers/pwm.h>

#include <ap_power/ap_power.h>

static void power_led_update(void)
{
	bool lid_closed_in_suspend =
		!lid_is_open() && chipset_in_state(CHIPSET_STATE_ANY_SUSPEND);

	if (lid_closed_in_suspend) {
		/* Stop the policy engine and force the LED off */
		led_auto_control(EC_LED_ID_POWER_LED, 0);
		gpio_pin_set_dt(GPIO_DT_FROM_NODELABEL(gpio_led_blue), false);
	} else {
		led_auto_control(EC_LED_ID_POWER_LED, 1);
	}
}
static void power_led_change(void)
{
	power_led_update();
}
DECLARE_HOOK(HOOK_LID_CHANGE, power_led_change, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, power_led_change, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_RESUME, power_led_change, HOOK_PRIO_DEFAULT);
