/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Host command for the Lightbar Driver
 */

#include "ec_commands.h"
#include "host_command.h"
#include "led_common.h"
#include "led_lb_host_program.h"
#include "led_lightbar.h"
#include "lightbar_msg_list.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lightbar_hostcmd, LOG_LEVEL_INF);

#define LIGHTBAR_API_VERSION (3)

#define LBMSG(state) LIGHTBAR_CMD_SEQ_##state
#include "lightbar_msg_list.h"
enum lightbar_cmd_seq { LIGHTBAR_MSG_LIST };
#undef LBMSG

/* Data structure to store lightbar effect,
 * - as a raw buffer
 * - as a proto buffer
 */
static uint8_t custom_sequence_buffer[EC_LB_PROG_EX_LEN];
static size_t custom_sequence_buffer_size;

__overridable enum ec_status board_lightbar_custom_seq(uint8_t seq)
{
	return EC_RES_INVALID_PARAM;
}

static enum ec_status host_cmd_lightbar(struct host_cmd_handler_args *args)
{
	const struct ec_params_lightbar *in = args->params;
	struct ec_response_lightbar *out = args->response;
	struct led_rgb color = { .r = 0, .g = 0, .b = 0 };
	enum ec_status rv = EC_RES_SUCCESS;

	if (args->params_size < sizeof(in->cmd)) {
		LOG_ERR("Missing command");
		return EC_RES_REQUEST_TRUNCATED;
	}

	switch (in->cmd) {
	case LIGHTBAR_CMD_INIT:
		/* no-op, return SUCCESS */
		break;
	case LIGHTBAR_CMD_SET_BRIGHTNESS:
		/* Workaround for b:512005830 to avoid dimming on shutdown */
		if (0 == in->set_brightness.num) {
			LOG_WRN("LB: ignore brightness = 0 request");
			break;
		}
		lb_set_brightness_pct(CLAMP(in->set_brightness.num,
					    lb_data.brightness_min_pct, 100));
		break;
	case LIGHTBAR_CMD_GET_BRIGHTNESS:
		out->get_brightness.num = lb_get_brightness_pct();
		args->response_size = sizeof(out->get_brightness);
		break;
	case LIGHTBAR_CMD_SET_RGB:
		int8_t seg_id = in->set_rgb.led;
		/* Special case to reset animation. */
		if (seg_id == lb_get_num_segments() + 1) {
			reset_policy_patterns(EC_LED_ID_LIGHTBAR_LED);
			break;
		}
		/* TODO(b/462434336): The RGB color needs conversion */
		color.r = in->set_rgb.red;
		color.g = in->set_rgb.green;
		color.b = in->set_rgb.blue;

		/* Legacy behavior: if led >= num_segments, set all */
		if (seg_id >= lb_get_num_segments()) {
			seg_id = SEGMENT_ID_ALL;
		}
		led_auto_control(EC_LED_ID_LIGHTBAR_LED, 0);
		lb_set_and_apply_rgb(color, seg_id);
		break;
	case LIGHTBAR_CMD_VERSION:
		out->version.num = LIGHTBAR_API_VERSION;
		out->version.flags = lb_get_version_flags();
		args->response_size = sizeof(out->version);
		break;
	case LIGHTBAR_CMD_GET_RGB:
		/* Return the current RGB (regardless of brightness) */
		lb_get_rgb(&color);
		out->get_rgb.red = color.r;
		out->get_rgb.green = color.g;
		out->get_rgb.blue = color.b;
		args->response_size = sizeof(out->get_rgb);
		break;
	case LIGHTBAR_CMD_SET_PROGRAM_EX: {
		size_t hdr_size = offsetof(struct ec_params_lightbar,
					   set_program_ex.data);
		if (args->params_size < hdr_size) {
			LOG_ERR("Missing header");
			rv = EC_RES_REQUEST_TRUNCATED;
			break;
		}
		if (args->params_size < hdr_size + in->set_program_ex.size) {
			LOG_ERR("Truncated payload");
			rv = EC_RES_REQUEST_TRUNCATED;
			break;
		}
		/* Check data is valid to avoid memory corruption. */
		if (in->set_program_ex.offset >=
			    sizeof(custom_sequence_buffer) ||
		    in->set_program_ex.offset + in->set_program_ex.size >
			    sizeof(custom_sequence_buffer)) {
			LOG_ERR("Unexpected offset/size");
			rv = EC_RES_OVERFLOW;
			break;
		}

		memcpy(&custom_sequence_buffer[in->set_program_ex.offset],
		       in->set_program_ex.data, in->set_program_ex.size);
		custom_sequence_buffer_size =
			in->set_program_ex.size + in->set_program_ex.offset;
		break;
	}
	case LIGHTBAR_CMD_MANUAL_SUSPEND_CTRL:
	case LIGHTBAR_CMD_SUSPEND:
	case LIGHTBAR_CMD_RESUME:
		LOG_ERR("Command 0x%x ignored", in->cmd);
		break;
	case LIGHTBAR_CMD_SEQ: {
		if (in->seq.num == LIGHTBAR_CMD_SEQ_PROGRAM) {
			if (custom_sequence_buffer_size == 0) {
				break;
			}
			rv = host_program_sequence(custom_sequence_buffer,
						   custom_sequence_buffer_size);
			custom_sequence_buffer_size = 0;
		} else {
			rv = board_lightbar_custom_seq(in->seq.num);
			if (rv != EC_RES_SUCCESS) {
				LOG_ERR("Seq 0x%x ignored", in->seq.num);
			}
		}
		break;
	}
	case LIGHTBAR_CMD_GET_PARAMS_V3:
		LOG_ERR("LB_get_params_v3");
		out->get_params_v3.reported_led_num = lb_get_num_segments();
		args->response_size = sizeof(out->get_params_v3);
		break;
	default:
		LOG_ERR("LB unsupported host cmd 0x%x", in->cmd);
		rv = EC_RES_INVALID_PARAM;
		break;
	}

	return rv;
}

DECLARE_HOST_COMMAND(EC_CMD_LIGHTBAR_CMD, host_cmd_lightbar, EC_VER_MASK(0));
