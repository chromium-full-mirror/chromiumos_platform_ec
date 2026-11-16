/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef PLATFORM_EC_ZEPHYR_TEST_OCELOT_SRC_BLANC_FAKES_H_
#define PLATFORM_EC_ZEPHYR_TEST_OCELOT_SRC_BLANC_FAKES_H_

#include "chipset.h"

#include <zephyr/fff.h>

/*
 * Fakes shared by the blanc board and fan suites. Both are compiled into the
 * same test binary (ocelot.blanc_board), so the definition lives in
 * blanc_fakes.c and this header only declares it. Same split as
 * zephyr/test/rex/tests/rex/src/rex_fakes.h + fakes.c.
 */
DECLARE_FAKE_VALUE_FUNC(int, chipset_in_state, int);

#endif /* PLATFORM_EC_ZEPHYR_TEST_OCELOT_SRC_BLANC_FAKES_H_ */
