/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Definitions for the fakes shared by the blanc board and fan suites.
 *
 * The header is included first on purpose: DECLARE_FAKE_VALUE_FUNC() creates
 * the chipset_in_state_Fake struct that DEFINE_FAKE_VALUE_FUNC() below needs.
 */

#include "blanc_fakes.h"

DEFINE_FAKE_VALUE_FUNC(int, chipset_in_state, int);
