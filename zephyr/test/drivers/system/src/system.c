/* Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "ec_commands.h"
#include "host_command.h"
#include "mock/power.h"
#include "panic.h"
#include "system.h"
#include "test/drivers/stubs.h"
#include "test/drivers/test_mocks.h"
#include "test/drivers/test_state.h"
#include "test/drivers/utils.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/ztest.h>

FAKE_VALUE_FUNC(int, system_run_image_copy_with_flags, enum ec_image, int);
FAKE_VALUE_FUNC(enum ec_image, system_get_image_copy);
FAKE_VOID_FUNC(system_disable_jump);
FAKE_VOID_FUNC(jump_to_image, uintptr_t);
FAKE_VALUE_FUNC(int, system_jumped_to_this_image);

/* System Host Commands */

ZTEST_USER(system, test_hostcmd_sysinfo)
{
	struct ec_response_sysinfo response;
	struct host_cmd_handler_args args;

	/* Simply issue the command and get the results */
	zassert_ok(ec_cmd_sysinfo(&args, &response), NULL);
	zassert_equal(args.response_size, sizeof(response), NULL);
	zassert_equal(response.reset_flags, 0, "response.reset_flags = %d",
		      response.reset_flags);
	zassert_equal(response.current_image,
		      IS_ENABLED(CONFIG_CROS_EC_RW) ? EC_IMAGE_RW : EC_IMAGE_RO,
		      "response.current_image = %d", response.current_image);
	zassert_equal(response.flags, 0, "response.flags = %d", response.flags);
}

/* System Function Testing */

static void system_before_after(void *data)
{
	ARG_UNUSED(data);
	system_clear_reset_flags(-1);

	RESET_FAKE(system_run_image_copy_with_flags);
	RESET_FAKE(system_get_image_copy);
	system_get_image_copy_fake.return_val =
		IS_ENABLED(CONFIG_CROS_EC_RW) ? EC_IMAGE_RW : EC_IMAGE_RO;
	RESET_FAKE(system_disable_jump);
	RESET_FAKE(jump_to_image);
	RESET_FAKE(system_jumped_to_this_image);
}

ZTEST(system, test_system_enter_hibernate__at_g3)
{
	set_ac_enabled(false);
	test_set_chipset_to_g3();

	/* Reset after set to g3 */
	chipset_force_shutdown_fake.call_count = 0;

	/* Arbitrary Args */
	system_enter_hibernate(0x12, 0x34);
	zassert_equal(chipset_force_shutdown_fake.call_count, 0);
	zassert_equal(system_hibernate_fake.call_count, 1);
}

ZTEST(system, test_system_enter_hibernate__ac_on)
{
	test_set_chipset_to_s0();
	set_ac_enabled(true);

	/* Arbitrary Args */
	system_enter_hibernate(0x12, 0x34);
	zassert_equal(chipset_force_shutdown_fake.call_count, 0);
}

ZTEST(system, test_system_enter_hibernate__at_s0)
{
	test_set_chipset_to_s0();
	set_ac_enabled(false);

	/* Arbitrary Args */
	system_enter_hibernate(0x12, 0x34);

	zassert_equal(chipset_force_shutdown_fake.call_count, 1);
	zassert_equal(chipset_force_shutdown_fake.arg0_val,
		      CHIPSET_SHUTDOWN_CONSOLE_CMD);
}

ZTEST(system, test_get_program_memory_addr_bad_args)
{
	zassert_equal(get_program_memory_addr(-1), INVALID_ADDR);
}

ZTEST(system, test_system_encode_save_flags)
{
	int flags_to_save = 0;
	uint32_t saved_flags = 0;
	int arbitrary_reset_flags = 1;

	/* Save all possible flags */
	flags_to_save = -1;

	/* Clear all reset flags and set them arbitrarily */
	system_set_reset_flags(arbitrary_reset_flags);

	system_encode_save_flags(flags_to_save, &saved_flags);

	/* Verify all non-mutually exclusive flags */
	zassert_equal(1, saved_flags & system_get_reset_flags(), NULL);
	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_AP_OFF, NULL);
	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_STAY_IN_RO, NULL);
	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_AP_WATCHDOG, NULL);
}

