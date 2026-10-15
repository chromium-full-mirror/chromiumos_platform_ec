/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "driver/accel_lis2dh.h"
#include "emul/emul_common_i2c.h"
#include "emul/emul_lis2dh.h"
#include "motion_sense.h"
#include "motion_sense_fifo.h"
#include "test/drivers/test_state.h"

#include <zephyr/ztest.h>

#define LIS2DH_NODE DT_NODELABEL(lis2dh_emul)
#define ACC_SENSOR_ID SENSOR_ID(DT_NODELABEL(ms_lis2dh_accel))
#define OTHER_SENSOR_ID SENSOR_ID(DT_NODELABEL(lid_accel))

static const struct emul *emul = EMUL_DT_GET(LIS2DH_NODE);
static struct motion_sensor_t *acc = &motion_sensors[ACC_SENSOR_ID];
#ifdef CONFIG_GESTURE_HOST_DETECTION
static struct motion_sensor_t *other_acc = &motion_sensors[OTHER_SENSOR_ID];
#endif

static void lis2dh_double_tap_before(void *state)
{
	ARG_UNUSED(state);
	lis2dh_emul_reset(emul);
	acc->flags &= ~(MOTIONSENSE_FLAG_IN_FORCED_MODE |
			MOTIONSENSE_FLAG_IN_SPOOF_MODE);
	acc->current_range = BASE_RANGE(acc->default_range);
	motion_sense_fifo_reset();
}

static void enable_double_tap_if_host_managed(void)
{
#ifdef CONFIG_GESTURE_HOST_DETECTION
	zassert_ok(acc->drv->manage_activity(
		acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 1, NULL));
#endif
}

ZTEST(lis2dh_double_tap, test_init_click_registers)
{
	zassert_ok(lis2dh_drv.init(acc));

	zassert_true(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL2_ADDR) &
		     LIS2DH_CTRL2_HPCLICK);
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_THS_ADDR),
		      LIS2DH_CLICK_THS_REG(BASE_RANGE(acc->default_range)));
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_TIME_LIMIT_ADDR),
		      LIS2DH_TIME_LIMIT_VAL);
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_TIME_LATENCY_ADDR),
		      LIS2DH_TIME_LATENCY_VAL);
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_TIME_WINDOW_ADDR),
		      LIS2DH_TIME_WINDOW_VAL);
	/* FIFO must remain in bypass mode at init while ODR == 0. */
	zassert_false(lis2dh_emul_is_fifo_enabled(emul));

#ifdef CONFIG_GESTURE_HOST_DETECTION
	{
		uint32_t enabled = 0, disabled = 0;

		zassert_ok(acc->drv->list_activities(acc, &enabled, &disabled));
		zassert_equal(enabled, 0);
		zassert_equal(disabled, BIT(MOTIONSENSE_ACTIVITY_DOUBLE_TAP));
		zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_CFG_ADDR),
			      0);
		zassert_false(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
			      LIS2DH_INT1_CLICK_MASK);
	}
#else
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_CFG_ADDR),
		      LIS2DH_CLICK_ZD);
	zassert_true(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		     LIS2DH_INT1_CLICK_MASK);
#endif
}

ZTEST(lis2dh_double_tap, test_set_range_updates_click_ths)
{
	struct i2c_common_emul_data *common_data =
		emul_lis2dh_get_i2c_common_data(emul);

	zassert_ok(lis2dh_drv.init(acc));
	enable_double_tap_if_host_managed();

	for (int range = 2; range <= 16; range *= 2) {
		zassert_ok(lis2dh_drv.set_range(acc, range, 0));
		zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_THS_ADDR),
			      LIS2DH_CLICK_THS_REG(range));
		/* ODR is 0 with double-tap active, so FIFO stays in bypass. */
		zassert_false(lis2dh_emul_is_fifo_enabled(emul));
	}

	/* CLICK_THS I2C write failure propagates error. */
	i2c_common_emul_set_write_fail_reg(common_data, LIS2DH_CLICK_THS_ADDR);
	zassert_equal(lis2dh_drv.set_range(acc, 4, 0), EC_ERROR_INVAL);
	i2c_common_emul_set_write_fail_reg(common_data,
					   I2C_COMMON_EMUL_NO_FAIL_REG);
}

