/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Board-specific state machine to drive alternate LED animations.
 */

#include "charge_state.h"
#include "chipset.h"
#include "extpower.h"
#include "hooks.h"
#include "led_common.h"
#include "led_lightbar.h"
#include "power.h"
#include "util.h"

#include <zephyr/kernel.h>

#include <drivers/lightbar_policy_alt.h>

/*
 * Safety check: The shutdown animation must complete before the system
 * transitions from S5 to Z1 (G3).
 *
 * On specific platforms, the 5V rail switches to a low-current LDO mode
 * upon entering Z1. If the lightbar is still active (drawing ~100mA),
 * it will overload the LDO. Check b/482049478.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(intel_ap_pwrseq)
#define S5_TIMEOUT_SEC                                          \
	DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(intel_ap_pwrseq), \
		s5_inactivity_timeout)
#else
#define S5_TIMEOUT_SEC 10 /* Default 10s in power/common.c */
#endif

BUILD_ASSERT(LED_ANIMATION_SHUTDOWN_MS < S5_TIMEOUT_SEC * 1000,
	     "Shutdown animation exceeds S5 timeout; risks LDO overload in Z1.");

/* Ensure animations are not accidentally deleted in project overlays. */
BUILD_ASSERT(LED_ANIMATION_SHUTDOWN_MS > 0, "Shutdown animation duration is 0");
BUILD_ASSERT(LED_ANIMATION_SUSPEND_MS > 0, "Suspend animation duration is 0");
BUILD_ASSERT(LED_ANIMATION_LOGO_AC_MS > 0,
	     "Logo-on-AC animation duration is 0");
BUILD_ASSERT(LED_ANIMATION_LOGO_BATTERY_MS > 0,
	     "Logo-on-battery animation duration is 0");
BUILD_ASSERT(LED_ANIMATION_PLUG_MS > 0, "AC Plug animation duration is 0");
BUILD_ASSERT(LED_ANIMATION_UNPLUG_MS > 0, "AC Unplug animation duration is 0");

#define LB_PRIO_STANDARD 0
#define LB_PRIO_DIAGNOSTIC 1

/*
 * This value was determined experimentally; use a conservative value 1500ms.
 * Adjust as needed or override it on a per-project basis.
 *
 * Note that this value must be a multiple of the tick duration (250ms), as the
 * policies are only evaluated during the tick hook.
 */
#define DARK_RESUME_DEBOUNCE_MS                     \
	(LED_ANIMATION_RESUME_DEBOUNCE_MS > 0 ?     \
		 LED_ANIMATION_RESUME_DEBOUNCE_MS : \
		 1500)

static bool is_startup = false;
static bool is_suspend = false;
static bool is_idle_disabled = false;
static bool is_battery_saver = false;
static bool is_real_resume = false;
static bool ignore_charger_events = false;
static bool is_takeover;
static bool policy_sync_pending;
static struct custom_led_patterns_t *pending_custom_pattern;

static int active_one_shot_policy = LED_ALT_POLICY_NORMAL;
static uint8_t active_prio = LB_PRIO_STANDARD;

/* Timer to reset the one-shot policy after an animation completes */
static void one_shot_timer_expiry_handler(struct k_timer *timer);
static K_TIMER_DEFINE(one_shot_timer, one_shot_timer_expiry_handler, NULL);

bool lb_is_takeover_active(void)
{
	return is_takeover;
}

bool lb_is_ec_busy(void)
{
	return k_timer_remaining_get(&one_shot_timer) > 0 ||
	       active_prio > LB_PRIO_STANDARD;
}

uint32_t lb_get_version_flags(void)
{
	uint32_t flags = LB_VERSION_FLAG_TAKEOVER_CAPABLE;

	if (is_takeover) {
		flags |= LB_VERSION_FLAG_TAKEOVER_ACTIVE;
	}
	if (lb_is_ec_busy()) {
		flags |= LB_VERSION_FLAG_BUSY;
	}

	return flags;
}

