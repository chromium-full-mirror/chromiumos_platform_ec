/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Unit tests for USB I2C bridge framing, error handling, and wedge recovery.
 */

#include "common.h"
#include "consumer.h"
#include "hooks.h"
#include "i2c.h"
#include "producer.h"
#include "queue.h"
#include "queue_policies.h"
#include "test_util.h"
#include "timer.h"
#include "usb_i2c.h"
#include "util.h"

#define MOCK_I2C_ADDR 0x20
#define USB_PACKET_MAX 64

static int board_enabled = 1;
static int mock_i2c_ret = EC_SUCCESS;
static uint8_t mock_i2c_regs[512];
static size_t last_i2c_write_len;
static size_t last_i2c_read_len;
static size_t tx_count_during_xfer;
static int clear_tx_calls;

static struct queue const test_i2c_tx_q;
static struct queue const fallback_i2c_tx_q;

int usb_i2c_board_is_enabled(void)
{
	return board_enabled;
}

static int mock_i2c_xfer(int port, uint16_t addr_flags, const uint8_t *out,
			 int out_size, uint8_t *in, int in_size, int flags)
{
	tx_count_during_xfer =
		queue_count(&test_i2c_tx_q) + queue_count(&fallback_i2c_tx_q);

	if (port != I2C_PORT_EEPROM || addr_flags != MOCK_I2C_ADDR)
		return EC_ERROR_INVAL;

	if (mock_i2c_ret != EC_SUCCESS)
		return mock_i2c_ret;

	last_i2c_write_len = out_size;
	last_i2c_read_len = in_size;

	if (out_size > 0)
		memcpy(mock_i2c_regs, out,
		       min((size_t)out_size, sizeof(mock_i2c_regs)));
	if (in_size > 0) {
		for (int i = 0; i < in_size; i++)
			in[i] = mock_i2c_regs[i % sizeof(mock_i2c_regs)] ^ 0x5a;
	}

	return EC_SUCCESS;
}
DECLARE_TEST_I2C_XFER(mock_i2c_xfer);

/* Primary test instance with clear_tx callback wired up (like STM32 C2D2). */
static uint16_t test_i2c_buffer[USB_I2C_BUFFER_SIZE / 2];
static struct usb_i2c_state test_i2c_state;
static void test_i2c_deferred_(void);
DECLARE_DEFERRED(test_i2c_deferred_);
static struct queue const test_i2c_tx_q =
	QUEUE_NULL(USB_I2C_READ_BUFFER, uint8_t);
static struct queue const test_i2c_rx_q;

static void test_clear_tx(void)
{
	clear_tx_calls++;
	if (!queue_is_empty(&test_i2c_tx_q))
		queue_advance_head(&test_i2c_tx_q, queue_count(&test_i2c_tx_q));
}

static struct usb_i2c_config const test_i2c = {
	.buffer = test_i2c_buffer,
	.state = &test_i2c_state,
	.deferred = &test_i2c_deferred__data,
	.clear_tx = test_clear_tx,
	.consumer = {
		.queue = &test_i2c_rx_q,
		.ops = &usb_i2c_consumer_ops,
	},
	.tx_queue = &test_i2c_tx_q,
};

static struct queue const test_i2c_rx_q = QUEUE_DIRECT(
	USB_I2C_WRITE_BUFFER, uint8_t, null_producer, test_i2c.consumer);

static void test_i2c_deferred_(void)
{
	usb_i2c_deferred(&test_i2c);
}

/* Secondary instance without clear_tx callback (like DWC/host fallback). */
static uint16_t fallback_i2c_buffer[USB_I2C_BUFFER_SIZE / 2];
static struct usb_i2c_state fallback_i2c_state;
static void fallback_i2c_deferred_(void);
DECLARE_DEFERRED(fallback_i2c_deferred_);
static struct queue const fallback_i2c_tx_q =
	QUEUE_NULL(USB_I2C_READ_BUFFER, uint8_t);