#ifdef CONFIG_GESTURE_HOST_DETECTION
ZTEST(lis2dh_double_tap, test_manage_activity)
{
	struct i2c_common_emul_data *common_data =
		emul_lis2dh_get_i2c_common_data(emul);
	struct stprivate_data *drvdata = acc->drv_data;
	uint32_t enabled = 0, disabled = 0;

	zassert_ok(lis2dh_drv.init(acc));

	/* Invalid sensor and unsupported activity return INVALID_PARAM. */
	zassert_equal(acc->drv->manage_activity(other_acc,
						MOTIONSENSE_ACTIVITY_DOUBLE_TAP,
						1, NULL),
		      EC_RES_INVALID_PARAM);
	zassert_equal(acc->drv->manage_activity(
			      acc, MOTIONSENSE_ACTIVITY_SIG_MOTION, 1, NULL),
		      EC_RES_INVALID_PARAM);

	/* At ODR == 0, enabling double-tap runs CTRL1 at 400 Hz & routes INT1.
	 */
	zassert_ok(acc->drv->manage_activity(
		acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 1, NULL));
	zassert_ok(acc->drv->list_activities(acc, &enabled, &disabled));
	zassert_equal(enabled, BIT(MOTIONSENSE_ACTIVITY_DOUBLE_TAP));
	zassert_equal(disabled, 0);
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_CFG_ADDR),
		      LIS2DH_CLICK_ZD);
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_400HZ_VAL);
	zassert_equal(drvdata->base.odr, 0);
	zassert_true(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		     LIS2DH_INT1_CLICK_MASK);
	zassert_false(lis2dh_emul_is_fifo_enabled(emul));

	/* Disabling double-tap powers down CTRL1 and unroutes I1_CLICK. */
	zassert_ok(acc->drv->manage_activity(
		acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 0, NULL));
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_CFG_ADDR), 0);
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_0HZ_VAL);
	zassert_false(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		      LIS2DH_INT1_CLICK_MASK);

	/* Start streaming at 10 Hz, then toggle double-tap on and back off. */
	zassert_ok(acc->drv->set_data_rate(acc, 10000, 1));
	zassert_equal(drvdata->base.odr, 10000);
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_10HZ_VAL);

	zassert_ok(acc->drv->manage_activity(
		acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 1, NULL));
	zassert_equal(drvdata->base.odr, 400000);
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_400HZ_VAL);
	zassert_false(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		      LIS2DH_INT1_CLICK_MASK);

	/* Disabling double-tap restores the original 10 Hz ODR. */
	zassert_ok(acc->drv->manage_activity(
		acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 0, NULL));
	zassert_equal(drvdata->base.odr, 10000);
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_10HZ_VAL);

	/* I2C failure paths return EC_RES_UNAVAILABLE. */
	i2c_common_emul_set_write_fail_reg(common_data, LIS2DH_CLICK_CFG_ADDR);
	zassert_equal(acc->drv->manage_activity(
			      acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 1, NULL),
		      EC_RES_UNAVAILABLE);
	i2c_common_emul_set_write_fail_reg(common_data, LIS2DH_CTRL1_ADDR);
	zassert_equal(acc->drv->manage_activity(
			      acc, MOTIONSENSE_ACTIVITY_DOUBLE_TAP, 1, NULL),
		      EC_RES_UNAVAILABLE);
	i2c_common_emul_set_write_fail_reg(common_data,
					   I2C_COMMON_EMUL_NO_FAIL_REG);
}
#endif /* CONFIG_GESTURE_HOST_DETECTION */

ZTEST(lis2dh_double_tap, test_forced_mode_preserves_click_irq)
{
	struct stprivate_data *drvdata = acc->drv_data;

	zassert_ok(lis2dh_drv.init(acc));
	enable_double_tap_if_host_managed();

	/*
	 * Simulate S3 forced-mode polling: motion_sense disables FIFO
	 * data-ready interrupts via enable_interrupt(false) and polls at 10 Hz.
	 */
	acc->flags |= MOTIONSENSE_FLAG_IN_FORCED_MODE;
	zassert_ok(acc->drv->enable_interrupt(acc, false));
	zassert_ok(acc->drv->set_data_rate(acc, 10000, 1));

	/* Hardware runs at 400 Hz with I1_CLICK routed, while EC polls 10 Hz.
	 */
	zassert_equal(lis2dh_emul_peek_odr(emul), LIS2DH_ODR_400HZ_VAL);
	zassert_equal(drvdata->base.odr, 10000);
	zassert_true(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		     LIS2DH_INT1_CLICK_MASK);
	zassert_false(lis2dh_emul_peek_reg(emul, LIS2DH_CTRL3_ADDR) &
		      LIS2DH_INT1_FTH_MASK);
	zassert_false(lis2dh_emul_is_fifo_enabled(emul));
}