static void update_idle_policy(void)
{
	int next_policy;

	/*
	 * Do not clobber an active one-shot animation (such as the cold-boot
	 * logo animation) while its timer is still running. When the timer
	 * expires, one_shot_timer_expiry_handler() will apply the updated
	 * idle policy.
	 */
	if (k_timer_remaining_get(&one_shot_timer) > 0) {
		return;
	}

	/*
	 * When userspace takeover is active, select
	 * LED_ALT_POLICY_IDLE_DISABLED so the active DT node does not depend on
	 * chipset-state, charge-state, or batt-lvl, preventing S0 <-> S3
	 * transitions from cancelling host custom patterns in led.c.
	 */
	if (is_takeover || is_idle_disabled || is_battery_saver ||
	    !is_real_resume) {
		next_policy = LED_ALT_POLICY_IDLE_DISABLED;
	} else {
		next_policy = LED_ALT_POLICY_NORMAL;
	}

	if (active_one_shot_policy != next_policy) {
		active_one_shot_policy = next_policy;
		policy_sync_pending = true;
	}
}

void lb_cancel_one_shot(struct custom_led_patterns_t *pattern)
{
	/*
	 * Unlike lb_set_takeover(true), which lets an active cold-boot logo
	 * animation keep playing until the host is ready, programming a custom
	 * sequence immediately overrides the LEDs in led.c. Stop any running
	 * standard one-shot timer (including logo animations) so its expiry
	 * does not later cancel the host's custom pattern.
	 */
	if (is_takeover && active_prio == LB_PRIO_STANDARD &&
	    active_one_shot_policy != LED_ALT_POLICY_SHUTDOWN &&
	    k_timer_remaining_get(&one_shot_timer) > 0) {
		k_timer_stop(&one_shot_timer);
		update_idle_policy();
	}
	if (is_takeover && policy_sync_pending) {
		pending_custom_pattern = pattern;
	}
}

static void led_alt_policy_tick(void)
{
	struct custom_led_patterns_t *pattern = pending_custom_pattern;

	if (!policy_sync_pending) {
		return;
	}
	policy_sync_pending = false;
	pending_custom_pattern = NULL;

	if (is_takeover && !lb_is_ec_busy() && pattern) {
		led_set_custom_patterns(pattern);
	}
}
DECLARE_HOOK(HOOK_TICK, led_alt_policy_tick, HOOK_PRIO_DEFAULT + 1);

static void one_shot_timer_expiry_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	/* Back to the standard normal policy */
	update_idle_policy();
	active_prio = LB_PRIO_STANDARD;
}

static void start_one_shot_animation(int policy, uint32_t duration_ms,
				     uint8_t prio)
{
	if (prio < active_prio) {
		return;
	}

	k_timer_stop(&one_shot_timer);
	active_one_shot_policy = policy;

	if (policy == LED_ALT_POLICY_NORMAL) {
		active_prio = LB_PRIO_STANDARD;
	} else {
		active_prio = prio;
		if (duration_ms > 0) {
			k_timer_start(&one_shot_timer, K_MSEC(duration_ms),
				      K_NO_WAIT);
		}
	}
}

void lb_set_diag_policy(int policy, uint32_t duration_ms)
{
	start_one_shot_animation(policy, duration_ms, LB_PRIO_DIAGNOSTIC);
}

/* Override the function to provide an alternate policy label. */
__override int board_led_alt_policy(void)
{
	return active_one_shot_policy;
}

