/* Copyright 2020 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This file is used for platform/ec implementations of irq_lock and irq_unlocks
 */

#include "task.h"

uint32_t irq_lock(void)
{
	uint32_t key = is_interrupt_enabled();

	interrupt_disable();
	return key;
}

void irq_unlock(uint32_t key)
{
	if (key)
		interrupt_enable();
}