ZTEST(lis2dh_double_tap, test_double_tap_irq_idle_and_streaming)
{
	int16_t fake_sample[] = { 10, 20, 30, 40, 50, 60 };
	uint32_t evt;

	zassert_ok(lis2dh_drv.init(acc));
	enable_double_tap_if_host_managed();
	zassert_ok(acc->drv->set_data_rate(acc, 0, 0));

	/* 1. Idle (ODR == 0): I1_CLICK asserts INT1 and dispatches event. */
	lis2dh_emul_set_click_src(emul, LIS2DH_CLICK_SRC_IA |
						LIS2DH_CLICK_SRC_DCLICK);
	zassert_equal(lis2dh_emul_get_interrupt_pin(emul), 1);

	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_true(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				   MOTIONSENSE_ACTIVITY_DOUBLE_TAP));
	lis2dh_emul_set_click_src(emul, 0);
	zassert_equal(lis2dh_emul_get_interrupt_pin(emul), 0);

	/* 2. Forced mode: dispatches activity event and skips FIFO drain. */
	acc->flags |= MOTIONSENSE_FLAG_IN_FORCED_MODE;
	zassert_ok(acc->drv->enable_interrupt(acc, false));
	zassert_ok(acc->drv->set_data_rate(acc, 10000, 1));
	lis2dh_emul_set_click_src(emul, LIS2DH_CLICK_SRC_IA |
						LIS2DH_CLICK_SRC_DCLICK);
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_equal(lis2dh_drv.irq_handler(acc, &evt), EC_ERROR_NOT_HANDLED);
	zassert_true(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				   MOTIONSENSE_ACTIVITY_DOUBLE_TAP));
	lis2dh_emul_set_click_src(emul, 0);
	acc->flags &= ~MOTIONSENSE_FLAG_IN_FORCED_MODE;
	zassert_ok(acc->drv->enable_interrupt(acc, true));

	/*
	 * 3. FIFO streaming (LIR_Click = 0): CLICK_SRC stays asserted across
	 * multiple watermark interrupts during TIME_LATENCY; click_active must
	 * deduplicate so only one double-tap event is emitted per pulse.
	 */
	zassert_ok(acc->drv->set_data_rate(acc, 10000, 1));
	acc->oversampling_ratio = 1;
	lis2dh_emul_set_click_src(emul, LIS2DH_CLICK_SRC_IA |
						LIS2DH_CLICK_SRC_DCLICK);

	lis2dh_emul_set_fifo_data(emul, fake_sample, sizeof(fake_sample));
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_true(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				   MOTIONSENSE_ACTIVITY_DOUBLE_TAP));

	/* Second watermark while DCLICK pulse is still active: no duplicate. */
	lis2dh_emul_set_fifo_data(emul, fake_sample, sizeof(fake_sample));
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_false(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				    MOTIONSENSE_ACTIVITY_DOUBLE_TAP));

	/* Pulse ends (CLICK_SRC == 0), re-arming click_active for next tap. */
	lis2dh_emul_set_click_src(emul, 0);
	lis2dh_emul_set_fifo_data(emul, fake_sample, sizeof(fake_sample));
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_false(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				    MOTIONSENSE_ACTIVITY_DOUBLE_TAP));

	/* Next double-tap emits a fresh activity event. */
	lis2dh_emul_set_click_src(emul, LIS2DH_CLICK_SRC_IA |
						LIS2DH_CLICK_SRC_DCLICK);
	lis2dh_emul_set_fifo_data(emul, fake_sample, sizeof(fake_sample));
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_true(evt & TASK_EVENT_MOTION_ACTIVITY_INTERRUPT(
				   MOTIONSENSE_ACTIVITY_DOUBLE_TAP));

	/* 4. Emulator LIR_Click = 1 read-to-clear check. */
	zassert_ok(acc->drv->set_data_rate(acc, 0, 0));
	zassert_ok(i2c_write8(acc->port, acc->i2c_spi_addr_flags,
			      LIS2DH_CLICK_THS_ADDR,
			      LIS2DH_CLICK_LIR | LIS2DH_CLICK_THS_REG(2)));
	lis2dh_emul_set_click_src(emul, LIS2DH_CLICK_SRC_IA |
						LIS2DH_CLICK_SRC_DCLICK);
	evt = ACCEL_LIS2DH_INT_EVENT;
	zassert_ok(lis2dh_drv.irq_handler(acc, &evt));
	zassert_equal(lis2dh_emul_peek_reg(emul, LIS2DH_CLICK_SRC_ADDR), 0);
	zassert_equal(lis2dh_emul_get_interrupt_pin(emul), 0);
}

ZTEST_SUITE(lis2dh_double_tap, drivers_predicate_post_main, NULL,
	    lis2dh_double_tap_before, NULL, NULL);
