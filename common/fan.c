/* Copyright 2013 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Basic Chrome OS fan control */

#include "builtin/assert.h"
#include "chipset.h"
#include "common.h"
#include "fan.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "printf.h"
#include "system.h"
#include "util.h"

#include <zephyr/shell/shell.h>

/* True if we're listening to the thermal control task. False if we're setting
 * things manually. */
static int thermal_control_enabled[CONFIG_FANS];

int is_thermal_control_enabled(int idx)
{
	return thermal_control_enabled[idx];
}

#ifdef CONFIG_FAN_UPDATE_PERIOD
/* Should we ignore the fans for a while? */
static int fan_update_counter[CONFIG_FANS];
#endif

/*
 * Number of fans.
 *
 * Use fan_get_count and fan_set_count to access it. It should be set only
 * before HOOK_INIT/HOOK_PRIO_DEFAULT.
 */
static int fan_count = CONFIG_FANS;

int fan_get_count(void)
{
	return fan_count;
}

void fan_set_count(int count)
{
	/* You can only decrease the count. */
	assert(count <= CONFIG_FANS);
	fan_count = count;
}

#ifndef CONFIG_FAN_RPM_CUSTOM
/* This is the default implementation. It's only called over [0,100].
 * Convert the percentage to a target RPM. We can't simply scale all
 * the way down to zero because most fans won't turn that slowly, so
 * we'll map [1,100] => [FAN_MIN,FAN_MAX], and [0] => "off".
 */
int fan_percent_to_rpm(int fan_index, int temp_ratio)
{
	int rpm, max, min;

	if (temp_ratio <= 0) {
		rpm = 0;
	} else {
		min = fans[fan_index].rpm->rpm_min;
		max = fans[fan_index].rpm->rpm_max;
		rpm = ((temp_ratio - 1) * max + (100 - temp_ratio) * min) / 99;
	}

	return rpm;
}
#endif /* CONFIG_FAN_RPM_CUSTOM */

int temp_ratio_to_rpm_hysteresis(const struct fan_step_1_1 *fan_table,
				 int num_fan_levels, int fan_index,
				 int temp_ratio, void (*on_change)(void))
{
	static int previous_temp_ratio;
	const int previous_rpm = fan_get_rpm_target(FAN_CH(fan_index));
	int rpm;

	if (temp_ratio <= fan_table[0].decreasing_temp_ratio_threshold) {
		rpm = 0;
	} else if (previous_rpm == 0 &&
		   temp_ratio < fan_table[0].increasing_temp_ratio_threshold) {
		rpm = 0;
	} else {
		/*
		 * Comparing temp_ratio and previous_temp_ratio, trichotomy:
		 *  1. decreasing path. (check the decreasing threshold)
		 *  2. increasing path. (check the increasing threshold)
		 *  3. invariant path. (return the current RPM)
		 */
		if (temp_ratio < previous_temp_ratio) {
			int i = num_fan_levels - 1;

			while (i > 0 &&
			       fan_table[i].decreasing_temp_ratio_threshold >=
				       temp_ratio) {
				i--;
			}
			rpm = fan_table[i].rpm;
		} else if (temp_ratio > previous_temp_ratio) {
			int i = 0;

			while (i < num_fan_levels &&
			       fan_table[i].increasing_temp_ratio_threshold <=
				       temp_ratio) {
				i++;
			}
			if (i > 0) {
				i--;
			}
			rpm = fan_table[i].rpm;
		} else {
			rpm = previous_rpm;
		}
	}

	previous_temp_ratio = temp_ratio;

	if (rpm != previous_rpm) {
		cprints(CC_THERMAL, "Setting fan %d RPM to %d", fan_index, rpm);
		if (on_change) {
			on_change();
		}
	}

	return rpm;
}

static void set_enabled(int fan, int enable)
{
	fan_set_enabled(FAN_CH(fan), enable);

	if (fans[fan].conf->enable_gpio != GPIO_UNIMPLEMENTED)
		gpio_set_level(fans[fan].conf->enable_gpio, enable);
}

