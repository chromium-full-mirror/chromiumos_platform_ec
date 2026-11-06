/* Copyright 2013 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* ADC module for Chrome EC */

#include "adc.h"
#include "common.h"
#include "ec_commands.h"
#include "host_command.h"
#include "util.h"

#include <zephyr/shell/shell.h>

/* 'adc' console command is not supported in continuous mode */
#ifndef CONFIG_ADC_PROFILE_FAST_CONTINUOUS

static int print_one_adc(const struct shell *sh, int channel)
{
	int v;

	v = adc_read_channel(channel);
	if (v == ADC_READ_ERROR)
		return -EIO;
	shell_print(sh, "[%d] %s = %d mV", channel, adc_channels[channel].name,
		    v);
	return 0;
}

static int command_adc(const struct shell *sh, size_t argc, char **argv)
{
	int i, ret;

	/* If a channel is specified, read only that one */
	if (argc == 2) {
		char *end;

		i = strtoi(argv[1], &end, 0);
		if (*end || i < 0 || i >= ADC_CH_COUNT)
			return -EINVAL;
		return print_one_adc(sh, i);
	} else {
		/* Otherwise print them all */
		for (i = 0; i < ADC_CH_COUNT; ++i) {
			ret = print_one_adc(sh, i);
			if (ret)
				return ret;
		}
		return 0;
	}
}
SHELL_CMD_ARG_REGISTER(adc, NULL, SHELL_HELP("Print ADC channels", "[id]"),
		       command_adc, 1, 1);

static enum ec_host_cmd_status
hc_adc_read(struct ec_host_cmd_handler_args *args)
{
	const struct ec_params_adc_read *params = args->input_buf;
	struct ec_response_adc_read *resp = args->output_buf;
	enum adc_channel ch = (enum adc_channel)params->adc_channel;
	int32_t adc_value;

	if (ch >= ADC_CH_COUNT)
		return EC_HOST_CMD_INVALID_PARAM;

	adc_value = adc_read_channel(ch);
	if (adc_value == ADC_READ_ERROR)
		return EC_HOST_CMD_ERROR;

	resp->adc_value = adc_value;
	args->output_buf_size = sizeof(*resp);
	return EC_HOST_CMD_SUCCESS;
}
EC_HOST_CMD_HANDLER(EC_CMD_ADC_READ, hc_adc_read, EC_VER_MASK(0),
		    struct ec_params_adc_read, struct ec_response_adc_read);
#endif /* CONFIG_ADC_PROFILE_FAST_CONTINUOUS */