static struct queue const fallback_i2c_rx_q;

static struct usb_i2c_config const fallback_i2c = {
	.buffer = fallback_i2c_buffer,
	.state = &fallback_i2c_state,
	.deferred = &fallback_i2c_deferred__data,
	.clear_tx = NULL,
	.consumer = {
		.queue = &fallback_i2c_rx_q,
		.ops = &usb_i2c_consumer_ops,
	},
	.tx_queue = &fallback_i2c_tx_q,
};

static struct queue const fallback_i2c_rx_q = QUEUE_DIRECT(
	USB_I2C_WRITE_BUFFER, uint8_t, null_producer, fallback_i2c.consumer);

static void fallback_i2c_deferred_(void)
{
	usb_i2c_deferred(&fallback_i2c);
}

static int send_usb_packets(struct queue const *rx_q, const uint8_t *data,
			    size_t len)
{
	size_t offset = 0;

	while (offset < len) {
		size_t chunk = min(len - offset, (size_t)USB_PACKET_MAX);
		int retries = 50;

		while (queue_space(rx_q) < chunk && retries-- > 0)
			crec_usleep(1 * MSEC);

		TEST_ASSERT(queue_space(rx_q) >= chunk);
		TEST_EQ(queue_add_units(rx_q, data + offset, chunk), chunk,
			"%zu");
		offset += chunk;
	}

	return EC_SUCCESS;
}

static int wait_for_tx(struct queue const *tx_q, size_t expected_len)
{
	int retries = 50;

	while (queue_count(tx_q) < expected_len && retries-- > 0)
		crec_usleep(1 * MSEC);

	TEST_EQ(queue_count(tx_q), expected_len, "%zu");
	return EC_SUCCESS;
}

static uint16_t read_status_le16(const uint8_t *resp)
{
	return (uint16_t)resp[0] | ((uint16_t)resp[1] << 8);
}

void before_test(void)
{
	hook_call_deferred(test_i2c.deferred, -1);
	hook_call_deferred(fallback_i2c.deferred, -1);
	queue_init(&test_i2c_rx_q);
	queue_init(&test_i2c_tx_q);
	queue_init(&fallback_i2c_rx_q);
	queue_init(&fallback_i2c_tx_q);
	memset(&test_i2c_state, 0, sizeof(test_i2c_state));
	memset(&fallback_i2c_state, 0, sizeof(fallback_i2c_state));
	memset(test_i2c_buffer, 0xaa, sizeof(test_i2c_buffer));
	memset(fallback_i2c_buffer, 0xaa, sizeof(fallback_i2c_buffer));
	memset(mock_i2c_regs, 0, sizeof(mock_i2c_regs));
	board_enabled = 1;
	mock_i2c_ret = EC_SUCCESS;
	last_i2c_write_len = 0;
	last_i2c_read_len = 0;
	tx_count_during_xfer = 0;
	clear_tx_calls = 0;
}

static int test_normal_write_and_read(void)
{
	uint8_t write_req[260];
	uint8_t resp[204];

	/* 260-byte multi-packet write (4-byte header + 256-byte ITE block). */
	write_req[0] = ((256 >> 4) & 0xf0) | 0;
	write_req[1] = MOCK_I2C_ADDR;
	write_req[2] = 256 & 0xff;
	write_req[3] = 0;
	for (size_t i = 0; i < 256; i++)
		write_req[4 + i] = (uint8_t)(i + 1);

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, write_req, sizeof(write_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(last_i2c_write_len, (size_t)256, "%zu");
	TEST_EQ(clear_tx_calls, 1, "%d");

	/* Extended read (200 bytes >= 128 uses 6-byte header with rc1). */
	uint8_t ext_read_req[6] = {
		0x00, MOCK_I2C_ADDR, 0x00, (200 & 0x7f) | 0x80, 200 >> 7, 0x00,
	};

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, ext_read_req,
				 sizeof(ext_read_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 204), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 204), (size_t)204,
		"%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(last_i2c_read_len, (size_t)200, "%zu");
	for (size_t i = 0; i < 200; i++)
		TEST_EQ(resp[4 + i], (uint8_t)((i + 1) ^ 0x5a), "%u");

	return EC_SUCCESS;
}

