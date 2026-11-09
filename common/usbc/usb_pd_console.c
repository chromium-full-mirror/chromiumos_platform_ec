/* Copyright 2020 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "console.h"
#include "usb_common.h"
#include "usb_pd.h"
#include "usb_pd_dpm_sm.h"
#include "usb_pd_timer.h"
#include "usb_pe_sm.h"
#include "usb_prl_sm.h"
#include "usb_tc_sm.h"
#include "util.h"

#include <zephyr/shell/shell.h>

#ifndef TEST_USB_PD_CONSOLE
static
#endif
	int command_pd(const struct shell *sh, size_t argc, const char **argv)
{
	int port;
	char *e;
	int rv = 0;

	if (argc < 2)
		return -EINVAL;

	if (!strcasecmp(argv[1], "dump")) {
		if (argc >= 3) {
			int level = strtoi(argv[2], &e, 10);

			if (*e)
				return -EINVAL;

			if (level < DEBUG_DISABLE)
				level = DEBUG_DISABLE;
			else if (level > DEBUG_LEVEL_MAX)
				level = DEBUG_LEVEL_MAX;

			dpm_set_debug_level(level);
			prl_set_debug_level(level);
			pe_set_debug_level(level);
			tc_set_debug_level(level);
			shell_print(sh, "debug=%d", level);
			return 0;
		}
	} else if (IS_ENABLED(CONFIG_USB_PD_TRY_SRC) &&
		   !strcasecmp(argv[1], "trysrc")) {
		enum try_src_override_t ov = tc_get_try_src_override();

		if (argc >= 3) {
			ov = strtoi(argv[2], &e, 10);
			if (*e || ov > TRY_SRC_NO_OVERRIDE)
				return -EINVAL;
			tc_try_src_override(ov);
		}

		if (ov == TRY_SRC_NO_OVERRIDE)
			shell_print(sh, "Try.SRC System controlled");
		else
			shell_print(sh, "Try.SRC Forced %s", ov ? "ON" : "OFF");

		return 0;
	} else if (!strcasecmp(argv[1], "version")) {
		shell_print(sh, "%d", PD_STACK_VERSION);
		return 0;
	} else if (!strcasecmp(argv[1], "bistsharemode")) {
		if (!strcasecmp(argv[2], "disable"))
			rv = pd_set_bist_share_mode(0) ? -EINVAL : 0;
		else if (!strcasecmp(argv[2], "enable"))
			rv = pd_set_bist_share_mode(1) ? -EINVAL : 0;
		else
			rv = -EINVAL;
		return rv;
	}

	/* command: pd <port> <subcmd> [args] */
	port = strtoi(argv[1], &e, 10);
	if (argc < 3)
		return -EINVAL;

	if (*e || port >= board_get_usb_pd_port_count())
		return -EINVAL;

	if (IS_ENABLED(CONFIG_USB_PD_DUAL_ROLE)) {
		if (!strcasecmp(argv[2], "tx")) {
			pd_dpm_request(port, DPM_REQUEST_SNK_STARTUP);
		} else if (!strcasecmp(argv[2], "charger")) {
			pd_dpm_request(port, DPM_REQUEST_SRC_STARTUP);
		} else if (!strcasecmp(argv[2], "dev")) {
			int max_volt;

			if (argc >= 4) {
				max_volt = strtoi(argv[3], &e, 10) * 1000;
				if (*e)
					return -EINVAL;
			} else {
				max_volt = pd_get_max_voltage();
			}
			pd_request_source_voltage(port, max_volt);
			pd_dpm_request(port, DPM_REQUEST_NEW_POWER_LEVEL);
			shell_print(sh, "max req: %dmV", max_volt);
		} else if (!strcasecmp(argv[2], "disable")) {
			pd_comm_enable(port, 0);
			shell_print(sh, "Port C%d disable", port);
			return 0;
		} else if (!strcasecmp(argv[2], "enable")) {
			pd_comm_enable(port, 1);
			shell_print(sh, "Port C%d enabled", port);
			return 0;
		} else if (!strcasecmp(argv[2], "hard")) {
			pd_dpm_request(port, DPM_REQUEST_HARD_RESET_SEND);
		} else if (!strcasecmp(argv[2], "soft")) {
			pd_dpm_request(port, DPM_REQUEST_SOFT_RESET_SEND);
		} else if (!strcasecmp(argv[2], "swap")) {
			if (argc < 4)
				return -EINVAL;

			if (!strcasecmp(argv[3], "power"))
				pd_dpm_request(port, DPM_REQUEST_PR_SWAP);
			else if (!strcasecmp(argv[3], "data"))
				pd_dpm_request(port, DPM_REQUEST_DR_SWAP);
			else if (IS_ENABLED(CONFIG_USBC_VCONN_SWAP) &&
				 !strcasecmp(argv[3], "vconn"))
				pd_request_vconn_swap(port);
			else
				return -EINVAL;
		} else if (!strcasecmp(argv[2], "dualrole")) {
			if (argc < 4) {
				cflush();
				shell_fprintf(sh, SHELL_NORMAL,
					      "dual-role toggling: ");
				switch (pd_get_dual_role(port)) {
				case PD_DRP_TOGGLE_ON:
					shell_print(sh, "on");
					break;
				case PD_DRP_TOGGLE_OFF:
					shell_print(sh, "off");
					break;
				case PD_DRP_FREEZE:
					shell_print(sh, "freeze");
					break;
				case PD_DRP_FORCE_SINK:
					shell_print(sh, "force sink");
					break;
				case PD_DRP_FORCE_SOURCE:
					shell_print(sh, "force source");
					break;
				}
				cflush();
			} else {
				if (!strcasecmp(argv[3], "on"))
					pd_set_dual_role(port,
							 PD_DRP_TOGGLE_ON);
				else if (!strcasecmp(argv[3], "off"))
					pd_set_dual_role(port,
							 PD_DRP_TOGGLE_OFF);
				else if (!strcasecmp(argv[3], "freeze"))
					pd_set_dual_role(port, PD_DRP_FREEZE);
				else if (!strcasecmp(argv[3], "sink"))
					pd_set_dual_role(port,
							 PD_DRP_FORCE_SINK);
				else if (!strcasecmp(argv[3], "source"))
					pd_set_dual_role(port,
							 PD_DRP_FORCE_SOURCE);
				else
					return -EINVAL;
			}
			return 0;
		} else if (!strcasecmp(argv[2], "suspend")) {
			pd_comm_enable(port, 0);
			pd_set_suspend(port, 1);
		} else if (!strcasecmp(argv[2], "resume")) {
			pd_comm_enable(port, 1);
			pd_set_suspend(port, 0);
		}
	}

	if (!strcasecmp(argv[2], "state")) {
		cflush();
		shell_fprintf(
			sh, SHELL_NORMAL, "Port C%d CC%d, %s - Role: %s-%s",
			port, pd_get_polarity(port) + 1,
			pd_comm_is_enabled(port) ? "Enable" : "Disable",
			pd_get_power_role(port) == PD_ROLE_SOURCE ? "SRC" :
								    "SNK",
			pd_get_data_role(port) == PD_ROLE_DFP ? "DFP" : "UFP");

		if (IS_ENABLED(CONFIG_USBC_VCONN))
			shell_fprintf(sh, SHELL_NORMAL, "%s ",
				      tc_is_vconn_src(port) ? "-VC" : "");

		/* PD-FAFT requires a space before the TC State in the following
		 * print */
		shell_fprintf(sh, SHELL_NORMAL, " TC State: %s, Flags: 0x%04x",
			      tc_get_current_state(port), tc_get_flags(port));

		/* PD-FAFT requires a space before the PE State in the
		 * following print */
		shell_fprintf(sh, SHELL_NORMAL, " PE State: %s, Flags: 0x%04x",
			      pe_get_current_state(port), pe_get_flags(port));
		if (pe_is_explicit_contract(port)) {
			if (pe_snk_in_epr_mode(port))
				shell_fprintf(sh, SHELL_NORMAL, " EPR");
			else
				shell_fprintf(sh, SHELL_NORMAL, " SPR");
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");

		cflush();
	} else if (!strcasecmp(argv[2], "srccaps")) {
		pd_srccaps_dump(port);
	} else if (!strcasecmp(argv[2], "cc")) {
		shell_print(sh, "Port C%d CC%d", port,
			    pd_get_task_cc_state(port));
#ifdef CONFIG_USB_PD_EPR
	} else if (!strcasecmp(argv[2], "epr")) {
		enum pd_dpm_request req;

		if (argc < 4) {
			return -EINVAL;
		}
		if (pd_get_power_role(port) != PD_ROLE_SINK) {
			shell_print(sh,
				    "EPR is currently supported only for sink");
			/* Suppress (long) help message */
			return 0;
		}
		if (!strcasecmp(argv[3], "enter")) {
			req = DPM_REQUEST_EPR_MODE_ENTRY;
		} else if (!strcasecmp(argv[3], "exit")) {
			req = DPM_REQUEST_EPR_MODE_EXIT;
			/* Prevent snk_ready from repeatedly entering EPR. */
			pe_snk_epr_explicit_exit(port);
		} else {
			return -EINVAL;
		}
		pd_dpm_request(port, req);
		shell_print(sh, "EPR %s requested", argv[3]);
		return 0;
#endif
	}

	if (IS_ENABLED(CONFIG_CMD_PD_TIMER) && !strcasecmp(argv[2], "timer")) {
		pd_timer_dump(port);
	}

	return 0;
}
#ifndef TEST_USB_PD_CONSOLE
SHELL_CMD_ARG_REGISTER(pd, NULL,
		       "USB PD\n"
		       "Usage: pd version"
		       "\ndump [0|1|2|3]"
#ifdef CONFIG_USB_PD_TRY_SRC
		       "\ntrysrc [0|1|2]"
#endif
		       "\nbistsharemode [disable|enable]"
		       "\n\t<port> state"
		       "\n\t<port> srccaps"
		       "\n\t<port> cc"
#ifdef CONFIG_CMD_PD_TIMER
		       "\n\t<port> timer"
#endif /* CONFIG_CMD_PD_TIMER */
#ifdef CONFIG_USB_PD_DUAL_ROLE
		       "|tx|charger|dev"
		       "\n\t<port> disable|enable|soft|hard"
		       "\n\t<port> suspend|resume"
		       "\n\t<port> dualrole [on|off|freeze|sink|source]"
		       "\n\t<port> swap [power|data|vconn]"
#ifdef CONFIG_USB_PD_EPR
		       "\n\t<port> epr [enter|exit]"
#endif
#endif /* CONFIG_USB_PD_DUAL_ROLE */
		       ,
		       command_pd, 2, 2);
#endif
