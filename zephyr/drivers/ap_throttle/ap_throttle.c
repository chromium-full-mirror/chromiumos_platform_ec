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
};

struct ap_throttle_data {
	bool ac_only_boot;
	bool throttle_active;
};

static struct ap_throttle_data throttle_data;

static const struct ap_throttle_config throttle_cfg = {
	.throttle_gpio = GPIO_DT_SPEC_GET(DT_DRV_INST(0), throttle_gpios),
	.ac_only_boot_throttle = DT_PROP(DT_DRV_INST(0), ac_only_boot_throttle),
};

void chipset_throttle_cpu(int throttle)
{
	/*
	 * Do not allow changing throttle state during AC-only boot to ensure
	 * PROCHOT remains asserted to limit SoC power consumption.
	 */
	if (!chipset_in_state(CHIPSET_STATE_ON) || throttle_data.ac_only_boot)
		return;

	gpio_pin_set_dt(&throttle_cfg.throttle_gpio, throttle ? 1 : 0);

	if (throttle) {
		if (!throttle_data.throttle_active) {
			throttle_data.throttle_active = true;
			LOG_INF("Asserting AP throttle");
		}
	} else {
		if (throttle_data.throttle_active) {
			throttle_data.throttle_active = false;
			LOG_INF("AP thermal recovered; deasserting throttle");
		}
	}
}

static void ap_throttle_chipset_pre_init(void)
{
	throttle_data.ac_only_boot = false;

	if (throttle_cfg.ac_only_boot_throttle &&
	    battery_is_present() != BP_YES) {
		LOG_INF("AC-only boot detected; asserting AP throttle");
		gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 1);
		throttle_data.ac_only_boot = true;
	}
}
DECLARE_HOOK(HOOK_CHIPSET_PRE_INIT, ap_throttle_chipset_pre_init,
	     HOOK_PRIO_DEFAULT);

static void ap_throttle_chipset_shutdown(void)
{
	throttle_data.throttle_active = false;
	throttle_data.ac_only_boot = false;

	/* Deassert throttle GPIO on shutdown */
	gpio_pin_set_dt(&throttle_cfg.throttle_gpio, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, ap_throttle_chipset_shutdown,
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