static int test_oversized_write_does_not_wedge(void)
{
	/*
	 * write_count = 600 exceeds USB_I2C_WRITE_BUFFER (512). Previously
	 * usb_i2c waited forever for queue_count >= 604, filling the 512-byte
	 * RX queue and permanently wedging EP_RX_NAK.
	 */
	uint8_t oversized[604];
	uint8_t resp[8];

	memset(oversized, 0xa5, sizeof(oversized));
	oversized[0] = ((600 >> 4) & 0xf0) | 0;
	oversized[1] = MOCK_I2C_ADDR;
	oversized[2] = 600 & 0xff;
	oversized[3] = 0;

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, oversized, sizeof(oversized)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_WRITE_COUNT_INVALID,
		"%u");
	TEST_ASSERT(queue_is_empty(&test_i2c_rx_q));

	/*
	 * Boundary check: write_count = 508 (MAX_WRITE_COUNT) with extended
	 * read header (6 + 508 = 514 > 512) must also drain without wedging.
	 */
	uint8_t boundary[514];

	memset(boundary, 0x5a, sizeof(boundary));
	boundary[0] = ((508 >> 4) & 0xf0) | 0;
	boundary[1] = MOCK_I2C_ADDR;
	boundary[2] = 508 & 0xff;
	boundary[3] = 0x80 | 4;
	boundary[4] = 1;
	boundary[5] = 0;

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, boundary, sizeof(boundary)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_WRITE_COUNT_INVALID,
		"%u");

	/* Subsequent normal 4-byte I2C read succeeds immediately. */
	uint8_t normal_req[4] = { 0x00, MOCK_I2C_ADDR, 0x00, 0x04 };

	mock_i2c_regs[0] = 0x11;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, normal_req,
				 sizeof(normal_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 8), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 8), (size_t)8, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(resp[4], (uint8_t)(0x11 ^ 0x5a), "%u");

	return EC_SUCCESS;
}

static int test_interrupted_partial_write_recovers(void)
{
	/*
	 * Simulate iteflash getting killed after sending only the first 64
	 * bytes of a 260-byte DFU block write (containing 0xff payload bytes
	 * that would decode to expected_size > 512 if misaligned).
	 */
	uint8_t partial_req[64];
	uint8_t full_req[260];
	uint8_t resp[4];

	memset(partial_req, 0xff, sizeof(partial_req));
	partial_req[0] = ((256 >> 4) & 0xf0) | 0;
	partial_req[1] = MOCK_I2C_ADDR;
	partial_req[2] = 256 & 0xff;
	partial_req[3] = 0;

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, partial_req,
				 sizeof(partial_req)),
		EC_SUCCESS, "%d");

	/* Let the incomplete RX transfer time out. */
	crec_usleep(USB_I2C_RX_TIMEOUT_US + 30 * MSEC);
	TEST_ASSERT(queue_is_empty(&test_i2c_rx_q));
	TEST_ASSERT(queue_is_empty(&test_i2c_tx_q));

	/* Next 260-byte multi-packet write executes without desync or wedge. */
	memset(full_req, 0xff, sizeof(full_req));
	full_req[0] = ((256 >> 4) & 0xf0) | 0;
	full_req[1] = MOCK_I2C_ADDR;
	full_req[2] = 256 & 0xff;
	full_req[3] = 0;

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, full_req, sizeof(full_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_ASSERT(queue_is_empty(&test_i2c_rx_q));

	return EC_SUCCESS;
}

