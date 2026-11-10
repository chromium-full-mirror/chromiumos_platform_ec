/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Devicetree constants for alternate LED policies.
 */

#ifndef PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LIGHTBAR_POLICY_ALT_H_
#define PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LIGHTBAR_POLICY_ALT_H_

#define LED_ALT_POLICY_SHUTDOWN 0
#define LED_ALT_POLICY_SUSPEND 1
#define LED_ALT_POLICY_LOGO_AC 2
#define LED_ALT_POLICY_LOGO_BATTERY 3
#define LED_ALT_POLICY_NORMAL 4
#define LED_ALT_POLICY_AC_PLUG_S0 5
#define LED_ALT_POLICY_AC_PLUG_S3S5 6
#define LED_ALT_POLICY_AC_UNPLUG_S0 7
#define LED_ALT_POLICY_AC_UNPLUG_S3S5 8
#define LED_ALT_POLICY_RESUME_DEBOUNCE 9
#define LED_ALT_POLICY_UNPLUG_DEBOUNCE 10
#define LED_ALT_POLICY_IDLE_DISABLED 11
#define LED_ALT_POLICY_STARTUP_CRITICAL 12

/* Base index for diagnostic policies */
#define LED_ALT_POLICY_DIAG_BASE 13

#define LB_CONTROL_REAL_RESUME 1
#define LB_CONTROL_IDLE_DISABLE 2
#define LB_CONTROL_IDLE_ENABLE 3
#define LB_CONTROL_BATTERY_SAVER_ENABLE 4
#define LB_CONTROL_BATTERY_SAVER_DISABLE 5
#define LB_CONTROL_WARM_REBOOT 6
#define LB_CONTROL_TAKEOVER_ENABLE 7
#define LB_CONTROL_TAKEOVER_RELEASE 8

#define LB_VERSION_FLAG_TAKEOVER_CAPABLE BIT(0)
#define LB_VERSION_FLAG_TAKEOVER_ACTIVE BIT(1)
#define LB_VERSION_FLAG_BUSY BIT(2)

/*
 * The policy matching logic runs on the system hook tick. To account for
 * the worst-case scheduling delay (the animation starting just after a tick),
 * we add one full tick interval to the calculated timeout.
 * Note: CONFIG_CROS_EC_HOOK_TICK_INTERVAL is in microseconds.
 */
#define ANIMATION_TICK_ERROR_MS (CONFIG_CROS_EC_HOOK_TICK_INTERVAL / 1000)

/*
 * We add a 50ms hold period at the end of one-shot animations. This
 * is the same value of the Cancel Animation behavior to prevents the
 * lightbar from instantly stacking up.
 */
#define ANIMATION_DEFAULT_HOLD_MS 50

#if defined(CONFIG_PLATFORM_EC_LIGHTBAR_ANIMATION_EXTRA_HOLD_MS) && \
	(CONFIG_PLATFORM_EC_LIGHTBAR_ANIMATION_EXTRA_HOLD_MS > 0)
#define ANIMATION_TRANSITION_HOLD_MS \
	(ANIMATION_DEFAULT_HOLD_MS + \
	 CONFIG_PLATFORM_EC_LIGHTBAR_ANIMATION_EXTRA_HOLD_MS)
#else
#define ANIMATION_TRANSITION_HOLD_MS ANIMATION_DEFAULT_HOLD_MS
#endif

/*
 * Configurable delay (in milliseconds) to wait after an AC unplug event
 * before triggering the unplug animation. This provides a platform sync
 * window to ensure the system settles and prevents the animation from
 * playing during transient power-state transitions or while LED power rails
 * are temporarily unpowered.
 */
#if defined(CONFIG_PLATFORM_EC_LIGHTBAR_AC_UNPLUG_DELAY_MS) && \
	(CONFIG_PLATFORM_EC_LIGHTBAR_AC_UNPLUG_DELAY_MS > 0)
#define ANIMATION_AC_UNPLUG_DELAY_MS \
	CONFIG_PLATFORM_EC_LIGHTBAR_AC_UNPLUG_DELAY_MS
