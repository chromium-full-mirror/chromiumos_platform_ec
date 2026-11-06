/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "console.h"
#include "ec_commands.h"
#include "test/drivers/test_state.h"

#include <zephyr/shell/shell.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(console_cmd_i2c_portmap, drivers_predicate_post_main, NULL, NULL,
	    NULL, NULL);

ZTEST_USER(console_cmd_i2c_portmap, test_too_many_args)
{
	int rv = shell_execute_cmd(get_ec_shell(), "i2c_portmap arg1");

	zassert_equal(rv, -EINVAL, "Expected %d, but got %d", -EINVAL, rv);
}

ZTEST_USER(console_cmd_i2c_portmap, test_get_i2c_portmap)
{
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2c_portmap"), NULL);
}

ZTEST_USER(console_cmd_i2c_portmap, test_i2ctrace)
{
	/* Invalid command / argument counts / values */
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace list 0"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace enable"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace enable bad"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(),
					"i2ctrace enable 0 bad"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(),
					"i2ctrace enable 0 0x10 bad"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(),
					"i2ctrace enable 99 0x10"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(),
					"i2ctrace enable 0 0x20 0x10"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace disable 99"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2ctrace invalid 0"),
		      -EINVAL);

	/* Enable single address and range, then list */
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x10"));
	zassert_ok(shell_execute_cmd(get_ec_shell(),
				     "i2ctrace enable 0 0x20 0x25"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace list"));

	/* Subset, extend both directions, extend below, extend above */
	zassert_ok(shell_execute_cmd(get_ec_shell(),
				     "i2ctrace enable 0 0x21 0x22"));
	zassert_ok(shell_execute_cmd(get_ec_shell(),
				     "i2ctrace enable 0 0x1f 0x26"));
	zassert_ok(shell_execute_cmd(get_ec_shell(),
				     "i2ctrace enable 0 0x1d 0x1e"));
	zassert_ok(shell_execute_cmd(get_ec_shell(),
				     "i2ctrace enable 0 0x27 0x28"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace list"));

	/* Fill all 8 entries to hit -ENOMEM, then disable all */
	for (int i = 0; i < 8; i++)
		zassert_ok(shell_execute_cmd(get_ec_shell(),
					     "i2ctrace disable 0") ||
			   shell_execute_cmd(get_ec_shell(),
					     "i2ctrace disable 1"));

	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x10"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x12"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x14"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x16"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x18"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x1a"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x1c"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2ctrace enable 0 0x1e"));
	zassert_equal(shell_execute_cmd(get_ec_shell(),
					"i2ctrace enable 0 0x30"),
		      -ENOMEM);

	for (int i = 0; i < 8; i++) {
		char cmd[32];

		snprintf(cmd, sizeof(cmd), "i2ctrace disable %d", i);
		zassert_ok(shell_execute_cmd(get_ec_shell(), cmd));
	}

	/* Exercise i2cprotect console command */
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2cprotect"));
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2cprotect bad"),
		      -EINVAL);
	zassert_equal(shell_execute_cmd(get_ec_shell(), "i2cprotect 99"),
		      -EINVAL);
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2cprotect 0"));
	zassert_ok(shell_execute_cmd(get_ec_shell(), "i2cprotect"));
}
