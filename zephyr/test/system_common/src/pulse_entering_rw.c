/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gpio.h"
#include "gpio_signal.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/ztest.h>

void pulse_entering_rw(void);

static int high_transitions;
static int low_transitions;

static void entering_rw_callback(const struct device *port,
				 struct gpio_callback *cb,
				 gpio_port_pins_t pins)
{
	const struct gpio_dt_spec *spec = gpio_get_dt_spec(GPIO_ENTERING_RW);

	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	if (gpio_emul_output_get(port, spec->pin)) {
		high_transitions++;
	} else {
		low_transitions++;
	}
}

ZTEST(pulse_entering_rw, test_pulse_entering_rw)
{
	const struct gpio_dt_spec *spec = gpio_get_dt_spec(GPIO_ENTERING_RW);
	struct gpio_callback cb;

	zassert_not_null(spec, "GPIO_ENTERING_RW should be implemented");

	/* Initialize and add callback to detect pin state transitions */
	high_transitions = 0;
	low_transitions = 0;
	gpio_init_callback(&cb, entering_rw_callback, BIT(spec->pin));
	zassert_ok(gpio_add_callback(spec->port, &cb));

	/* Run pulse_entering_rw and verify behavior */
	pulse_entering_rw();

	/* Clean up callback */
	gpio_remove_callback(spec->port, &cb);

	/* Verify the pulse asserted high then deasserted low */
	zassert_equal(high_transitions, 1, "Expected 1 high transition, got %d",
		      high_transitions);
	zassert_equal(low_transitions, 1, "Expected 1 low transition, got %d",
		      low_transitions);
	zassert_equal(gpio_emul_output_get(spec->port, spec->pin), 0,
		      "GPIO_ENTERING_RW should be deasserted (0) after pulse");

	/* Preset high to verify pulse_entering_rw resets pin from high */
	zassert_ok(gpio_pin_set_dt(spec, 1));
	zassert_equal(gpio_emul_output_get(spec->port, spec->pin), 1);
	pulse_entering_rw();
	zassert_equal(gpio_emul_output_get(spec->port, spec->pin), 0,
		      "GPIO_ENTERING_RW should be deasserted (0) after pulse");
}

ZTEST_SUITE(pulse_entering_rw, NULL, NULL, NULL, NULL, NULL);
