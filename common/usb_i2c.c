/* Copyright 2016 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "console.h"
#include "consumer.h"
#include "i2c.h"
#include "link_defs.h"
#include "producer.h"
#include "queue.h"
#include "queue_policies.h"
#include "registers.h"
#include "task.h"
#include "timer.h"

#if defined(CONFIG_PLATFORM_EC_USB_I2C)
#include "drivers/usb_stream.h"
#elif !defined(BOARD_HOST)
#include "usb-stream.h"
#endif

#include "usb_descriptor.h"
#include "usb_i2c.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_I2C, format, ##args)

#ifndef BOARD_HOST
USB_I2C_CONFIG(i2c, USB_IFACE_I2C, USB_STR_I2C_NAME, USB_EP_I2C)
#endif

static int (*cros_cmd_handler)(void *data_in, size_t in_size, void *data_out,
			       size_t out_size);

static int16_t usb_i2c_map_error(int error)
{
	switch (error) {
	case EC_SUCCESS:
		return USB_I2C_SUCCESS;
	case EC_ERROR_TIMEOUT:
		return USB_I2C_TIMEOUT;
	case EC_ERROR_BUSY:
		return USB_I2C_BUSY;
	default:
		return USB_I2C_UNKNOWN_ERROR | (error & 0x7fff);
	}
}

/*
 * Return value should be large enough to accommodate the entire read queue
 * buffer size. Let's use 4 bytes in case future designs have a lot of RAM and
 * allow for large buffers.
 */
static uint32_t usb_i2c_read_packet(struct usb_i2c_config const *config,
				    size_t count)
{
	return QUEUE_REMOVE_UNITS(config->consumer.queue, config->buffer,
				  count);
}

static void usb_i2c_write_packet(struct usb_i2c_config const *config,
				 size_t count)
{
	QUEUE_ADD_UNITS(config->tx_queue, config->buffer, count);
}

static void usb_i2c_clear_tx(struct usb_i2c_config const *config)
{
	if (config->clear_tx)
		config->clear_tx();
	else if (!queue_is_empty(config->tx_queue))
		queue_advance_head(config->tx_queue,
				   queue_count(config->tx_queue));
}

static size_t usb_i2c_header_expected_size(const uint8_t peek[4])
{
	/* Header bytes and extra rc bytes, if present. */
	size_t expected_size = (peek[3] & 0x80) ? 6 : 4;

	/* Write count */
	expected_size += (((size_t)peek[0] & 0xf0) << 4) | peek[2];
	return expected_size;
}

static timestamp_t usb_i2c_get_rx_deadline(struct usb_i2c_state *state)
{
	timestamp_t deadline;
	uint32_t key = irq_lock();

	deadline = state->rx_deadline;
	irq_unlock(key);
	return deadline;
}

static void usb_i2c_reset_rx(struct usb_i2c_config const *config, size_t count)
{
	struct usb_i2c_state *state = config->state;

	if (count)
		queue_advance_head(config->consumer.queue, count);
	state->expected_size = 0;
	state->skip_bytes = 0;
}

static void usb_i2c_execute(struct usb_i2c_config const *config,
			    size_t packet_size)
{
	/* Payload is ready to execute. */
	uint32_t count = usb_i2c_read_packet(config, packet_size);
	int portindex = (config->buffer[0] >> 0) & 0xf;
	uint16_t addr_flags = (config->buffer[0] >> 8) & 0x7f;
	int write_count = ((config->buffer[0] << 4) & 0xf00) |
			  ((config->buffer[1] >> 0) & 0xff);
	int read_count = (config->buffer[1] >> 8) & 0xff;
	int offset = 0; /* Offset for extended reading header. */

	config->buffer[0] = 0;
	config->buffer[1] = 0;

	if (read_count & 0x80) {
		read_count = ((config->buffer[2] & 0xff) << 7) |
			     (read_count & 0x7f);
		offset = 2;
	}

	if (!count || (!read_count && !write_count))
		return;

	if (!usb_i2c_board_is_enabled()) {
		config->buffer[0] = USB_I2C_DISABLED;
	} else if (write_count > CONFIG_USB_I2C_MAX_WRITE_COUNT ||
		   write_count != (count - 4 - offset)) {
		config->buffer[0] = USB_I2C_WRITE_COUNT_INVALID;
	} else if (read_count > CONFIG_USB_I2C_MAX_READ_COUNT) {
		config->buffer[0] = USB_I2C_READ_COUNT_INVALID;
	} else if (portindex >= i2c_ports_used) {
		config->buffer[0] = USB_I2C_PORT_INVALID;
	} else if (addr_flags == USB_I2C_CMD_ADDR_FLAGS) {
		/*
		 * This is a non-i2c command, invoke the handler if it has
		 * been registered, if not - report the appropriate error.
		 */
		if (!cros_cmd_handler)
			config->buffer[0] = USB_I2C_MISSING_HANDLER;
		else
			config->buffer[0] = cros_cmd_handler(config->buffer + 2,
							     write_count,
							     config->buffer + 2,
							     read_count);
	} else {
		int ret;

		/*
		 * TODO (crbug.com/750397): Add security.  This currently
		 * blindly passes through ALL I2C commands on any bus the EC
		 * knows about.  It should behave closer to
		 * EC_CMD_I2C_PASSTHRU, which can protect ports and ranges.
		 */
		ret = i2c_xfer(i2c_ports[portindex].port, addr_flags,
			       (uint8_t *)(config->buffer + 2) + offset,
			       write_count, (uint8_t *)(config->buffer + 2),
			       read_count);
		config->buffer[0] = usb_i2c_map_error(ret);
	}

	if (read_count > CONFIG_USB_I2C_MAX_READ_COUNT)
		read_count = 0;

	if (config->buffer[0] != USB_I2C_SUCCESS && read_count > 0)
		memset(config->buffer + 2, 0, read_count);

	usb_i2c_write_packet(config, read_count + 4);
}