/* The thermal task will only call this function with pct in [0,100]. */
test_mockable void fan_set_percent_needed(int fan, int pct)
{
	int actual_rpm, new_rpm;

	if (!is_thermal_control_enabled(fan))
		return;

#ifdef CONFIG_FAN_UPDATE_PERIOD
	/* Only set each fan every so often, to avoid rapid changes. */
	fan_update_counter[fan] %= CONFIG_FAN_UPDATE_PERIOD;
	if (fan_update_counter[fan]++)
		return;
#endif

	new_rpm = fan_percent_to_rpm(fan, pct);
	actual_rpm = fan_get_rpm_actual(FAN_CH(fan));

	/* If we want to turn and the fans are currently significantly below
	 * the minimum turning speed, we should turn at least as fast as the
	 * necessary start speed instead. */
	if (new_rpm && actual_rpm < fans[fan].rpm->rpm_min * 9 / 10 &&
	    new_rpm < fans[fan].rpm->rpm_start)
		new_rpm = fans[fan].rpm->rpm_start;

	if (fans[fan].conf->fan_force_off) {
		/* enable the fan if rpm is non-zero */
		set_enabled(fan, (new_rpm > 0) ? 1 : 0);
	}
	fan_set_rpm_target(FAN_CH(fan), new_rpm);
}

test_export_static void set_thermal_control_enabled(int fan, int enable)
{
	thermal_control_enabled[fan] = enable;

	/* If controlling the fan, need it in RPM-control mode */
	if (enable)
		fan_set_rpm_mode(FAN_CH(fan), 1);
}

static void set_duty_cycle(int fan, int percent)
{
	/* Move the fan to manual control */
	fan_set_rpm_mode(FAN_CH(fan), 0);

	/* enable the fan when non-zero duty */
	set_enabled(fan, (percent > 0) ? 1 : 0);

	/* Disable thermal engine automatic fan control. */
	set_thermal_control_enabled(fan, 0);

	/* Set the duty cycle */
	fan_set_duty(FAN_CH(fan), percent);
}

/*****************************************************************************/
/* Console commands */

static int cc_fanauto(const struct shell *sh, size_t argc, char **argv)
{
	char *e;
	int fan = 0;

	if (fan_count > 1) {
		if (argc < 2) {
			shell_print(sh,
				    "fan number is required as the first arg");
			return -EINVAL;
		}
		fan = strtoi(argv[1], &e, 0);
		if (*e || fan >= fan_count)
			return -EINVAL;
		argc--;
		argv++;
	}

	set_thermal_control_enabled(fan, 1);
	return 0;
}
SHELL_CMD_ARG_REGISTER(fanauto, NULL,
		       SHELL_HELP("Enable thermal fan control", "{fan}"),
		       cc_fanauto, 1, 1);

/* Return 0 for off, 1 for on, -1 for unknown */
static int is_powered(int fan)
{
	int is_pgood = -1;

	/* If we have an enable output, see if it's on or off. */
	if (fans[fan].conf->enable_gpio >= 0)
		is_pgood = gpio_get_level(fans[fan].conf->enable_gpio);
	/* If we have a pgood input, it overrides any enable output. */
	if (fans[fan].conf->pgood_gpio >= 0)
		is_pgood = gpio_get_level(fans[fan].conf->pgood_gpio);

	return is_pgood;
}

static int cc_faninfo(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const human_status[] = { "not spinning", "changing",
						    "locked", "frustrated" };
	int tmp, is_pgood;
	int fan;
	char leader[20] = "";
	for (fan = 0; fan < fan_count; fan++) {
		if (fan_count > 1)
			if (snprintf(leader, sizeof(leader), "Fan %d ", fan) <
			    0)
				leader[0] = '\0';
		if (fan)
			shell_fprintf(sh, SHELL_NORMAL, "\n");
		shell_print(sh, "%sActual: %4d rpm", leader,
			    fan_get_rpm_actual(FAN_CH(fan)));
		shell_print(sh, "%sTarget: %4d rpm", leader,
			    fan_get_rpm_target(FAN_CH(fan)));
		shell_print(sh, "%sDuty:   %d%%", leader,
			    fan_get_duty(FAN_CH(fan)));
		tmp = fan_get_status(FAN_CH(fan));
		shell_print(sh, "%sStatus: %d (%s)", leader, tmp,
			    human_status[tmp]);
		shell_print(sh, "%sMode:   %s", leader,
			    fan_get_rpm_mode(FAN_CH(fan)) ? "rpm" : "duty");
		shell_print(sh, "%sAuto:   %s", leader,
			    is_thermal_control_enabled(fan) ? "yes" : "no");
		shell_print(sh, "%sEnable: %s", leader,
			    fan_get_enabled(FAN_CH(fan)) ? "yes" : "no");
		is_pgood = is_powered(fan);
		if (is_pgood >= 0)
			shell_print(sh, "%sPower:  %s", leader,
				    is_pgood ? "yes" : "no");
	}

	return 0;
}
SHELL_CMD_REGISTER(faninfo, NULL, "Print fan info", cc_faninfo);