static void trigger_startup_animation(void)
{
	int batt_lvl = DIV_ROUND_NEAREST(charge_get_display_charge(), 10);

	/* Update this flag to show the following suspend animation */
	is_real_resume = true;

#ifdef CONFIG_PLATFORM_EC_CHARGER
	if (charge_get_status()->batt.is_present == BP_NO) {
		/* No battery, let the normal policy handle the error display */
		update_idle_policy();
		active_prio = LB_PRIO_STANDARD;
		return;
	}
#endif

	if (batt_lvl == 0) {
		/* Use infinite timeout to stay OFF during off-mode charging */
		start_one_shot_animation(LED_ALT_POLICY_STARTUP_CRITICAL, 0,
					 LB_PRIO_STANDARD);
	} else if (extpower_is_present()) {
		start_one_shot_animation(LED_ALT_POLICY_LOGO_AC,
					 LED_ANIMATION_LOGO_AC_MS,
					 LB_PRIO_STANDARD);
	} else {
		start_one_shot_animation(LED_ALT_POLICY_LOGO_BATTERY,
					 LED_ANIMATION_LOGO_BATTERY_MS,
					 LB_PRIO_STANDARD);
	}
}

/* Register the corresponding hooks to trigger animations. */
static void led_alt_policy_startup(void)
{
	is_startup = true;
	is_suspend = false;
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, led_alt_policy_startup, HOOK_PRIO_DEFAULT);

static void led_alt_policy_shutdown(void)
{
	is_startup = false;
	lb_set_takeover(false);

	if (is_suspend) {
		is_suspend = false;
		start_one_shot_animation(LED_ALT_POLICY_SHUTDOWN,
					 LED_ANIMATION_SHUTDOWN_MS,
					 LB_PRIO_STANDARD);
	}
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, led_alt_policy_shutdown, HOOK_PRIO_DEFAULT);

static void led_alt_policy_reset(void)
{
	lb_set_takeover(false);
}
DECLARE_HOOK(HOOK_CHIPSET_RESET, led_alt_policy_reset, HOOK_PRIO_DEFAULT);

static void led_alt_policy_suspend(void)
{
	is_startup = false;
	is_suspend = true;

	if (is_takeover) {
		is_real_resume = false;
		return;
	}

	/* Ignore if a higher priority animation is running */
	if (active_prio > LB_PRIO_STANDARD) {
		return;
	}

	/* If we were waiting in the debounce state, return to normal policy. */
	if (active_one_shot_policy == LED_ALT_POLICY_RESUME_DEBOUNCE) {
		k_timer_stop(&one_shot_timer);
		update_idle_policy();
	}

	/*
	 * If the system was never fully awake or an AC animation is currently
	 * running, ignore the suspend animation.
	 */
	if (!is_real_resume ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_PLUG_S0 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_UNPLUG_S0 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_PLUG_S3S5 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_UNPLUG_S3S5) {
		return;
	}
	is_real_resume = false;
	start_one_shot_animation(LED_ALT_POLICY_SUSPEND,
				 LED_ANIMATION_SUSPEND_MS, LB_PRIO_STANDARD);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, led_alt_policy_suspend, HOOK_PRIO_DEFAULT);

static void led_alt_policy_resume(void)
{
	is_suspend = false;

	/* Ignore if a higher priority animation is running */
	if (active_prio > LB_PRIO_STANDARD) {
		return;
	}

	if (is_startup) {
		is_startup = false;
		trigger_startup_animation();
		return;
	}

	if (is_takeover) {
		return;
	}

	/*
	 * AC change triggers a dark resume; ignore it to let the animation
	 * play. Also ignore dark resumes / GPIO bounces during suspend or
	 * resume animations (note that LOGO_AC and LOGO_BATTERY are used as
	 * resume animations) so they can finish playing uninterrupted.
	 */
	if (active_one_shot_policy == LED_ALT_POLICY_SUSPEND ||
	    active_one_shot_policy == LED_ALT_POLICY_LOGO_AC ||
	    active_one_shot_policy == LED_ALT_POLICY_LOGO_BATTERY ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_PLUG_S0 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_UNPLUG_S0 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_PLUG_S3S5 ||
	    active_one_shot_policy == LED_ALT_POLICY_AC_UNPLUG_S3S5) {
		return;
	}

	/* Switch to DEBOUNCE. This prevents Default-S0 from activating. */
	start_one_shot_animation(LED_ALT_POLICY_RESUME_DEBOUNCE,
				 DARK_RESUME_DEBOUNCE_MS, LB_PRIO_STANDARD);
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, led_alt_policy_resume, HOOK_PRIO_DEFAULT);