static int test_misaligned_oversized_header_recovers(void)
{
	/*
	 * 4 stray bytes whose header bits claim expected_size = 970 > 512,
	 * followed by silence (host is not actually sending 966 more bytes).
	 */
	uint8_t stray[4] = { 0x3c, 0x92, 0xc6, 0x22 };
	uint8_t normal_req[4] = { 0x00, MOCK_I2C_ADDR, 0x00, 0x02 };
	uint8_t resp[6];

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, stray, sizeof(stray)),
		EC_SUCCESS, "%d");
	crec_usleep(USB_I2C_RX_TIMEOUT_US + 30 * MSEC);

	/* Timeout resets skip_bytes silently without queueing bogus TX data. */
	TEST_ASSERT(queue_is_empty(&test_i2c_rx_q));
	TEST_ASSERT(queue_is_empty(&test_i2c_tx_q));

	/* Next valid request succeeds immediately. */
	mock_i2c_regs[0] = 0x42;
	mock_i2c_regs[1] = 0x99;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, normal_req,
				 sizeof(normal_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 6), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 6), (size_t)6, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(resp[4], (uint8_t)(0x42 ^ 0x5a), "%u");
	TEST_EQ(resp[5], (uint8_t)(0x99 ^ 0x5a), "%u");

	return EC_SUCCESS;
}

static int test_stale_tx_cleared_before_next_response(void)
{
	uint8_t req1[5] = { 0x00, MOCK_I2C_ADDR, 0x01, 0x00, 0xaa };
	uint8_t req2[4] = { 0x00, MOCK_I2C_ADDR, 0x00, 0x01 };
	uint8_t resp[5];

	/*
	 * Execute req1 then req2 without draining req1's 4-byte response;
	 * stale TX must be cleared before i2c_xfer() runs on req2.
	 */
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, req1, sizeof(req1)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");

	tx_count_during_xfer = 99;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, req2, sizeof(req2)),
		EC_SUCCESS, "%d");
	crec_usleep(5 * MSEC);
	TEST_EQ(tx_count_during_xfer, (size_t)0, "%zu");
	TEST_EQ(queue_count(&test_i2c_tx_q), (size_t)5, "%zu");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 5), (size_t)5, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(resp[4], (uint8_t)(0xaa ^ 0x5a), "%u");

	/* Same check on fallback_i2c (NULL clear_tx callback). */
	TEST_EQ(send_usb_packets(&fallback_i2c_rx_q, req1, sizeof(req1)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&fallback_i2c_tx_q, 4), EC_SUCCESS, "%d");

	tx_count_during_xfer = 99;
	TEST_EQ(send_usb_packets(&fallback_i2c_rx_q, req2, sizeof(req2)),
		EC_SUCCESS, "%d");
	crec_usleep(5 * MSEC);
	TEST_EQ(tx_count_during_xfer, (size_t)0, "%zu");
	TEST_EQ(queue_count(&fallback_i2c_tx_q), (size_t)5, "%zu");
	TEST_EQ(queue_remove_units(&fallback_i2c_tx_q, resp, 5), (size_t)5,
		"%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(resp[4], (uint8_t)(0xaa ^ 0x5a), "%u");

	return EC_SUCCESS;
}

static int mock_cros_cmd_handler(void *data_in, size_t in_size, void *data_out,
				 size_t out_size)
{
	uint8_t *in = data_in;
	uint8_t *out = data_out;

	if (in_size == 1 && out_size == 1) {
		out[0] = in[0] + 1;
		return USB_I2C_SUCCESS;
	}
	return USB_I2C_UNSUPPORTED_COMMAND;
}