static int cc_fanset(const struct shell *sh, size_t argc, char **argv)
{
	const char *rpm_str;
	int rpm;
	char *e;
	int fan = 0;

	if (fan_count == 0) {
		shell_print(sh, "Fan count is zero");
		return -EINVAL;
	}

	if (fan_count > 1) {
		if (argc < 3) {
			shell_print(sh,
				    "fan number is required as the first arg");
			return -EINVAL;
		}
	}

	if (argc == 3) {
		fan = strtoi(argv[1], &e, 0);
		if (*e || fan >= fan_count)
			return -EINVAL;
		rpm_str = argv[2];
	} else {
		rpm_str = argv[1];
	}

	rpm = strtoi(rpm_str, &e, 0);
	if (*e == '%') { /* Wait, that's a percentage */
		shell_print(sh, "Fan rpm given as %d%%", rpm);
		if (rpm < 0)
			rpm = 0;
		else if (rpm > 100)
			rpm = 100;
		rpm = fan_percent_to_rpm(fan, rpm);
	} else if (*e) {
		return -EINVAL;
	}

	/* Move the fan to automatic control */
	fan_set_rpm_mode(FAN_CH(fan), 1);

	/* enable the fan when non-zero rpm */
	set_enabled(fan, (rpm > 0) ? 1 : 0);

	/* Disable thermal engine automatic fan control. */
	set_thermal_control_enabled(fan, 0);

	fan_set_rpm_target(FAN_CH(fan), rpm);

	shell_print(sh, "Setting fan %d rpm target to %d", fan, rpm);

	return 0;
}
SHELL_CMD_ARG_REGISTER(fanset, NULL,
		       SHELL_HELP("Set fan speed", "[fan] (rpm | pct%)"),
		       cc_fanset, 2, 1);

static int cc_fanduty(const struct shell *sh, size_t argc, char **argv)
{
	const char *percent_str;
	int percent = 0;
	char *e;
	int fan = 0;

	if (fan_count == 0) {
		shell_print(sh, "Fan count is zero");
		return -EINVAL;
	}

	if (fan_count > 1) {
		if (argc < 3) {
			shell_print(sh,
				    "fan number is required as the first arg");
			return -EINVAL;
		}
	}

	if (argc == 3) {
		fan = strtoi(argv[1], &e, 0);
		if (*e || fan >= fan_count)
			return -EINVAL;
		percent_str = argv[2];
	} else {
		percent_str = argv[1];
	}

	percent = strtoi(percent_str, &e, 0);
	if (*e)
		return -EINVAL;

	shell_print(sh, "Setting fan %d duty cycle to %d%%", fan, percent);
	set_duty_cycle(fan, percent);

	return 0;
}
SHELL_CMD_ARG_REGISTER(fanduty, NULL,
		       SHELL_HELP("Set fan duty cycle", "[fan] percent"),
		       cc_fanduty, 2, 1);

/*****************************************************************************/
/* DPTF interface functions */

/* 0-100% if in duty mode. -1 if not */
int dptf_get_fan_duty_target(void)
{
	int fan = 0; /* TODO(crosbug.com/p/23803) */

	if (fan_count == 0)
		return -1;

	if (is_thermal_control_enabled(fan) || fan_get_rpm_mode(FAN_CH(fan)))
		return -1;

	return fan_get_duty(FAN_CH(fan));
}

/* 0-100% sets duty, out of range means let the EC drive */
void dptf_set_fan_duty_target(int pct)
{
	int fan;

	if (pct < 0 || pct > 100) {
		/* TODO(crosbug.com/p/23803) */
		for (fan = 0; fan < fan_count; fan++)
			set_thermal_control_enabled(fan, 1);
	} else {
		/* TODO(crosbug.com/p/23803) */
		for (fan = 0; fan < fan_count; fan++)
			set_duty_cycle(fan, pct);
	}
}

