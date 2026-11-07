/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "chipset.h"
#include "hooks.h"
#include "throttle_ap.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define AP_THROTTLE_NODE DT_NODELABEL(ap_throttle)

/*
 * Because throttle-gpios is configured as GPIO_ACTIVE_LOW in
 * native_sim.overlay, gpio_emul_output_get() returns the raw physical pin
 * level: 0 = physical LOW  (logical 1, throttle asserted) 1 = physical HIGH
 * (logical 0, throttle deasserted)
 */
#define THROTTLE_RAW_ASSERTED 0
#define THROTTLE_RAW_DEASSERTED 1

static const struct device *const ap_throttle_dev =
	DEVICE_DT_GET(AP_THROTTLE_NODE);
static const struct gpio_dt_spec throttle_gpio =
	GPIO_DT_SPEC_GET(AP_THROTTLE_NODE, throttle_gpios);

FAKE_VALUE_FUNC(int, chipset_in_state, int);
FAKE_VALUE_FUNC(enum battery_present, battery_is_present);

static int chipset_in_state_on(int state_mask)
{
	return (state_mask & CHIPSET_STATE_ON) != 0;
}

static int chipset_in_state_off(int state_mask)
{
	ARG_UNUSED(state_mask);
	return 0;
}

static void ap_throttle_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(chipset_in_state);
	RESET_FAKE(battery_is_present);

	chipset_in_state_fake.custom_fake = chipset_in_state_on;
	battery_is_present_fake.return_val = BP_YES;

	/* Reset driver state and deassert GPIO before each test */
	hook_notify(HOOK_CHIPSET_SHUTDOWN);
}

ZTEST(ap_throttle, test_init)
{
	zassert_true(device_is_ready(ap_throttle_dev));
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_throttle_ignored_when_chipset_off)
{
	chipset_in_state_fake.custom_fake = chipset_in_state_off;

	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_throttle_assert_and_deassert)
{
	/* Assert throttle (and test idempotent second call) */
	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Deassert throttle before timeout (and test idempotent second call) */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_ac_only_boot_throttle)
{
	/* Simulate AC-only boot (no battery present) */
	battery_is_present_fake.return_val = BP_NO;
	hook_notify(HOOK_CHIPSET_PRE_INIT);

	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* chipset_throttle_cpu(0) must not clear throttle during AC-only boot
	 */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Shutdown must deassert throttle and clear AC-only lock */
	hook_notify(HOOK_CHIPSET_SHUTDOWN);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Booting with battery present leaves throttle deasserted */
	battery_is_present_fake.return_val = BP_YES;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST_SUITE(ap_throttle, NULL, NULL, ap_throttle_before, NULL, NULL);
