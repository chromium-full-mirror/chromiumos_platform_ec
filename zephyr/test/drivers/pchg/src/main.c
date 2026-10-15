/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "peripheral_charger.h"

#include <string.h>

#include <zephyr/ztest.h>

/* Exposed by test_export_static in common/peripheral_charger.c */
const char *_text_event(enum pchg_event event);
const char *_text_state(enum pchg_state state);

ZTEST_SUITE(pchg, NULL, NULL, NULL, NULL, NULL);

ZTEST(pchg, test_text_event_limits)
{
	/* Valid boundary events (first and last valid indices) */
	zassert_equal(0, strcmp("NONE", _text_event(PCHG_EVENT_NONE)));
	zassert_equal(0, strcmp("INITIALIZED",
				_text_event(PCHG_EVENT_INITIALIZED)));
	zassert_equal(0, strcmp("UPDATE_ERROR",
				_text_event(PCHG_EVENT_UPDATE_ERROR)));

	/* Invalid event indices */
	zassert_equal(0, strcmp("UNDEF", _text_event((enum pchg_event) - 1)));
	zassert_equal(0, strcmp("UNDEF", _text_event(PCHG_EVENT_COUNT)));
	zassert_equal(0, strcmp("UNDEF", _text_event(PCHG_EVENT_COUNT + 1)));
}

ZTEST(pchg, test_text_state_limits)
{
	/* Valid boundary states (first and last valid indices) */
	zassert_equal(0, strcmp("RESET", _text_state(PCHG_STATE_RESET)));
	zassert_equal(0, strcmp("BIST", _text_state(PCHG_STATE_BIST)));

	/* Invalid state indices */
	zassert_equal(0, strcmp("UNDEF", _text_state((enum pchg_state) - 1)));
	zassert_equal(0, strcmp("UNDEF", _text_state(PCHG_STATE_COUNT)));
	zassert_equal(0, strcmp("UNDEF", _text_state(PCHG_STATE_COUNT + 1)));
}