#if ANIMATION_AC_UNPLUG_DELAY_MS > 0
/* Timer to delay the transition to the unplug animation */
static void ac_unplug_delay_expiry_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (is_takeover) {
		return;
	}

	/* Asynchronously start the unplug animation after the sync window
	 * expires */
	start_one_shot_animation(LED_ALT_POLICY_AC_UNPLUG_S3S5,
				 LED_ANIMATION_UNPLUG_MS, LB_PRIO_STANDARD);
}

static K_TIMER_DEFINE(ac_unplug_delay_timer, ac_unplug_delay_expiry_handler,
		      NULL);
#endif

static bool unplug_debounce_in_progress = false;

static void lb_handle_debounced_ac_change(struct k_work *work)
{
	ARG_UNUSED(work);

	unplug_debounce_in_progress = false;

	/* Ignore AC change triggers during boot and shutdown */
	if (is_takeover || active_one_shot_policy == LED_ALT_POLICY_LOGO_AC ||
	    active_one_shot_policy == LED_ALT_POLICY_LOGO_BATTERY ||
	    active_one_shot_policy == LED_ALT_POLICY_SHUTDOWN ||
	    active_one_shot_policy == LED_ALT_POLICY_STARTUP_CRITICAL ||
	    ignore_charger_events) {
		return;
	}

	if (active_one_shot_policy != LED_ALT_POLICY_IDLE_DISABLED &&
	    chipset_in_state(CHIPSET_STATE_ON)) {
		if (extpower_is_present()) {
			start_one_shot_animation(LED_ALT_POLICY_AC_PLUG_S0,
						 LED_ANIMATION_PLUG_MS,
						 LB_PRIO_STANDARD);
		} else {
			start_one_shot_animation(LED_ALT_POLICY_AC_UNPLUG_S0,
						 LED_ANIMATION_UNPLUG_MS,
						 LB_PRIO_STANDARD);
		}
	} else {
		if (extpower_is_present()) {
			start_one_shot_animation(LED_ALT_POLICY_AC_PLUG_S3S5,
						 LED_ANIMATION_PLUG_MS,
						 LB_PRIO_STANDARD);
		} else {
#if ANIMATION_AC_UNPLUG_DELAY_MS > 0
			k_timer_start(&ac_unplug_delay_timer,
				      K_MSEC(ANIMATION_AC_UNPLUG_DELAY_MS),
				      K_NO_WAIT);
#else
			start_one_shot_animation(LED_ALT_POLICY_AC_UNPLUG_S3S5,
						 LED_ANIMATION_UNPLUG_MS,
						 LB_PRIO_STANDARD);
#endif
		}
	}
}

static K_WORK_DELAYABLE_DEFINE(debounced_ac_change_work,
			       lb_handle_debounced_ac_change);

