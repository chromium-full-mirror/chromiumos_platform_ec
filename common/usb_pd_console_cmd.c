/* Copyright 2020 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Console commands for USB-PD module.
 */

#include "console.h"
#include "led_onoff_states.h"
#include "usb_dp_alt_mode.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "util.h"

#include <zephyr/shell/shell.h>

#ifdef CONFIG_USB_PD_ALT_MODE_DFP
#ifdef CONFIG_CMD_USB_PD_PE
static void dump_pe(const struct shell *sh, int port)
{
	int i, j, idh_ptype;
	const union disc_ident_ack *resp;
	enum tcpci_msg_type type;
	/* TODO(b/152417597): Output SOP' discovery results */
	const struct pd_discovery *disc =
		pd_get_am_discovery(port, TCPCI_MSG_SOP);

	static const char *const idh_ptype_names[] = { "UNDEF",	 "Hub",
						       "Periph", "PCable",
						       "ACable", "AMA",
						       "RSV6",	 "RSV7" };
	static const char *const tx_names[] = { "SOP", "SOP'", "SOP''" };

	for (type = TCPCI_MSG_SOP; type < DISCOVERY_TYPE_COUNT; type++) {
		resp = pd_get_identity_response(port, type);
		if (pd_get_identity_discovery(port, type) != PD_DISC_COMPLETE) {
			shell_print(sh, "No %s identity discovered yet.",
				    tx_names[type]);
			continue;
		}

		idh_ptype = resp->idh.product_type;
		shell_print(sh, "IDENT %s:", tx_names[type]);
		shell_print(sh, "\t[ID Header] %08x :: %s, VID:%04x",
			    resp->raw_value[0], idh_ptype_names[idh_ptype],
			    resp->idh.usb_vendor_id);

		shell_print(sh, "\t[Cert Stat] %08x", resp->cert.xid);
		for (i = 2; i < ARRAY_SIZE(resp->raw_value); i++) {
			shell_fprintf(sh, SHELL_NORMAL, "\t");
			if (resp->raw_value[i])
				shell_fprintf(sh, SHELL_NORMAL, "[%d] %08x ", i,
					      resp->raw_value[i]);
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}

	if (pd_get_svid_count(port, TCPCI_MSG_SOP) < 1) {
		shell_print(sh, "No SVIDS discovered yet.");
		return;
	}

	/* TODO(b/152418267): Display discovered SVIDs and modes for SOP' */
	for (i = 0; i < pd_get_svid_count(port, TCPCI_MSG_SOP); i++) {
		shell_fprintf(sh, SHELL_NORMAL, "SVID[%d]: %04x MODES:", i,
			      disc->svids[i].svid);
		for (j = 0; j < disc->svids[i].mode_cnt; j++)
			shell_fprintf(sh, SHELL_NORMAL, " [%d] %08x", j + 1,
				      disc->svids[i].mode_vdo[j]);
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}
}

static int command_pe(const struct shell *sh, size_t argc, const char **argv)
{
	int port;
	char *e;

	/* command: pe <port> <subcmd> <args> */
	port = strtoi(argv[1], &e, 10);
	if (*e || port < 0 || port >= board_get_usb_pd_port_count())
		return -EINVAL;
	if (!strncasecmp(argv[2], "dump", 4))
		dump_pe(sh, port);

	return 0;
}
SHELL_CMD_ARG_REGISTER(pe, NULL,
		       "USB PE\n"
		       "Usage: pe <port> dump",
		       command_pe, 3, 0);
#endif /* CONFIG_CMD_USB_PD_PE */

#ifdef CONFIG_CMD_USB_PD_CABLE
static const char *const cable_type[] = {
	[IDH_PTYPE_PCABLE] = "Passive",
	[IDH_PTYPE_ACABLE] = "Active",
};

static const char *const cable_curr[] = {
	[USB_VBUS_CUR_3A] = "3A",
	[USB_VBUS_CUR_5A] = "5A",
};

static const char *const dp21_cable_type[] = {
	[DP21_PASSIVE_CABLE] = "Passive",
	[DP21_ACTIVE_RETIMER_CABLE] = "Active-Retimer",
	[DP21_ACTIVE_REDRIVER_CABLE] = "Active-Redriver",
	[DP21_OPTICAL_CABLE] = "Optical",
};

static const char *const dp21_cable_speed[] = {
	[DP_HBR3] = "HBR3",
	[DP_UHBR10] = "UHBR10",
	[DP_UHBR20] = "UHBR20",
};

static int command_cable(const struct shell *sh, size_t argc, const char **argv)
{
	int port;
	char *e;
	const struct pd_discovery *disc;
	enum idh_ptype ptype;
	int cable_rev;
	union tbt_mode_resp_cable cable_tbt_mode_resp;
	union dp_mode_resp_cable cable_dp_mode_resp;
	uint8_t dp_bit_rate;
	enum dp21_cable_type active_comp;

	port = strtoi(argv[1], &e, 0);
	if (*e || port < 0 || port >= board_get_usb_pd_port_count())
		return -EINVAL;

	ptype = get_usb_pd_cable_type(port);

	shell_fprintf(sh, SHELL_NORMAL, "Cable Type: ");
	if (ptype != IDH_PTYPE_PCABLE && ptype != IDH_PTYPE_ACABLE) {
		shell_print(sh, "Not Emark Cable");
		return 0;
	}
	shell_print(sh, "%s", cable_type[ptype]);

	cable_rev = pd_get_rev(port, TCPCI_MSG_SOP_PRIME);
	disc = pd_get_am_discovery(port, TCPCI_MSG_SOP_PRIME);
	cable_tbt_mode_resp.raw_value =
		IS_ENABLED(CONFIG_USB_PD_TBT_COMPAT_MODE) ?
			pd_get_tbt_mode_vdo(port, TCPCI_MSG_SOP_PRIME) :
			0;
	cable_dp_mode_resp.raw_value =
		IS_ENABLED(CONFIG_USB_PD_DP21_MODE) ?
			dp_get_mode_vdo(port, TCPCI_MSG_SOP_PRIME) :
			0;

	/* Cable revision */
	shell_print(sh, "Cable Rev: %d.0", cable_rev + 1);

	/*
	 * For rev 2.0, rev 3.0 active and passive cables have same bits for
	 * connector type (Bit 19:18) and current handling capability bit 6:5
	 */
	shell_print(sh, "Connector Type: %d",
		    disc->identity.product_t1.p_rev20.connector);

	if (disc->identity.product_t1.p_rev20.vbus_cur) {
		shell_print(sh, "Cable Current: %s",
			    disc->identity.product_t1.p_rev20.vbus_cur >
					    ARRAY_SIZE(cable_curr) ?
				    "Invalid" :
				    cable_curr[disc->identity.product_t1.p_rev20
						       .vbus_cur]);
	} else
		shell_print(sh, "Cable Current: Invalid");

	/*
	 * For Rev 3.0 passive cables and Rev 2.0 active and passive cables,
	 * USB Superspeed Signaling support have same bits 2:0
	 */
	if (ptype == IDH_PTYPE_PCABLE)
		shell_print(sh, "USB Superspeed Signaling support: %d",
			    disc->identity.product_t1.p_rev20.ss);

	/*
	 * For Rev 3.0 active cables and Rev 2.0 active and passive cables,
	 * SOP" controller preset have same bit 3
	 */
	if (ptype == IDH_PTYPE_ACABLE)
		shell_print(sh, "SOP'' Controller: %s present",
			    disc->identity.product_t1.a_rev20.sop_p_p ? "" :
									"Not");

	if (cable_rev == PD_REV30) {
		/*
		 * For Rev 3.0 active and passive cables, Max Vbus vtg have
		 * same bits 10:9.
		 */
		shell_print(
			sh, "Max vbus voltage: %d",
			20 + 10 * disc->identity.product_t1.p_rev30.vbus_max);

		/* For Rev 3.0 Active cables */
		if (ptype == IDH_PTYPE_ACABLE) {
			shell_print(sh, "SS signaling: USB_SS_GEN%u",
				    disc->identity.product_t2.a2_rev30.usb_gen ?
					    2 :
					    1);
			shell_print(
				sh, "Number of SS lanes supported: %u",
				disc->identity.product_t2.a2_rev30.usb_lanes);
		}
	}

	if (IS_ENABLED(CONFIG_USB_PD_DP21_MODE) &&
	    cable_dp_mode_resp.raw_value) {
		enum dpam_version dp_ver =
			dp_resolve_dpam_version(port, TCPCI_MSG_SOP_PRIME);
		if (dp_ver == DPAM_VERSION_21) {
			shell_print(sh, "DPAM Version : %s",
				    (dp_ver ? "2.1 or higher" :
					      "2.0 or earlier"));
			dp_bit_rate = dp_get_cable_bit_rate(port);
			shell_print(sh, "DP Cable bitrate : %s",
				    ((dp_bit_rate <= DP_UHBR20) ?
					     dp21_cable_speed[dp_bit_rate] :
					     "Invalid"));
			shell_print(sh, "DP UHBR13.5 Support : %s",
				    (cable_dp_mode_resp.uhbr13_5_support ?
					     "True" :
					     "False"));
			active_comp = cable_dp_mode_resp.active_comp;
			shell_print(sh, "DP Cable Type : %s",
				    dp21_cable_type[active_comp]);
		}
	}

	if (!cable_tbt_mode_resp.raw_value)
		return 0;

	shell_print(sh, "Rounded support: %s",
		    cable_tbt_mode_resp.tbt_rounded ==
				    TBT_GEN3_GEN4_ROUNDED_NON_ROUNDED ?
			    "Yes" :
			    "No");

	shell_print(sh, "Optical cable: %s",
		    cable_tbt_mode_resp.tbt_cable == TBT_CABLE_OPTICAL ? "Yes" :
									 "No");

	shell_print(sh, "Retimer support: %s",
		    cable_tbt_mode_resp.retimer_type == USB_RETIMER ? "Yes" :
								      "No");

	shell_print(sh, "Link training: %s-directional",
		    cable_tbt_mode_resp.lsrx_comm == BIDIR_LSRX_COMM ? "Bi" :
								       "Uni");

	shell_print(sh, "Thunderbolt cable type: %s",
		    cable_tbt_mode_resp.tbt_active_passive == TBT_CABLE_ACTIVE ?
			    "Active" :
			    "Passive");

	return 0;
}

SHELL_CMD_ARG_REGISTER(pdcable, NULL,
		       "Cable Characteristics\n"
		       "Usage: pdcable <port>",
		       command_cable, 2, 0);
#endif /* CONFIG_CMD_USB_PD_CABLE */
#endif /* CONFIG_USB_PD_ALT_MODE_DFP */