static int test_error_paths_and_cros_cmd(void)
{
	uint8_t resp[8];

	/* Zero-length read+write is a silent no-op. */
	uint8_t noop_req[4] = { 0x00, MOCK_I2C_ADDR, 0x00, 0x00 };

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, noop_req, sizeof(noop_req)),
		EC_SUCCESS, "%d");
	crec_usleep(5 * MSEC);
	TEST_ASSERT(queue_is_empty(&test_i2c_tx_q));

	/* Board disabled -> USB_I2C_DISABLED, read payload zeroed. */
	board_enabled = 0;
	memset(test_i2c_buffer, 0xaa, sizeof(test_i2c_buffer));
	uint8_t rd_req[4] = { 0x00, MOCK_I2C_ADDR, 0x00, 0x02 };

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, rd_req, sizeof(rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 6), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 6), (size_t)6, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_DISABLED, "%u");
	TEST_EQ(resp[4], 0, "%u");
	TEST_EQ(resp[5], 0, "%u");

	/*
	 * Board disabled + oversized extended read_count (1024 > 506) must
	 * clamp read_count to 0 without overflowing test_i2c_buffer.
	 */
	uint8_t big_rd_req[6] = {
		0x00, MOCK_I2C_ADDR, 0x00, 0x80, 1024 >> 7, 0x00,
	};

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, big_rd_req,
				 sizeof(big_rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_DISABLED, "%u");
	board_enabled = 1;

	/* Oversized read_count (1024 > 506) -> 4B USB_I2C_READ_COUNT_INVALID.
	 */
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, big_rd_req,
				 sizeof(big_rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 4), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 4), (size_t)4, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_READ_COUNT_INVALID,
		"%u");

	/* Invalid port -> USB_I2C_PORT_INVALID. */
	uint8_t bad_port_req[4] = { 0x0f, MOCK_I2C_ADDR, 0x00, 0x01 };

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, bad_port_req,
				 sizeof(bad_port_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 5), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 5), (size_t)5, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_PORT_INVALID, "%u");

	/* I2C error mapping: TIMEOUT, BUSY, UNKNOWN. */
	mock_i2c_ret = EC_ERROR_TIMEOUT;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, rd_req, sizeof(rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 6), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 6), (size_t)6, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_TIMEOUT, "%u");

	mock_i2c_ret = EC_ERROR_BUSY;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, rd_req, sizeof(rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 6), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 6), (size_t)6, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_BUSY, "%u");

	mock_i2c_ret = EC_ERROR_INVAL;
	TEST_EQ(send_usb_packets(&test_i2c_rx_q, rd_req, sizeof(rd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 6), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 6), (size_t)6, "%zu");
	TEST_EQ(read_status_le16(resp),
		(uint16_t)(USB_I2C_UNKNOWN_ERROR | EC_ERROR_UNKNOWN), "%u");
	mock_i2c_ret = EC_SUCCESS;

	/* Special command address before and after handler registration. */
	uint8_t cmd_req[5] = { 0x00, USB_I2C_CMD_ADDR_FLAGS, 0x01, 0x01, 0x41 };

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, cmd_req, sizeof(cmd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 5), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 5), (size_t)5, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_MISSING_HANDLER,
		"%u");

	TEST_EQ(usb_i2c_register_cros_cmd_handler(mock_cros_cmd_handler), 0,
		"%d");
	TEST_EQ(usb_i2c_register_cros_cmd_handler(mock_cros_cmd_handler), -1,
		"%d");

	TEST_EQ(send_usb_packets(&test_i2c_rx_q, cmd_req, sizeof(cmd_req)),
		EC_SUCCESS, "%d");
	TEST_EQ(wait_for_tx(&test_i2c_tx_q, 5), EC_SUCCESS, "%d");
	TEST_EQ(queue_remove_units(&test_i2c_tx_q, resp, 5), (size_t)5, "%zu");
	TEST_EQ(read_status_le16(resp), (uint16_t)USB_I2C_SUCCESS, "%u");
	TEST_EQ(resp[4], 0x42, "%u");

	return EC_SUCCESS;
}

void run_test(int argc, const char **argv)
{
	test_reset();

	RUN_TEST(test_normal_write_and_read);
	RUN_TEST(test_oversized_write_does_not_wedge);
	RUN_TEST(test_interrupted_partial_write_recovers);
	RUN_TEST(test_misaligned_oversized_header_recovers);
	RUN_TEST(test_stale_tx_cleared_before_next_response);
	RUN_TEST(test_error_paths_and_cros_cmd);

	test_print_result();
}