static void led_alt_policy_ac_change(void)
{
	if (is_takeover) {
		return;
	}

	if (extpower_is_present()) {
		bool was_transient_drop = unplug_debounce_in_progress;

		unplug_debounce_in_progress = false;
		k_work_cancel_delayable(&debounced_ac_change_work);
#if ANIMATION_AC_UNPLUG_DELAY_MS > 0
		k_timer_stop(&ac_unplug_delay_timer);
#endif

		/* If we filtered an unplug, skip firing the redundant plug
		 * event */
		if (was_transient_drop) {
			return;
		}

		k_work_reschedule(&debounced_ac_change_work, K_NO_WAIT);
	} else {
		if (LED_ANIMATION_UNPLUG_DEBOUNCE_MS > 0) {
			unplug_debounce_in_progress = true;
			/* Hold the LED in a consistent state to avoid visual
			 * flickering while evaluating a potential PD hard reset
			 */
			if (active_one_shot_policy == LED_ALT_POLICY_NORMAL) {
				start_one_shot_animation(
					LED_ALT_POLICY_UNPLUG_DEBOUNCE,
					LED_ANIMATION_UNPLUG_DEBOUNCE_MS,
					LB_PRIO_STANDARD);
			}
			k_work_reschedule(
				&debounced_ac_change_work,
				K_MSEC(LED_ANIMATION_UNPLUG_DEBOUNCE_MS));
		} else {
			unplug_debounce_in_progress = false;
			k_work_reschedule(&debounced_ac_change_work, K_NO_WAIT);
		}
	}
}
DECLARE_HOOK(HOOK_AC_CHANGE, led_alt_policy_ac_change, HOOK_PRIO_DEFAULT);

/* Unblock charger events if warm reboot didn't finish in time */
static void warm_reboot_timer_expiry_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	ignore_charger_events = false;
}
static K_TIMER_DEFINE(warm_reboot_timer, warm_reboot_timer_expiry_handler,
		      NULL);

void lb_set_takeover(bool enable)
{
	if (is_takeover == enable) {
		return;
	}

	is_takeover = enable;
	pending_custom_pattern = NULL;
	if (enable) {
		unplug_debounce_in_progress = false;
		k_work_cancel_delayable(&debounced_ac_change_work);
#if ANIMATION_AC_UNPLUG_DELAY_MS > 0
		k_timer_stop(&ac_unplug_delay_timer);
#endif
		if (active_prio == LB_PRIO_STANDARD &&
		    active_one_shot_policy != LED_ALT_POLICY_LOGO_AC &&
		    active_one_shot_policy != LED_ALT_POLICY_LOGO_BATTERY &&
		    active_one_shot_policy != LED_ALT_POLICY_SHUTDOWN &&
		    k_timer_remaining_get(&one_shot_timer) > 0) {
			k_timer_stop(&one_shot_timer);
			reset_policy_patterns(EC_LED_ID_LIGHTBAR_LED);
		}
		update_idle_policy();
	} else {
		policy_sync_pending = false;
		update_idle_policy();
		if (!lb_is_ec_busy()) {
			reset_policy_patterns(EC_LED_ID_LIGHTBAR_LED);
		}
		led_set_custom_patterns(NULL);
	}
}

void lb_control_value_process(uint8_t control)
{
	switch (control) {
	case LB_CONTROL_IDLE_ENABLE:
		is_idle_disabled = false;
		ignore_charger_events = false;
		k_timer_stop(&warm_reboot_timer);
		break;
	case LB_CONTROL_IDLE_DISABLE:
		is_idle_disabled = true;
		break;
	case LB_CONTROL_BATTERY_SAVER_ENABLE:
		is_battery_saver = true;
		break;
	case LB_CONTROL_BATTERY_SAVER_DISABLE:
		is_battery_saver = false;
		break;
	case LB_CONTROL_REAL_RESUME:
		if (is_takeover) {
			is_real_resume = true;
			return;
		}
		trigger_startup_animation();
		return;
	case LB_CONTROL_WARM_REBOOT:
		lb_set_takeover(false);
		ignore_charger_events = true;
		k_timer_start(&warm_reboot_timer,
			      K_MSEC(WARM_REBOOT_TIMEOUT_MS), K_NO_WAIT);
		break;
	case LB_CONTROL_TAKEOVER_ENABLE:
		lb_set_takeover(true);
		return;
	case LB_CONTROL_TAKEOVER_RELEASE:
		lb_set_takeover(false);
		return;
	default:
		break;
	}

	update_idle_policy();
}
