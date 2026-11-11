/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "keyboard_protocol.h"

#include <zephyr/fff.h>
#include <zephyr/input/input_keymap.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <dt-bindings/kbd.h>

DEFINE_FFF_GLOBALS;

FAKE_VOID_FUNC(keyboard_state_changed_process, int, int, int, int);

#define FN_KEYS_NODE DT_NODELABEL(fn_keys)

#define FN_ROW KBD_RC_ROW(DT_PROP_BY_IDX(FN_KEYS_NODE, fn_rc, 0))
#define FN_COL KBD_RC_COL(DT_PROP_BY_IDX(FN_KEYS_NODE, fn_rc, 0))

#define FN2_ROW KBD_RC_ROW(DT_PROP_BY_IDX(FN_KEYS_NODE, fn_rc, 1))
#define FN2_COL KBD_RC_COL(DT_PROP_BY_IDX(FN_KEYS_NODE, fn_rc, 1))

#define F1_ROW MATRIX_ROW(DT_PROP_BY_IDX(FN_KEYS_NODE, keymap, 0))
#define F1_COL MATRIX_COL(DT_PROP_BY_IDX(FN_KEYS_NODE, keymap, 0))
#define F1_CODE (DT_PROP_BY_IDX(FN_KEYS_NODE, keymap, 0) & 0xffff)

void assert_last_state(int row, int col, int is_pressed, int override)
{
	zassert_equal(keyboard_state_changed_process_fake.call_count, 1);
	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0], row);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0], col);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0],
		      is_pressed);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0],
		      override);

	RESET_FAKE(keyboard_state_changed_process);
}

ZTEST(keyboard_fn, test_unrelated_key)
{
	keyboard_state_changed(0, 11, 1);
	assert_last_state(0, 11, 1, -1);

	keyboard_state_changed(0, 11, 0);
	assert_last_state(0, 11, 0, -1);
}

ZTEST(keyboard_fn, test_fn_unrelated_key)
{
	/* No codes emitted when pressing Fn and a non mapped key */
	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(0, 11, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(0, 11, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);
}

ZTEST(keyboard_fn, test_fn_alone)
{
	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 2);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
}

ZTEST(keyboard_fn, test_fn_f1_f1_fn)
{
	/* Press and release F1 while olding Fn */

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, F1_CODE);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, F1_CODE);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);
}

ZTEST(keyboard_fn, test_fn_f1_fn_f1)
{
	/* Press Fn, press F1, release Fn, release F1 */

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, F1_CODE);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, F1_CODE);
}

ZTEST(keyboard_fn, test_f1_fn_f1_fn)
{
	/* Press F1, press Fn, release F1, release Fn */

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, -1);

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, -1);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
	RESET_FAKE(keyboard_state_changed_process);
}

ZTEST(keyboard_fn, test_f1_fn_fn_f1)
{
	/* Press F1, press and release Fn, release F1 */

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, -1);

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
	RESET_FAKE(keyboard_state_changed_process);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, -1);
}

ZTEST(keyboard_fn, test_two_fn_alone)
{
	/* Press both Fn keys, then release them: the Fn action is emitted
	 * once, when the last Fn key is released.
	 */

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	/* No tap yet, the second Fn key is still held */
	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 2);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN2_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN2_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN2_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN2_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
}

ZTEST(keyboard_fn, test_second_fn_alone)
{
	/* The second Fn key also acts as Fn on its own */
	keyboard_state_changed(FN2_ROW, FN2_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 2);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN2_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN2_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN2_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN2_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
}

ZTEST(keyboard_fn, test_fn2_f1_fn2)
{
	/* Press and release F1 while holding the second Fn key */

	keyboard_state_changed(FN2_ROW, FN2_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, F1_CODE);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, F1_CODE);

	/* F1 was triggered, so no tap is emitted on Fn release */
	keyboard_state_changed(FN2_ROW, FN2_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);
}

ZTEST(keyboard_fn, test_fn_fn2_f1_release_both)
{
	/* Press both Fn keys, use F1, then release both Fn keys */

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(F1_ROW, F1_COL, 1);
	assert_last_state(F1_ROW, F1_COL, 1, F1_CODE);

	keyboard_state_changed(F1_ROW, F1_COL, 0);
	assert_last_state(F1_ROW, F1_COL, 0, F1_CODE);

	/* Both Fn keys triggered an action, so neither emits a tap */
	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);
}

ZTEST(keyboard_fn, test_fn2_tap_while_fn_held)
{
	/* Hold the first Fn key, then tap the second Fn key. The Fn action is
	 * emitted only once, when the last Fn key is released.
	 */

	keyboard_state_changed(FN_ROW, FN_COL, 1);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN2_ROW, FN2_COL, 1);
	keyboard_state_changed(FN2_ROW, FN2_COL, 0);
	/* First Fn is still held, no tap yet */
	zassert_equal(keyboard_state_changed_process_fake.call_count, 0);

	keyboard_state_changed(FN_ROW, FN_COL, 0);
	zassert_equal(keyboard_state_changed_process_fake.call_count, 2);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[0],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[0],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[0], 1);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[0], -1);

	zassert_equal(keyboard_state_changed_process_fake.arg0_history[1],
		      FN_ROW);
	zassert_equal(keyboard_state_changed_process_fake.arg1_history[1],
		      FN_COL);
	zassert_equal(keyboard_state_changed_process_fake.arg2_history[1], 0);
	zassert_equal(keyboard_state_changed_process_fake.arg3_history[1], -1);
}

static void reset(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(keyboard_state_changed_process);
}

ZTEST_SUITE(keyboard_fn, NULL, NULL, reset, reset, NULL);