ZTEST(system, test_system_encode_save_flags_mutually_exclusive_reset_flags)
{
	int flags_to_save = 0;
	uint32_t saved_flags = 0;

	/* Verify reset hard takes precedence over hibernate/soft */
	flags_to_save = SYSTEM_RESET_HARD | SYSTEM_RESET_HIBERNATE;

	system_encode_save_flags(flags_to_save, &saved_flags);

	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_HARD, NULL);
	zassert_equal(0, saved_flags & EC_RESET_FLAG_HIBERNATE, NULL);
	zassert_equal(0, saved_flags & EC_RESET_FLAG_SOFT, NULL);

	/* Verify reset hibernate takes precedence over soft */
	flags_to_save = SYSTEM_RESET_HIBERNATE;

	system_encode_save_flags(flags_to_save, &saved_flags);

	zassert_equal(0, saved_flags & EC_RESET_FLAG_HARD, NULL);
	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_HIBERNATE, NULL);
	zassert_equal(0, saved_flags & EC_RESET_FLAG_SOFT, NULL);

	/* Verify reset soft is always saved given no other flags */
	flags_to_save = 0;

	system_encode_save_flags(flags_to_save, &saved_flags);

	zassert_equal(0, saved_flags & EC_RESET_FLAG_HARD, NULL);
	zassert_equal(0, saved_flags & EC_RESET_FLAG_HIBERNATE, NULL);
	zassert_not_equal(0, saved_flags & EC_RESET_FLAG_SOFT, NULL);
}

ZTEST(system, test_stringify_reset_flags)
{
	char buf[MAX_RESET_FLAG_STRLEN];

	/* 0 flags -> "unknown" */
	stringify_reset_flags(0, buf, sizeof(buf));
	zassert_str_equal(buf, "unknown");

	/* Single known flag */
	stringify_reset_flags(EC_RESET_FLAG_POWER_ON, buf, sizeof(buf));
	zassert_str_equal(buf, "power-on");

	/* Multiple known flags */
	stringify_reset_flags(EC_RESET_FLAG_RESET_PIN | EC_RESET_FLAG_HARD, buf,
			      sizeof(buf));
	zassert_str_equal(buf, "reset-pin hard");

	/* Flag with no description only */
	stringify_reset_flags(BIT(31), buf, sizeof(buf));
	zassert_str_equal(buf, "no-desc");

	/* Known flag and flag with no description */
	stringify_reset_flags(EC_RESET_FLAG_SOFT | BIT(31), buf, sizeof(buf));
	zassert_str_equal(buf, "soft no-desc");

	/* Print reset flags to console */
	system_set_reset_flags(EC_RESET_FLAG_POWER_ON);
	system_print_reset_flags();
}

ZTEST(system, test_stringify_reset_flags_overflow)
{
	/*
	 * Allocate a buffer with guard bytes before and after the target buffer
	 * to detect any out-of-bounds writes when all 32 reset flags are set
	 * (which would require ~188 bytes if untruncated, exceeding
	 * MAX_RESET_FLAG_STRLEN = 100).
	 */
	struct {
		char pre_guard[16];
		char buf[MAX_RESET_FLAG_STRLEN];
		char post_guard[16];
	} stack_frame;

	memset(&stack_frame, 0xA5, sizeof(stack_frame));

	stringify_reset_flags(UINT32_MAX, stack_frame.buf,
			      sizeof(stack_frame.buf));

	for (size_t i = 0; i < sizeof(stack_frame.pre_guard); i++) {
		zassert_equal((uint8_t)stack_frame.pre_guard[i], 0xA5,
			      "Pre-guard corrupted at index %zu", i);
	}
	for (size_t i = 0; i < sizeof(stack_frame.post_guard); i++) {
		zassert_equal((uint8_t)stack_frame.post_guard[i], 0xA5,
			      "Post-guard corrupted at index %zu", i);
	}
	zassert_equal(stack_frame.buf[sizeof(stack_frame.buf) - 1], '\0',
		      "Output buffer is not null-terminated");
	zassert_equal(strlen(stack_frame.buf), sizeof(stack_frame.buf) - 1);

	/* Also exercise system_print_reset_flags() and system_print_banner()
	 * with all non-USB-resume flags set to ensure their stack-allocated
	 * buffers do not overflow.
	 */
	system_set_reset_flags(UINT32_MAX & ~EC_RESET_FLAG_USB_RESUME);
	system_print_reset_flags();
	system_print_banner();

	/* Zero or negative buffer length should be a safe no-op */
	memset(&stack_frame, 0xA5, sizeof(stack_frame));
	stringify_reset_flags(UINT32_MAX, stack_frame.buf, 0);
	zassert_equal((uint8_t)stack_frame.buf[0], 0xA5);
}

