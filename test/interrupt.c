/* Copyright 2013 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Test interrupt support of EC emulator.
 */
#include "common.h"
#include "console.h"
#include "task.h"
#include "test_util.h"
#include "timer.h"
#include "util.h"

#include <stdio.h>

#include <unistd.h>

static volatile int main_count;
static volatile int has_error;
static volatile int interrupt_count;
static volatile int critical_section_isr_count;

/* period between 50us and 3.2ms */
#define PERIOD_US(num) (((num % 64) + 1) * 50)

void my_isr(void)
{
	int i = main_count;
	udelay(3 * PERIOD_US(prng_no_seed()));
	if (i != main_count || !in_interrupt_context())
		has_error |= BIT(0);
	interrupt_count++;
}

/* ISRs may enter a critical section with interrupt_disable(). */
static void critical_section_isr(void)
{
	interrupt_disable();
	if (is_interrupt_enabled())
		has_error |= BIT(1);
	critical_section_isr_count++;
	interrupt_enable();
	if (!is_interrupt_enabled())
		has_error |= BIT(2);
}

static volatile uint32_t enable_ready_reg;

static void set_ready_bit(void)
{
	if (enable_ready_reg & BIT(0))
		enable_ready_reg |= BIT(1);
}

void interrupt_generator(void)
{
	while (1) {
		udelay(3 * PERIOD_US(prng_no_seed()));
		task_trigger_test_interrupt(my_isr);
		task_trigger_test_interrupt(set_ready_bit);
		task_trigger_test_interrupt(critical_section_isr);
	}
}

static int interrupt_test(void)
{
	int i, j;

	for (i = 0; i < 50; i++) {
		for (j = 0; j < 10000; j++)
			++main_count;
		usleep(MSEC);
	}

	ccprintf("Interrupt count: %d\n", interrupt_count);
	ccprintf("Main thread tick: %d\n", main_count);

	TEST_GT(interrupt_count, 0, "%d");
	TEST_EQ(has_error, 0, "0x%x");
	TEST_ASSERT(!in_interrupt_context());

	return EC_SUCCESS;
}

static int interrupt_disable_test(void)
{
	int start_int_cnt, end_int_cnt;

	interrupt_disable();
	start_int_cnt = interrupt_count;
	usleep(50 * MSEC);
	end_int_cnt = interrupt_count;
	interrupt_enable();

	TEST_ASSERT(start_int_cnt == end_int_cnt);

	return EC_SUCCESS;
}

void test_init(void)
{
	interrupt_disable();
	if (is_interrupt_enabled())
		has_error |= BIT(3);
	interrupt_enable();
	if (!is_interrupt_enabled())
		has_error |= BIT(4);
}

static int test_interrupt_disable_in_isr(void)
{
	int start_int_cnt = critical_section_isr_count;
	int i;

	for (i = 0; i < 50 && critical_section_isr_count == start_int_cnt; i++)
		usleep(MSEC);

	TEST_GT(critical_section_isr_count, start_int_cnt, "%d");
	TEST_EQ(has_error, 0, "0x%x");
	TEST_ASSERT(is_interrupt_enabled());

	return EC_SUCCESS;
}

static int test_wait_for_ready(void)
{
	wait_for_ready(&enable_ready_reg, BIT(0), BIT(1));
	TEST_EQ(enable_ready_reg, BIT(0) | BIT(1), "%x");

	return EC_SUCCESS;
}

void run_test(int argc, const char **argv)
{
	test_reset();

	RUN_TEST(interrupt_test);
	RUN_TEST(interrupt_disable_test);
	RUN_TEST(test_interrupt_disable_in_isr);
	RUN_TEST(test_wait_for_ready);

	test_print_result();
}
