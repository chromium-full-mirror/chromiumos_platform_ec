/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "battery.h"
#include "chipset.h"
#include "hooks.h"
#include "throttle_ap.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define AP_THROTTLE_NODE DT_NODELABEL(ap_throttle)

/*
 * Because throttle-gpios is configured as GPIO_ACTIVE_LOW in
 * native_sim.overlay, gpio_emul_output_get() returns the raw physical pin
 * level:
 *   0 = physical LOW  (logical 1, throttle asserted)
 *   1 = physical HIGH (logical 0, throttle deasserted)
 */
#define THROTTLE_RAW_ASSERTED 0
#define THROTTLE_RAW_DEASSERTED 1

static const struct device *const ap_throttle_dev =
	DEVICE_DT_GET(AP_THROTTLE_NODE);
static const struct gpio_dt_spec throttle_gpio =
	GPIO_DT_SPEC_GET(AP_THROTTLE_NODE, throttle_gpios);

FAKE_VALUE_FUNC(int, chipset_in_state, int);
FAKE_VALUE_FUNC(enum battery_present, battery_is_present);
FAKE_VALUE_FUNC(int, battery_state_of_charge_abs, int *);

static int chipset_in_state_on(int state_mask)
{
	return (state_mask & CHIPSET_STATE_ON) != 0;
}

static int chipset_in_state_off(int state_mask)
{
	ARG_UNUSED(state_mask);
	return 0;
}

static int fake_battery_soc_normal(int *percent)
{
	*percent = 50; /* Normal battery charge (> 4%) */
	return EC_SUCCESS;
}

static int fake_battery_soc_low(int *percent)
{
	*percent = 1; /* Low battery charge (<= 4%) */
	return EC_SUCCESS;
}

static void ap_throttle_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(chipset_in_state);
	RESET_FAKE(battery_is_present);
	RESET_FAKE(battery_state_of_charge_abs);

	chipset_in_state_fake.custom_fake = chipset_in_state_on;
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;

	/* Reset driver state and deassert GPIO before each test */
	hook_notify(HOOK_CHIPSET_HARD_OFF);
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

	/* Deassert throttle (and test idempotent second call) */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_hard_off_clears_thermal_throttle)
{
	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Entering G3 must clear thermal_throttle and deassert the pin */
	hook_notify(HOOK_CHIPSET_HARD_OFF);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

#if DT_PROP(AP_THROTTLE_NODE, g3s5_boot_throttle)
ZTEST(ap_throttle, test_g3s5_boot_throttle_deasserted_on_resume)
{
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Entering S0 with battery and no thermal throttle deasserts pin */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Subsequent S3->S0 resume must not clear active thermal throttle */
	chipset_throttle_cpu(1);
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);
}

ZTEST(ap_throttle, test_g3s5_boot_throttle_retained_when_thermally_throttled)
{
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Thermal throttle activates before/during transition to S0 */
	chipset_throttle_cpu(1);
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Once thermal throttle recovers, pin deasserts */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_g3s5_boot_throttle_cleared_on_hard_off)
{
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Returning from S5 directly to G3 must deassert throttle */
	hook_notify(HOOK_CHIPSET_HARD_OFF);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#else /* !DT_PROP(AP_THROTTLE_NODE, g3s5_boot_throttle) */
ZTEST(ap_throttle, test_g3s5_boot_throttle_disabled)
{
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#endif /* DT_PROP(AP_THROTTLE_NODE, g3s5_boot_throttle) */

#if DT_PROP(AP_THROTTLE_NODE, ac_only_boot_throttle)
ZTEST(ap_throttle, test_ac_only_boot_throttle)
{
	/* Simulate AC-only boot (no battery present) */
	battery_is_present_fake.return_val = BP_NO;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Entering S0 must retain throttle during AC-only boot */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* chipset_throttle_cpu(0) must not clear throttle during AC-only boot
	 */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Hard off (G3) must deassert throttle and clear AC-only lock */
	hook_notify(HOOK_CHIPSET_HARD_OFF);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Subsequent boot to S0 with battery leaves throttle deasserted */
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#else /* !DT_PROP(AP_THROTTLE_NODE, ac_only_boot_throttle) */
ZTEST(ap_throttle, test_ac_only_boot_throttle_disabled)
{
	battery_is_present_fake.return_val = BP_NO;
	hook_notify(HOOK_CHIPSET_PRE_INIT);

#if !DT_PROP(AP_THROTTLE_NODE, g3s5_boot_throttle)
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
#endif

	/* Entering S0 without battery does not retain throttle when disabled */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Normal thermal throttling still works in S0 without battery */
	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#endif /* DT_PROP(AP_THROTTLE_NODE, ac_only_boot_throttle) */

#if DT_PROP(AP_THROTTLE_NODE, low_battery_boot_throttle)
ZTEST(ap_throttle, test_low_battery_boot_throttle)
{
	/* Boot with battery present but SoC <
	 * CONFIG_BATT_HOST_SHUTDOWN_PERCENTAGE
	 */
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_low;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Reaching S0 retains throttle because AP boots strictly for charging
	 */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* chipset_throttle_cpu(0) must not clear throttle during low-battery
	 * boot
	 */
	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	/* Hard off (G3) clears low-battery latch and deasserts pin */
	hook_notify(HOOK_CHIPSET_HARD_OFF);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Subsequent boot with normal SoC leaves throttle deasserted in S0 */
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_normal;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}

ZTEST(ap_throttle, test_low_battery_soc_read_error_retains_throttle)
{
	/* Failed SoC read with battery present should conservatively throttle
	 */
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = NULL;
	battery_state_of_charge_abs_fake.return_val = EC_ERROR_UNKNOWN;
	hook_notify(HOOK_CHIPSET_PRE_INIT);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	hook_notify(HOOK_CHIPSET_HARD_OFF);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#else /* !DT_PROP(AP_THROTTLE_NODE, low_battery_boot_throttle) */
ZTEST(ap_throttle, test_low_battery_boot_throttle_disabled)
{
	battery_is_present_fake.return_val = BP_YES;
	battery_state_of_charge_abs_fake.custom_fake = fake_battery_soc_low;
	hook_notify(HOOK_CHIPSET_PRE_INIT);

#if !DT_PROP(AP_THROTTLE_NODE, g3s5_boot_throttle)
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
#endif

	/* Entering S0 with low battery does not retain throttle when disabled
	 */
	hook_notify(HOOK_CHIPSET_RESUME);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);

	/* Normal thermal throttling still works in S0 */
	chipset_throttle_cpu(1);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_ASSERTED);

	chipset_throttle_cpu(0);
	zassert_equal(gpio_emul_output_get(throttle_gpio.port,
					   throttle_gpio.pin),
		      THROTTLE_RAW_DEASSERTED);
}
#endif /* DT_PROP(AP_THROTTLE_NODE, low_battery_boot_throttle) */

ZTEST_SUITE(ap_throttle, NULL, NULL, ap_throttle_before, NULL, NULL);
