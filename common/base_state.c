/* Copyright 2018 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "base_state.h"
#include "console.h"
#include "hooks.h"
#include "host_command.h"

#include <zephyr/shell/shell.h>

#define CPRINTS(format, args...) cprints(CC_MOTION_LID, format, ##args)

#ifdef CONFIG_BASE_ATTACHED_SWITCH
/* 1: base attached, 0: otherwise */
static int base_state;

int base_get_state(void)
{
	return base_state;
}

void base_set_state(int state)
{
	if (base_state == !!state)
		return;

	base_state = !!state;
	CPRINTS("base state: %stached", state ? "at" : "de");
	hook_notify(HOOK_BASE_ATTACHED_CHANGE);

	/* Notify host of mode change. This likely will wake it up. */
	host_set_single_event(EC_HOST_EVENT_MODE_CHANGE);
}
#endif

static int cmd_basestate_attach(const struct shell *sh, size_t argc,
				char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	base_force_state(EC_SET_BASE_STATE_ATTACH);
	return 0;
}

static int cmd_basestate_detach(const struct shell *sh, size_t argc,
				char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	base_force_state(EC_SET_BASE_STATE_DETACH);
	return 0;
}

static int cmd_basestate_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	base_force_state(EC_SET_BASE_STATE_RESET);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_basestate_cmds,
			       SHELL_CMD_ARG(attach, NULL,
					     "Force base state attached",
					     cmd_basestate_attach, 1, 0),
			       SHELL_CMD_ARG(detach, NULL,
					     "Force base state detached",
					     cmd_basestate_detach, 1, 0),
			       SHELL_CMD_ARG(reset, NULL, "Reset base state",
					     cmd_basestate_reset, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(
	basestate, &sub_basestate_cmds,
	SHELL_HELP("Manually force base state to attached, detached or reset.",
		   "[attach | detach | reset]"),
	NULL);

static enum ec_host_cmd_status
hostcmd_setbasestate(struct ec_host_cmd_handler_args *args)
{
	const struct ec_params_set_base_state *params = args->input_buf;

	if (params->cmd > EC_SET_BASE_STATE_RESET)
		return EC_HOST_CMD_INVALID_PARAM;

	base_force_state(params->cmd);

	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER_REQ_ONLY(EC_CMD_SET_BASE_STATE, hostcmd_setbasestate,
			     EC_VER_MASK(0), struct ec_params_set_base_state);
