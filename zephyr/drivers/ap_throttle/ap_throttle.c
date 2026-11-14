/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#define DT_DRV_COMPAT cros_ec_ap_throttle

#include "battery.h"
#include "chipset.h"
#include "hooks.h"
#include "throttle_ap.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ap_throttle, LOG_LEVEL_INF);

BUILD_ASSERT(
	DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	"Exactly one 'cros-ec,ap-throttle' compatible node must be present");

struct ap_throttle_config {
	struct gpio_dt_spec throttle_gpio;
	bool ac_only_boot_throttle;
	bool g3s5_boot_throttle;
	bool low_battery_boot_throttle;
};

struct ap_throttle_data {
	bool boot_throttle; /* G3S5 pre-init boot throttle */
	bool ac_only_boot; /* Booted without battery */
	bool low_bat_boot; /* Booted with battery < MIN_BAT_PCT for charging
			      (retained in S0) */
	bool thermal_throttle; /* Commanded by chipset_throttle_cpu() */
};

static struct ap_throttle_data throttle_data;

static const struct ap_throttle_config throttle_cfg = {
	.throttle_gpio = GPIO_DT_SPEC_GET(DT_DRV_INST(0), throttle_gpios),
	.ac_only_boot_throttle = DT_PROP(DT_DRV_INST(0), ac_only_boot_throttle),
	.g3s5_boot_throttle = DT_PROP(DT_DRV_INST(0), g3s5_boot_throttle),
	.low_battery_boot_throttle =
		DT_PROP(DT_DRV_INST(0), low_battery_boot_throttle),
};

void chipset_throttle_cpu(int throttle)
{
	/*
	 * Do not allow changing throttle state during AC-only boot to ensure
	 * PROCHOT remains asserted to limit SoC power consumption.
	 */
	if (!chipset_in_state(CHIPSET_STATE_ON) || throttle_data.ac_only_boot ||
	    throttle_data.low_bat_boot)
		return;

	gpio_pin_set_dt(&throttle_cfg.throttle_gpio, throttle ? 1 : 0);

	if (throttle) {
		if (!throttle_data.thermal_throttle) {
			throttle_data.thermal_throttle = true;
			LOG_INF("Asserting AP throttle");
		}
	} else {
		if (throttle_data.thermal_throttle) {
			throttle_data.thermal_throttle = false;
			LOG_INF("AP thermal recovered; deasserting throttle");
		}
	}
}

static bool ap_throttle_is_low_battery(void)
{
	int percent = 0;

	if (!throttle_cfg.low_battery_boot_throttle)
		return false;

	if (battery_is_present() != BP_YES)
		return false;

	int ret;

	ret = battery_state_of_charge_abs(&percent);
	if (ret != EC_SUCCESS) {
		LOG_WRN("Failed to read battery SoC (%d); "
			"retaining AP throttle in S0",
			ret);
		return true;
	}

	if (percent < CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON) {
		LOG_INF("Low-battery boot detected (State-of-Charge: %d%% < %d%%); "
			"retaining AP throttle in S0",
			percent,
			CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON);
		return true;
	}

	return false;
}

static void ap_throttle_chipset_pre_init(void)
{
	throttle_data.ac_only_boot = false;
	throttle_data.low_bat_boot = false;
	throttle_data.boot_throttle = false;

	/*
	 * AC-only boot (No battery present):
	 * If booting without a battery on an adapter that cannot support peak
	 * power alone, latch ac_only_boot so the throttle remains asserted in
	 * S0.
	 */
	if (throttle_cfg.ac_only_boot_throttle &&
	    battery_is_present() != BP_YES) {
		LOG_INF("AC-only boot detected; retaining AP throttle in S0");
		gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 1);
		throttle_data.ac_only_boot = true;
	}

	/*
	 * Low-battery boot (<
	 * CONFIG_PLATFORM_EC_CHARGER_MIN_BAT_PCT_FOR_POWER_ON): Battery is
	 * present, but charge is insufficient for full OS boot. AP powers on
	 * strictly for charging (even in S0); retain "AP throttle" asserted.
	 */
	if (ap_throttle_is_low_battery()) {
		throttle_data.low_bat_boot = true;
		gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 1);
	}

	/*
	 * Off-mode charging boot:
	 * Assert "AP throttle" when booting from G3 to S5 if g3s5-boot-throttle
	 * is enabled. This ensures the SoC is pre-throttled before
	 * power_on_seq() runs for off-mode charging.
	 */
	if (throttle_cfg.g3s5_boot_throttle) {
		LOG_INF("G3S5 boot detected; asserting AP throttle for off-mode charging");
		throttle_data.boot_throttle = true;
		gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 1);
	}
}
DECLARE_HOOK(HOOK_CHIPSET_PRE_INIT, ap_throttle_chipset_pre_init,
	     HOOK_PRIO_DEFAULT);

static void ap_throttle_chipset_resume(void)
{
	/*
	 * If AC-only boot was latched, retain "AP throttle" asserted in S0.
	 */
	if (throttle_data.ac_only_boot || throttle_data.low_bat_boot) {
		gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 1);
		LOG_INF("Retaining AP throttle in S0 (%s)",
			throttle_data.ac_only_boot ? "AC-only" :
						     "low-battery charging");
		return;
	}

	/*
	 * Transitioning to S0 (normal OS boot) with battery present:
	 * Deassert the boot-time throttle, provided no thermal throttle is
	 * active.
	 */
	if (throttle_data.boot_throttle) {
		throttle_data.boot_throttle = false;
		if (!throttle_data.thermal_throttle) {
			gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 0);
			LOG_INF("Entered S0 with battery; deasserted AP throttle");
		} else {
			LOG_INF("Entered S0 with battery; throttle retained due to thermals");
		}
	}
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, ap_throttle_chipset_resume,
	     HOOK_PRIO_DEFAULT);

static void ap_throttle_chipset_hard_off(void)
{
	throttle_data.boot_throttle = false;
	throttle_data.ac_only_boot = false;
	throttle_data.low_bat_boot = false;
	throttle_data.thermal_throttle = false;
	/*
	 * Deassert "AP throttle" in G3 (hard off) so no leakage current is
	 * drawn through pull-up resistors while all system rails are unpowered.
	 */
	LOG_INF("Entering G3; deasserting AP throttle");
	gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_HARD_OFF, ap_throttle_chipset_hard_off,
	     HOOK_PRIO_DEFAULT);

static int ap_throttle_init(const struct device *dev)
{
	const struct ap_throttle_config *cfg = dev->config;
	int ret;

	if (!gpio_is_ready_dt(&cfg->throttle_gpio)) {
		LOG_ERR_DEVICE_NOT_READY(cfg->throttle_gpio.port);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->throttle_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Throttle GPIO configuration failed: %d", ret);
		return ret;
	}

	return 0;
}

DEVICE_DT_INST_DEFINE(0, ap_throttle_init, NULL, &throttle_data, &throttle_cfg,
		      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);
