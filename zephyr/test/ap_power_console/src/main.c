/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"
#include "console.h"

#include <string.h>

#include <zephyr/fff.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

FAKE_VALUE_FUNC(int, chipset_in_state, int);
FAKE_VOID_FUNC(chipset_power_on);
FAKE_VOID_FUNC(chipset_force_shutdown, enum chipset_shutdown_reason);

static int active_state_mask;

static int custom_chipset_in_state(int state_mask)
{
	return (state_mask & active_state_mask) != 0;
}

static void *ap_power_console_setup(void)
{
	k_sleep(K_MSEC(1));
	zassert_true(shell_ready(get_ec_shell()));

	return NULL;
}

static void ap_power_console_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(chipset_in_state);
	RESET_FAKE(chipset_power_on);
	RESET_FAKE(chipset_force_shutdown);

	active_state_mask = 0;
	chipset_in_state_fake.custom_fake = custom_chipset_in_state;
	shell_backend_dummy_clear_output(get_ec_shell());
}

ZTEST(ap_power_console, test_query_state_off)
{
	const char *outbuffer;
	size_t buffer_size;

	active_state_mask = CHIPSET_STATE_HARD_OFF;

	zassert_ok(shell_execute_cmd(get_ec_shell(), "power"));
	outbuffer =
		shell_backend_dummy_get_output(get_ec_shell(), &buffer_size);
	zassert_not_null(strstr(outbuffer, "off"));
}

ZTEST(ap_power_console, test_query_state_on)
{
	const char *outbuffer;
	size_t buffer_size;

	active_state_mask = CHIPSET_STATE_ON;

	zassert_ok(shell_execute_cmd(get_ec_shell(), "power"));
	outbuffer =
		shell_backend_dummy_get_output(get_ec_shell(), &buffer_size);
	zassert_not_null(strstr(outbuffer, "on"));
}

ZTEST(ap_power_console, test_query_state_unknown)
{
	const char *outbuffer;
	size_t buffer_size;

	active_state_mask = CHIPSET_STATE_SUSPEND;

	zassert_ok(shell_execute_cmd(get_ec_shell(), "power"));
	outbuffer =
		shell_backend_dummy_get_output(get_ec_shell(), &buffer_size);
	zassert_not_null(strstr(outbuffer, "unknown"));
}

ZTEST(ap_power_console, test_power_on)
{
	const char *outbuffer;
	size_t buffer_size;

	zassert_ok(shell_execute_cmd(get_ec_shell(), "power on"));
	zassert_equal(chipset_power_on_fake.call_count, 1);
	zassert_equal(chipset_force_shutdown_fake.call_count, 0);

	outbuffer =
		shell_backend_dummy_get_output(get_ec_shell(), &buffer_size);
	zassert_not_null(strstr(outbuffer, "Requesting power on"));
}

ZTEST(ap_power_console, test_power_off)
{
	const char *outbuffer;
	size_t buffer_size;

	zassert_ok(shell_execute_cmd(get_ec_shell(), "power off"));
	zassert_equal(chipset_power_on_fake.call_count, 0);
	zassert_equal(chipset_force_shutdown_fake.call_count, 1);
	zassert_equal(chipset_force_shutdown_fake.arg0_val,
		      CHIPSET_SHUTDOWN_CONSOLE_CMD);

	outbuffer =
		shell_backend_dummy_get_output(get_ec_shell(), &buffer_size);
	zassert_not_null(strstr(outbuffer, "Requesting power off"));
}

ZTEST(ap_power_console, test_power_invalid_arg)
{
	zassert_equal(shell_execute_cmd(get_ec_shell(), "power invalid"),
		      -EINVAL);
	zassert_equal(chipset_power_on_fake.call_count, 0);
	zassert_equal(chipset_force_shutdown_fake.call_count, 0);
}

ZTEST_SUITE(ap_power_console, NULL, ap_power_console_setup,
	    ap_power_console_before, NULL, NULL);