#else
#define ANIMATION_AC_UNPLUG_DELAY_MS 0
#endif

#define WARM_REBOOT_TIMEOUT_MS (30 * MSEC_PER_SEC)

/* Helper macro to sum the 'period-ms' property of a color node. */
#define ADD_PERIOD_MS(color_node) +DT_PROP_OR(color_node, period_ms, 0)

/* Helper macro to get the node identifier for the 'pattern-0' child. */
#define GET_PATTERN_NODE_ID(policy_label) \
	DT_CHILD(DT_NODELABEL(policy_label), pattern_0)

/*
 * Internal macro to calculate the total duration for a given policy label.
 * 1. Get the 'pattern-0' child node.
 * 2. Sum the 'period-ms' of all children (colors) of that pattern node.
 * 3. Multiply by 'cycle-count' (defaulting to 1 if missing).
 * 4. Add ANIMATION_TICK_ERROR_MS to cover the scheduling jitter.
 * 5. Add ANIMATION_TRANSITION_HOLD_MS for visual stability.
 */
#define _CALCULATE_RAW_DURATION(policy_label)                              \
	(((0 DT_FOREACH_CHILD(GET_PATTERN_NODE_ID(policy_label),           \
			      ADD_PERIOD_MS)) *                            \
	  DT_PROP_OR(GET_PATTERN_NODE_ID(policy_label), cycle_count, 1)) + \
	 ANIMATION_TICK_ERROR_MS + ANIMATION_TRANSITION_HOLD_MS)

/*
 * Public macro to calculate the total duration for a given policy label.
 * If the node label does not exist in DeviceTree, return 0.
 */
#define CALCULATE_TOTAL_DURATION(policy_label)                  \
	COND_CODE_1(DT_NODE_EXISTS(DT_NODELABEL(policy_label)), \
		    (_CALCULATE_RAW_DURATION(policy_label)), (0))

/* Define the constants by applying the calculation to each labeled policy */
#define LED_ANIMATION_LOGO_AC_MS CALCULATE_TOTAL_DURATION(lb_state_logo_ac)
#define LED_ANIMATION_LOGO_BATTERY_MS \
	CALCULATE_TOTAL_DURATION(lb_state_logo_battery)
#define LED_ANIMATION_RESUME_DEBOUNCE_MS \
	CALCULATE_TOTAL_DURATION(lb_state_resume_debounce)
#define LED_ANIMATION_UNPLUG_DEBOUNCE_MS                           \
	MAX(CALCULATE_TOTAL_DURATION(lb_state_unplug_debounce_s0), \
	    CALCULATE_TOTAL_DURATION(lb_state_unplug_debounce_s3_s5))
#define LED_ANIMATION_SHUTDOWN_MS CALCULATE_TOTAL_DURATION(lb_state_shutdown)
#define LED_ANIMATION_SUSPEND_MS CALCULATE_TOTAL_DURATION(lb_state_suspend)

/* For AC plug/unplug, use the maximum of all plug/unplug durations */
#define LED_ANIMATION_UNPLUG_MS                                     \
	MAX(CALCULATE_TOTAL_DURATION(lb_state_unplug_low),          \
	    MAX(CALCULATE_TOTAL_DURATION(lb_state_unplug_med),      \
		MAX(CALCULATE_TOTAL_DURATION(lb_state_unplug_high), \
		    CALCULATE_TOTAL_DURATION(lb_state_unplug_full))))

#define LED_ANIMATION_PLUG_MS                                     \
	MAX(CALCULATE_TOTAL_DURATION(lb_state_plug_low),          \
	    MAX(CALCULATE_TOTAL_DURATION(lb_state_plug_med),      \
		MAX(CALCULATE_TOTAL_DURATION(lb_state_plug_high), \
		    CALCULATE_TOTAL_DURATION(lb_state_plug_full))))

#endif /* PLATFORM_EC_ZEPHYR_INCLUDE_DRIVERS_LIGHTBAR_POLICY_ALT_H_ */
