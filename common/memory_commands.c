/* Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* System module for Chrome EC */

#include "console.h"
#include "system.h"
#include "timer.h"
#include "util.h"
#include "watchdog.h"

enum format {
	FMT_WORD,
	FMT_HALF,
	FMT_BYTE,
	FMT_STRING,
};

#ifdef CONFIG_CMD_MD
static void show_val(const struct shell *sh, uint32_t address, uint32_t index,
		     enum format fmt)
{
	uint32_t val;
	uintptr_t ptr = address;

	switch (fmt) {
	case FMT_WORD:
		if (0 == (index % 4))
			shell_fprintf(sh, SHELL_NORMAL,
				      "\n%08X:", address + index * 4);
		val = *((uint32_t *)ptr + index);
		shell_fprintf(sh, SHELL_NORMAL, " %08x", val);
		break;
	case FMT_HALF:
		if (0 == (index % 8))
			shell_fprintf(sh, SHELL_NORMAL,
				      "\n%08X:", address + index * 2);
		val = *((uint16_t *)ptr + index);
		shell_fprintf(sh, SHELL_NORMAL, " %04x", val);
		break;
	case FMT_BYTE:
		if (0 == (index % 16))
			shell_fprintf(sh, SHELL_NORMAL,
				      "\n%08X:", address + index);
		val = *((uint8_t *)ptr + index);
		shell_fprintf(sh, SHELL_NORMAL, " %02x", val);
		break;
	case FMT_STRING:
		if (0 == (index % 32))
			shell_fprintf(sh, SHELL_NORMAL,
				      "\n%08X: ", address + index);
		val = *((uint8_t *)ptr + index);
		if (val >= ' ' && val <= '~')
			shell_fprintf(sh, SHELL_NORMAL, "%c", val);
		else
			shell_fprintf(sh, SHELL_NORMAL, "\\x%02x", val);
		break;
	}
	cflush();
}

static int command_mem_dump(const struct shell *sh, size_t argc,
			    const char **argv)
{
	uint32_t address, i, num = 1;
	char *e;
	enum format fmt = FMT_WORD;

#ifdef CONFIG_BOARD_FINGERPRINT
	if (system_is_locked())
		return -EACCES;
#endif /* CONFIG_BOARD_FINGERPRINT */

	if (argc > 1) {
		if ((argv[1][0] == '.') && (strlen(argv[1]) == 2)) {
			switch (argv[1][1]) {
			case 'b':
				fmt = FMT_BYTE;
				break;
			case 'h':
				fmt = FMT_HALF;
				break;
			case 's':
				fmt = FMT_STRING;
				break;
			default:
				return -EINVAL;
			}
			argc--;
			argv++;
		}
	}

	if (argc < 2)
		return -EINVAL;

	address = strtoi(argv[1], &e, 0);
	if (*e)
		return -EINVAL;

	if (argc >= 3)
		num = strtoi(argv[2], &e, 0);

	for (i = 0; i < num; i++) {
		show_val(sh, address, i, fmt);
		/* Lots of output could take a while.
		 * Let other things happen, too */
		if (!(i % 0x100)) {
			watchdog_reload();
			k_usleep(10 * MSEC);
		}
	}
	shell_fprintf(sh, SHELL_NORMAL, "\n");
	cflush();
	return 0;
}

SHELL_CMD_REGISTER(
	md, NULL,
	SHELL_HELP("dump memory values, optionally specifying the format",
		   "[.b|.h|.s] addr [count]"),
	command_mem_dump);
#endif /* CONFIG_CMD_MD */

#ifdef CONFIG_CMD_RW
static int command_read_word(const struct shell *sh, size_t argc,
			     const char **argv)
{
	volatile uint32_t *address;
	uint32_t value;
	unsigned int access_size = 4;
	unsigned int argc_offs = 0;
	char *e;

#ifdef CONFIG_BOARD_FINGERPRINT
	if (system_is_locked())
		return -EACCES;
#endif /* CONFIG_BOARD_FINGERPRINT */

	if (argc < 2)
		return -EINVAL;

	if (argc > 2) {
		if ((argv[1][0] == '.') && (strlen(argv[1]) == 2)) {
			argc_offs = 1;
			switch (argv[1][1]) {
			case 'b':
				access_size = 1;
				break;
			case 'h':
				access_size = 2;
				break;
			default:
				return -EINVAL;
			}
		}
	}

	address = (uint32_t *)(uintptr_t)strtoi(argv[1 + argc_offs], &e, 0);
	if (*e)
		return -EINVAL;

	/* Just reading? */
	if ((argc - argc_offs) < 3) {
		switch (access_size) {
		case 1:
			shell_print(sh, "read 0x%p = 0x%02x", address,
				    *((uint8_t *)address));
			break;
		case 2:
			shell_print(sh, "read 0x%p = 0x%04x", address,
				    *((uint16_t *)address));
			break;

		default:
			shell_print(sh, "read 0x%p = 0x%08x", address,
				    *address);
			break;
		}
		return 0;
	}

	/* Writing! */
	value = strtoi(argv[2 + argc_offs], &e, 0);
	if (*e)
		return -EINVAL;

	switch (access_size) {
	case 1:
		shell_print(sh, "write 0x%p = 0x%02x", address, (uint8_t)value);
		cflush(); /* Flush before writing in case this crashes */
		*((uint8_t *)address) = (uint8_t)value;
		break;
	case 2:
		shell_print(sh, "write 0x%p = 0x%04x", address,
			    (uint16_t)value);
		cflush();
		*((uint16_t *)address) = (uint16_t)value;
		break;
	default:
		shell_print(sh, "write 0x%p = 0x%02x", address, value);
		cflush();
		*address = value;
		break;
	}

	return 0;
}

SHELL_CMD_REGISTER(
	rw, NULL,
	SHELL_HELP(
		"Read or write a word in memory optionally specifying the size",
		"[.b|.h] addr [value]"),
	command_read_word);
#endif /* CONFIG_CMD_RW */