ZTEST(system, test_system_print_banner)
{
	/* Normal reboot banner */
	system_jumped_to_this_image_fake.return_val = 0;
	system_set_reset_flags(EC_RESET_FLAG_POWER_ON);
	system_print_banner();

	/* Sysjump banner */
	system_jumped_to_this_image_fake.return_val = 1;
	system_set_reset_flags(EC_RESET_FLAG_SYSJUMP);
	system_print_banner();

	/* USB resume suppresses banner */
	system_set_reset_flags(EC_RESET_FLAG_USB_RESUME);
	system_print_banner();
}

/* System Console Commands */

ZTEST_USER(system, test_console_cmd_sysjump__no_args)
{
	const struct shell *shell_zephyr = get_ec_shell();

	/* No output from no-arg commands, so just test failure */
	zassert_equal(shell_execute_cmd(shell_zephyr, "sysjump"),
		      EC_ERROR_PARAM_COUNT);
}

ZTEST_USER(system, test_console_cmd_sysjump__RO)
{
	const struct shell *shell_zephyr = get_ec_shell();

	/* Since we start at RO this acts as NOOP */
	zassert_ok(shell_execute_cmd(shell_zephyr, "sysjump RO"));

	zassert_equal(system_run_image_copy_with_flags_fake.call_count, 1);
	zassert_equal(system_run_image_copy_with_flags_fake.arg0_val,
		      EC_IMAGE_RO);
	zassert_equal(system_run_image_copy_with_flags_fake.arg1_val,
		      EC_RESET_FLAG_STAY_IN_RO);
}

ZTEST_USER(system, test_console_cmd_sysjump__RW)
{
	const struct shell *shell_zephyr = get_ec_shell();

	zassert_ok(shell_execute_cmd(shell_zephyr, "sysjump RW"));
	zassert_equal(system_run_image_copy_with_flags_fake.call_count, 1);
	zassert_equal(system_run_image_copy_with_flags_fake.arg0_val,
		      EC_IMAGE_RW);
	zassert_equal(system_run_image_copy_with_flags_fake.arg1_val, 0);
}

ZTEST_USER(system, test_console_cmd_sysjump__A)
{
	const struct shell *shell_zephyr = get_ec_shell();

	zassert_ok(shell_execute_cmd(shell_zephyr, "sysjump A"));
	zassert_equal(system_run_image_copy_with_flags_fake.call_count, 1);
	zassert_equal(system_run_image_copy_with_flags_fake.arg0_val,
		      EC_IMAGE_RW);
	zassert_equal(system_run_image_copy_with_flags_fake.arg1_val, 0);
}

ZTEST_USER(system, test_console_cmd_sysjump__B)
{
	const struct shell *shell_zephyr = get_ec_shell();

	/* Downstream Zephyr isn't setup with A/B images */
	zassert_equal(shell_execute_cmd(shell_zephyr, "sysjump B"),
		      EC_ERROR_PARAM1);
	zassert_equal(system_run_image_copy_with_flags_fake.call_count, 0);
}

ZTEST_USER(system, test_console_cmd_sysjump__disable)
{
	const struct shell *shell_zephyr = get_ec_shell();

	zassert_ok(shell_execute_cmd(shell_zephyr, "sysjump disable"));
	zassert_equal(system_run_image_copy_with_flags_fake.call_count, 0);
	zassert_equal(system_disable_jump_fake.call_count, 1);
}

ZTEST_USER(system, test_console_cmd_sysjump__addr_while_sys_locked)
{
	const struct shell *shell_zephyr = get_ec_shell();

	system_is_locked_fake.return_val = true;

	/* No output to test against */
	zassert_equal(shell_execute_cmd(shell_zephyr, "sysjump 0x1234"),
		      EC_ERROR_ACCESS_DENIED);
	zassert_equal(system_is_locked_fake.call_count, 1);
}

ZTEST_USER(system, test_console_cmd_sysjump__addr)
{
	const struct shell *shell_zephyr = get_ec_shell();

	/* No output to test against */
	zassert_ok(shell_execute_cmd(shell_zephyr, "sysjump 0x1234"));
	zassert_equal(system_is_locked_fake.call_count, 1);
	zassert_equal(jump_to_image_fake.call_count, 1);
	zassert_equal(jump_to_image_fake.arg0_val, 0x1234);
}

ZTEST_USER(system, test_console_cmd_sysjump__addr_bad_number)
{
	const struct shell *shell_zephyr = get_ec_shell();

	/* No output to test against */
	zassert_equal(shell_execute_cmd(shell_zephyr, "sysjump O___o"),
		      EC_ERROR_PARAM1);
	zassert_equal(system_is_locked_fake.call_count, 1);
	zassert_equal(jump_to_image_fake.call_count, 0);
}

ZTEST_SUITE(system, drivers_predicate_post_main, NULL, system_before_after,
	    system_before_after, NULL);