/*****************************************************************************/
/* Host commands */

static enum ec_host_cmd_status
hc_pwm_get_fan_target_rpm(struct ec_host_cmd_handler_args *args)
{
	struct ec_response_pwm_get_fan_rpm *r = args->output_buf;

	if (fan_count == 0)
		return EC_HOST_CMD_ERROR;

	/* TODO(crosbug.com/p/23803) */
	r->rpm = fan_get_rpm_target(FAN_CH(0));
	args->output_buf_size = sizeof(*r);

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER_RESP_ONLY(EC_CMD_PWM_GET_FAN_TARGET_RPM,
			      hc_pwm_get_fan_target_rpm, EC_VER_MASK(0),
			      struct ec_response_pwm_get_fan_rpm);

static enum ec_host_cmd_status
hc_pwm_set_fan_target_rpm(struct ec_host_cmd_handler_args *args)
{
	const struct ec_params_pwm_set_fan_target_rpm_v1 *p_v1 =
		args->input_buf;
	const struct ec_params_pwm_set_fan_target_rpm_v0 *p_v0 =
		args->input_buf;
	int fan;

	if (args->version == 0) {
		for (fan = 0; fan < fan_count; fan++) {
			/* enable the fan if rpm is non-zero */
			set_enabled(fan, (p_v0->rpm > 0) ? 1 : 0);

			set_thermal_control_enabled(fan, 0);
			fan_set_rpm_mode(FAN_CH(fan), 1);
			fan_set_rpm_target(FAN_CH(fan), p_v0->rpm);
		}

		return EC_HOST_CMD_SUCCESS;
	}

	if (args->input_buf_size <
	    sizeof(struct ec_params_pwm_set_fan_target_rpm_v1))
		return EC_HOST_CMD_REQUEST_TRUNCATED;

	fan = p_v1->fan_idx;
	if (fan >= fan_count)
		return EC_HOST_CMD_ERROR;

	/* enable the fan if rpm is non-zero */
	set_enabled(fan, (p_v1->rpm > 0) ? 1 : 0);

	set_thermal_control_enabled(fan, 0);
	fan_set_rpm_mode(FAN_CH(fan), 1);
	fan_set_rpm_target(FAN_CH(fan), p_v1->rpm);

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER_REQ_ONLY(
	EC_CMD_PWM_SET_FAN_TARGET_RPM, hc_pwm_set_fan_target_rpm,
	EC_VER_MASK(0) | EC_VER_MASK(1),
	SMALLEST_TYPE(struct ec_params_pwm_set_fan_target_rpm_v0,
		      struct ec_params_pwm_set_fan_target_rpm_v1));

static enum ec_host_cmd_status
hc_pwm_set_fan_duty(struct ec_host_cmd_handler_args *args)
{
	const struct ec_params_pwm_set_fan_duty_v1 *p_v1 = args->input_buf;
	const struct ec_params_pwm_set_fan_duty_v0 *p_v0 = args->input_buf;
	int fan;

	if (args->version == 0) {
		for (fan = 0; fan < fan_count; fan++)
			set_duty_cycle(fan, p_v0->percent);

		return EC_HOST_CMD_SUCCESS;
	}

	if (args->input_buf_size < sizeof(struct ec_params_pwm_set_fan_duty_v1))
		return EC_HOST_CMD_REQUEST_TRUNCATED;

	fan = p_v1->fan_idx;
	if (fan >= fan_count)
		return EC_HOST_CMD_ERROR;

	set_duty_cycle(fan, p_v1->percent);

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER_REQ_ONLY(
	EC_CMD_PWM_SET_FAN_DUTY, hc_pwm_set_fan_duty,
	EC_VER_MASK(0) | EC_VER_MASK(1),
	SMALLEST_TYPE(struct ec_params_pwm_set_fan_duty_v0,
		      struct ec_params_pwm_set_fan_duty_v1));

static enum ec_host_cmd_status
hc_pwm_get_fan_duty(struct ec_host_cmd_handler_args *args)
{
	const struct ec_params_pwm_get_fan_duty *req = args->input_buf;
	struct ec_response_pwm_get_fan_duty *resp = args->output_buf;

	if (req->fan_idx >= fan_count)
		return EC_HOST_CMD_INVALID_PARAM;

	resp->percent = fan_get_duty(FAN_CH(req->fan_idx));
	args->output_buf_size = sizeof(*resp);

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER(EC_CMD_PWM_GET_FAN_DUTY, hc_pwm_get_fan_duty,
		    EC_VER_MASK(0), struct ec_params_pwm_get_fan_duty,
		    struct ec_response_pwm_get_fan_duty);

static enum ec_host_cmd_status
hc_thermal_auto_fan_ctrl(struct ec_host_cmd_handler_args *args)
{
	int fan;
	const struct ec_params_auto_fan_ctrl_v2 *req = args->input_buf;
	struct ec_response_auto_fan_control *resp = args->output_buf;

	if (args->version == 1) {
		if (args->input_buf_size <
		    sizeof(struct ec_params_auto_fan_ctrl_v1))
			return EC_HOST_CMD_REQUEST_TRUNCATED;
	} else if (args->version == 2) {
		if (args->input_buf_size <
		    sizeof(struct ec_params_auto_fan_ctrl_v2))
			return EC_HOST_CMD_REQUEST_TRUNCATED;
	}

	if (args->version == 0) {
		for (fan = 0; fan < fan_count; fan++)
			set_thermal_control_enabled(fan, 1);

		return EC_HOST_CMD_SUCCESS;
	}

	/* v1 and v2 share the same fan_idx at same byte location. */
	fan = req->fan_idx;
	if (fan >= fan_count)
		return EC_HOST_CMD_INVALID_PARAM;

	if (args->version == 1) {
		set_thermal_control_enabled(fan, 1);
		return EC_HOST_CMD_SUCCESS;
	}

	if (req->cmd == EC_AUTO_FAN_CONTROL_CMD_SET) {
		set_thermal_control_enabled(fan, req->set_auto);
	} else if (req->cmd == EC_AUTO_FAN_CONTROL_CMD_GET) {
		if (args->output_buf_max < sizeof(*resp))
			return EC_HOST_CMD_RESPONSE_TOO_BIG;
		resp->is_auto = is_thermal_control_enabled(fan);
		args->output_buf_size = sizeof(*resp);
	} else {
		return EC_HOST_CMD_INVALID_PARAM;
	}

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER_UNBOUND(EC_CMD_THERMAL_AUTO_FAN_CTRL,
			    hc_thermal_auto_fan_ctrl,
			    EC_VER_MASK(0) | EC_VER_MASK(1) | EC_VER_MASK(2));

/*****************************************************************************/
/* Hooks */

/* We only have a limited number of memory-mapped slots to report fan speed to
 * the AP. If we have more fans than that, some will be inaccessible. But
 * if we're using that many fans, we probably have bigger problems.
 */
BUILD_ASSERT(CONFIG_FANS <= EC_FAN_SPEED_ENTRIES);

#define PWMFAN_SYSJUMP_TAG 0x5046 /* "PF" */
#define PWM_HOOK_VERSION 1
/* Saved PWM state across sysjumps */
struct pwm_fan_state {
	/* TODO(crosbug.com/p/23530): Still treating all fans as one. */
	uint16_t rpm;
	uint8_t flag; /* FAN_STATE_FLAG_* */
};

/* For struct pwm_fan_state.flag */
#define FAN_STATE_FLAG_ENABLED BIT(0)
#define FAN_STATE_FLAG_THERMAL BIT(1)

test_export_static void pwm_fan_init(void)
{
	const struct pwm_fan_state *prev;
	struct pwm_fan_state state;
	uint16_t *mapped;
	int version, size;
	int i;
	int fan;

	for (fan = 0; fan < fan_count; fan++)
		fan_channel_setup(FAN_CH(fan), fans[fan].conf->flags);

	/* Restore previous state. */
	prev = (const struct pwm_fan_state *)system_get_jump_tag(
		PWMFAN_SYSJUMP_TAG, &version, &size);
	if (prev && version == PWM_HOOK_VERSION && size == sizeof(*prev)) {
		memcpy(&state, prev, sizeof(state));
	} else {
		memset(&state, 0, sizeof(state));
	}

	for (fan = 0; fan < fan_count; fan++) {
		fan_set_enabled(FAN_CH(fan),
				state.flag & FAN_STATE_FLAG_ENABLED);
		fan_set_rpm_target(FAN_CH(fan), state.rpm);
		set_thermal_control_enabled(
			fan, state.flag & FAN_STATE_FLAG_THERMAL);
	}

	/* Initialize memory-mapped data */
	mapped = (uint16_t *)host_get_memmap(EC_MEMMAP_FAN);
	for (i = 0; i < EC_FAN_SPEED_ENTRIES; i++)
		mapped[i] = EC_FAN_SPEED_NOT_PRESENT;
}
DECLARE_HOOK(HOOK_INIT, pwm_fan_init, HOOK_PRIO_DEFAULT);

static void pwm_fan_second(void)
{
	uint16_t *mapped = (uint16_t *)host_get_memmap(EC_MEMMAP_FAN);
	uint16_t rpm;
	int stalled = 0;
	int fan;

	for (fan = 0; fan < fan_count; fan++) {
		if (fan_is_stalled(FAN_CH(fan))) {
			rpm = EC_FAN_SPEED_STALLED;
			stalled = 1;
			cprints(CC_PWM, "Fan %d stalled!", fan);
		} else {
			rpm = fan_get_rpm_actual(FAN_CH(fan));
		}

		mapped[fan] = rpm;
	}

	/*
	 * Issue warning.  As we have thermal shutdown
	 * protection, issuing warning here should be enough.
	 */
	if (stalled)
		host_set_single_event(EC_HOST_EVENT_THERMAL);
}
DECLARE_HOOK(HOOK_SECOND, pwm_fan_second, HOOK_PRIO_DEFAULT);

static void pwm_fan_preserve_state(void)
{
	struct pwm_fan_state state = { 0 };
	int fan = 0;

	if (fan_count == 0)
		return;

	/* TODO(crosbug.com/p/23530): Still treating all fans as one. */
	if (fan_get_enabled(FAN_CH(fan)))
		state.flag |= FAN_STATE_FLAG_ENABLED;
	if (is_thermal_control_enabled(fan))
		state.flag |= FAN_STATE_FLAG_THERMAL;
	state.rpm = fan_get_rpm_target(FAN_CH(fan));

	system_add_jump_tag(PWMFAN_SYSJUMP_TAG, PWM_HOOK_VERSION, sizeof(state),
			    &state);
}
DECLARE_HOOK(HOOK_SYSJUMP, pwm_fan_preserve_state, HOOK_PRIO_DEFAULT);

static void pwm_fan_control(int enable)
{
	int fan;

	/* TODO(crosbug.com/p/23530): Still treating all fans as one. */
	for (fan = 0; fan < fan_count; fan++) {
		set_thermal_control_enabled(fan, enable);
		fan_set_rpm_target(
			FAN_CH(fan),
			enable ?
				fan_percent_to_rpm(FAN_CH(fan),
						   fans[fan].conf->init_speed) :
				0);
		set_enabled(fan, enable);
	}
}

static void pwm_fan_stop(void)
{
	/*
	 * There is no need to cool CPU in S3 or S5. We currently don't
	 * have fans for battery or charger chip. Battery systems will
	 * control charge current based on its own temperature readings.
	 * Thus, we do not need to keep fans running in S3 or S5.
	 *
	 * Even with a fan on charging system, it's questionable to run
	 * a fan in S3/S5. Under an extreme heat condition, spinning a
	 * fan would create more heat as it draws current from a
	 * battery and heat would come from ambient air instead of CPU.
	 *
	 * Thermal control may be already disabled if DPTF is used.
	 */
	pwm_fan_control(0); /* crosbug.com/p/8097 */
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, pwm_fan_stop, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, pwm_fan_stop, HOOK_PRIO_DEFAULT);

static void pwm_fan_start(void)
{
	/*
	 * Even if the DPTF is enabled, enable thermal control here.
	 * Upon booting to S0, if needed AP will disable/throttle it using
	 * host commands.
	 */
	if (chipset_in_or_transitioning_to_state(CHIPSET_STATE_ON))
		pwm_fan_control(1);
}
/* On Fizz, CHIPSET_RESUME isn't triggered when AP warm resets.
 * So we hook CHIPSET_RESET instead.
 */
DECLARE_HOOK(HOOK_CHIPSET_RESET, pwm_fan_start, HOOK_PRIO_FIRST);
DECLARE_HOOK(HOOK_CHIPSET_RESUME, pwm_fan_start, HOOK_PRIO_DEFAULT);
