/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(ap_power_console, LOG_LEVEL_INF);

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	int err = 0;
	bool enable;

	if (argc < 2) {
		const char *state = "unknown";

		if (chipset_in_state(CHIPSET_STATE_ANY_OFF)) {
			state = "off";
		} else if (chipset_in_state(CHIPSET_STATE_ON)) {
			state = "on";
		}

		shell_print(sh, "%s", state);
		return 0;
	}

	enable = shell_strtobool(argv[1], 0, &err);
	if (err != 0) {
		shell_error(sh, "Invalid argument: %s", argv[1]);
		return -EINVAL;
	}

	LOG_INF("Requesting power %s", enable ? "on" : "off");
	shell_print(sh, "Requesting power %s", enable ? "on" : "off");

	if (enable) {
		chipset_power_on();
	} else {
		chipset_force_shutdown(CHIPSET_SHUTDOWN_CONSOLE_CMD);
	}

	return 0;
}

SHELL_CMD_ARG_REGISTER(power, NULL, "Turn AP power on/off [on|off]", cmd_power,
		       1, 1);