static bool usb_i2c_drain_skip(struct usb_i2c_config const *config,
			       size_t count)
{
	struct usb_i2c_state *state = config->state;
	size_t drop;

	if (!count)
		return false;

	drop = min(count, state->skip_bytes);
	queue_advance_head(config->consumer.queue, drop);
	state->skip_bytes -= drop;
	if (!state->skip_bytes) {
		config->buffer[0] = USB_I2C_WRITE_COUNT_INVALID;
		config->buffer[1] = 0;
		usb_i2c_write_packet(config, 4);
	}
	return true;
}

static bool usb_i2c_process_rx(struct usb_i2c_config const *config,
			       size_t count)
{
	struct usb_i2c_state *state = config->state;
	size_t packet_size;

	if (!state->expected_size) {
		uint8_t peek[4];
		size_t expected_size;

		if (count < sizeof(peek))
			return false;

		usb_i2c_clear_tx(config);
		queue_peek_units(config->consumer.queue, peek, 0, sizeof(peek));
		expected_size = usb_i2c_header_expected_size(peek);
		if (expected_size > config->consumer.queue->buffer_units) {
			state->skip_bytes = expected_size;
			return true;
		}
		state->expected_size = expected_size;
	}

	if (count < state->expected_size)
		return false;

	packet_size = state->expected_size;
	state->expected_size = 0;
	usb_i2c_execute(config, packet_size);
	return true;
}

void usb_i2c_deferred(struct usb_i2c_config const *config)
{
	struct usb_i2c_state *state = config->state;

	for (;;) {
		size_t count = queue_count(config->consumer.queue);
		timestamp_t deadline;
		timestamp_t now;
		uint64_t remaining;

		if (state->skip_bytes) {
			if (usb_i2c_drain_skip(config, count))
				continue;
		} else if (usb_i2c_process_rx(config, count)) {
			continue;
		}

		if (!count && !state->skip_bytes)
			return;

		/*
		 * Discard incomplete multi-packet writes or stalled oversized
		 * discards after USB_I2C_RX_TIMEOUT_US so an aborted host
		 * transfer cannot wedge or desynchronize future requests.
		 */
		deadline = usb_i2c_get_rx_deadline(state);
		now = get_time();

		if (timestamp_expired(deadline, &now)) {
			usb_i2c_reset_rx(config, count);
			continue;
		}

		remaining = min(deadline.val - now.val,
				(uint64_t)USB_I2C_RX_TIMEOUT_US);

		hook_call_deferred(config->deferred, (int)remaining);
		if (queue_count(config->consumer.queue) == count)
			return;
		hook_call_deferred(config->deferred, -1);
	}
}

static void usb_i2c_written(struct consumer const *consumer, size_t count)
{
	struct usb_i2c_config const *config =
		DOWNCAST(consumer, struct usb_i2c_config, consumer);
	timestamp_t deadline;
	uint32_t key;

	deadline.val = get_time().val + USB_I2C_RX_TIMEOUT_US;
	key = irq_lock();
	config->state->rx_deadline = deadline;
	irq_unlock(key);
	hook_call_deferred(config->deferred, 0);
}

struct consumer_ops const usb_i2c_consumer_ops = {
	.written = usb_i2c_written,
};

int usb_i2c_register_cros_cmd_handler(int (*cmd_handler)(
	void *data_in, size_t in_size, void *data_out, size_t out_size))
{
	if (cros_cmd_handler)
		return -1;
	cros_cmd_handler = cmd_handler;
	return 0;
}
