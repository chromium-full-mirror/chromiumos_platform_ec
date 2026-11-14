/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "chipset.h"
#include "ec_app_main.h"
#include "ec_commands.h"
#include "hooks.h"
#include "host_command.h"
#include "host_command_heci.h"
#include "mkbp_event.h"

#include <string.h>

#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <heci.h>

#define TEST_CONN_ID 7
#define CROS_EC_COMMAND_CH 1
#define CROS_MKBP_EVENT_CH 2
#define TEST_SWITCH_VAL 0x5a

struct test_ishtp_hdr {
	uint8_t channel;
	uint8_t status;
	uint8_t id;
	uint8_t reserved;
} __packed;

static heci_event_cb_t registered_event_cb;
static struct heci_rx_msg_t *registered_rx_msg;

static uint8_t last_cmd_tx_buf[280];
static uint32_t last_cmd_tx_len;
static uint32_t last_cmd_tx_conn_id;

static struct test_ishtp_hdr last_mkbp_hdr;
static uint32_t last_mkbp_len;
static uint32_t last_mkbp_conn_id;
static int mkbp_tx_count;

FAKE_VALUE_FUNC(bool, heci_send_flow_control, uint32_t);
FAKE_VALUE_FUNC(int, heci_complete_disconnect, uint32_t);
FAKE_VOID_FUNC(sedi_pm_reset);

static int custom_heci_register(struct heci_client_t *client)
{
	registered_event_cb = client->event_cb;
	registered_rx_msg = client->rx_msg;
	return 0;
}
FAKE_VALUE_FUNC(int, heci_register, struct heci_client_t *);

static bool custom_heci_send(uint32_t conn_id, struct mrd_t *msg)
{
	last_cmd_tx_conn_id = conn_id;
	last_cmd_tx_len = MIN(msg->len, sizeof(last_cmd_tx_buf));
	memcpy(last_cmd_tx_buf, msg->buf, last_cmd_tx_len);
	return true;
}
FAKE_VALUE_FUNC(bool, heci_send, uint32_t, struct mrd_t *);

static bool custom_heci_send_timeout_ok(uint32_t conn_id, struct mrd_t *msg,
					k_timeout_t timeout)
{
	const struct test_ishtp_hdr *hdr = msg->buf;

	ARG_UNUSED(timeout);
	if (hdr->channel == CROS_MKBP_EVENT_CH) {
		last_mkbp_conn_id = conn_id;
		last_mkbp_len = msg->len;
		memcpy(&last_mkbp_hdr, msg->buf, sizeof(last_mkbp_hdr));
		mkbp_tx_count++;
	}
	return true;
}

static bool custom_heci_send_timeout_fail(uint32_t conn_id, struct mrd_t *msg,
					  k_timeout_t timeout)
{
	ARG_UNUSED(conn_id);
	ARG_UNUSED(msg);
	ARG_UNUSED(timeout);
	mkbp_tx_count++;
	return false;
}
FAKE_VALUE_FUNC(bool, heci_send_timeout, uint32_t, struct mrd_t *, k_timeout_t);

static int test_switch_get_data(uint8_t *data)
{
	uint32_t val = TEST_SWITCH_VAL;

	memcpy(data, &val, sizeof(val));
	return sizeof(val);
}
DECLARE_EVENT_SOURCE(EC_MKBP_EVENT_SWITCH, test_switch_get_data);

static uint8_t calc_ec_request_checksum(const uint8_t *buf, size_t len)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += buf[i];
	return (uint8_t)(-sum);
}

static void send_heci_host_cmd(uint16_t cmd, uint8_t ver, const void *params,
			       uint16_t params_len, uint8_t req_id)
{
	struct test_ishtp_hdr *hdr;
	struct ec_host_request *req;
	size_t total_len = sizeof(*hdr) + sizeof(*req) + params_len;

	zassert_not_null(registered_rx_msg);
	zassert_not_null(registered_event_cb);

	memset(registered_rx_msg->buffer, 0, total_len);
	hdr = (struct test_ishtp_hdr *)registered_rx_msg->buffer;
	hdr->channel = CROS_EC_COMMAND_CH;
	hdr->id = req_id;

	req = (struct ec_host_request *)(hdr + 1);
	req->struct_version = EC_HOST_REQUEST_VERSION;
	req->command = cmd;
	req->command_version = ver;
	req->data_len = params_len;
	if (params && params_len > 0)
		memcpy(req + 1, params, params_len);
	req->checksum = calc_ec_request_checksum((const uint8_t *)req,
						 sizeof(*req) + params_len);

	registered_rx_msg->type = HECI_REQUEST;
	registered_rx_msg->connection_id = TEST_CONN_ID;
	registered_rx_msg->msg_lock = MSG_LOCKED;
	registered_rx_msg->length = total_len;

	registered_event_cb(HECI_EVENT_NEW_MSG, NULL);
	/* Allow cros_ec_ishtp_client thread and HOSTCMD task to process. */
	k_sleep(K_MSEC(50));
}

static void send_sleep_event(enum host_sleep_event evt)
{
	struct ec_params_host_sleep_event_v1 p = {
		.sleep_event = evt,
	};

	send_heci_host_cmd(EC_CMD_HOST_SLEEP_EVENT, 1, &p, sizeof(p), 0x42);
}

static void drain_next_mkbp_event_over_heci(uint8_t expected_type,
					    uint32_t expected_switches)
{
	const struct test_ishtp_hdr *resp_hdr;
	const struct ec_host_response *ec_resp;
	const struct ec_response_get_next_event_v1 *evt;

	send_heci_host_cmd(EC_CMD_GET_NEXT_EVENT, 2, NULL, 0, 0x55);

	resp_hdr = (const struct test_ishtp_hdr *)last_cmd_tx_buf;
	zassert_equal(CROS_EC_COMMAND_CH, resp_hdr->channel);
	zassert_equal(0x55, resp_hdr->id);

	ec_resp = (const struct ec_host_response *)(resp_hdr + 1);
	zassert_equal(EC_RES_SUCCESS, ec_resp->result);

	evt = (const struct ec_response_get_next_event_v1 *)(ec_resp + 1);
	zassert_equal(expected_type, evt->event_type & EC_MKBP_EVENT_TYPE_MASK);
	if (expected_type == EC_MKBP_EVENT_SWITCH)
		zassert_equal(expected_switches, evt->data.switches);
}

static void *heci_test_setup(void)
{
	ec_app_main();

	zassert_not_null(registered_event_cb);
	zassert_not_null(registered_rx_msg);

	/* Establish HECI connection TEST_CONN_ID. */
	registered_rx_msg->type = HECI_CONNECT;
	registered_rx_msg->connection_id = TEST_CONN_ID;
	registered_rx_msg->msg_lock = MSG_LOCKED;
	registered_event_cb(HECI_EVENT_NEW_MSG, NULL);
	k_sleep(K_MSEC(20));

	return NULL;
}

static int install_early_fakes(void)
{
	heci_register_fake.custom_fake = custom_heci_register;
	return 0;
}
SYS_INIT(install_early_fakes, APPLICATION, 0);

static void heci_test_before(void *f)
{
	ARG_UNUSED(f);

	RESET_FAKE(heci_send_flow_control);
	RESET_FAKE(heci_complete_disconnect);
	RESET_FAKE(heci_send);
	RESET_FAKE(heci_send_timeout);

	heci_send_flow_control_fake.return_val = true;
	heci_send_fake.custom_fake = custom_heci_send;
	heci_send_timeout_fake.custom_fake = custom_heci_send_timeout_ok;

	memset(last_cmd_tx_buf, 0, sizeof(last_cmd_tx_buf));
	last_cmd_tx_len = 0;
	last_cmd_tx_conn_id = 0;

	memset(&last_mkbp_hdr, 0xff, sizeof(last_mkbp_hdr));
	last_mkbp_len = 0;
	last_mkbp_conn_id = 0;
	mkbp_tx_count = 0;

	send_sleep_event(HOST_SLEEP_EVENT_S0IX_RESUME);
	mkbp_event_clear_all();
	RESET_FAKE(heci_send);
	RESET_FAKE(heci_send_flow_control);
	heci_send_flow_control_fake.return_val = true;
	heci_send_fake.custom_fake = custom_heci_send;
	mkbp_tx_count = 0;
}

ZTEST_SUITE(host_command_heci, NULL, heci_test_setup, heci_test_before, NULL,
	    NULL);

ZTEST(host_command_heci, test_protocol_info_and_heci_events)
{
	const struct test_ishtp_hdr *resp_hdr;
	const struct ec_host_response *ec_resp;
	const struct ec_response_get_protocol_info *proto;

	/* Unlocked message and unknown channel must be ignored safely. */
	registered_rx_msg->msg_lock = MSG_UNLOCKED;
	registered_event_cb(HECI_EVENT_NEW_MSG, NULL);
	k_sleep(K_MSEC(10));
	zassert_equal(0, heci_send_fake.call_count);

	memset(registered_rx_msg->buffer, 0, 16);
	((struct test_ishtp_hdr *)registered_rx_msg->buffer)->channel = 0x99;
	registered_rx_msg->type = HECI_REQUEST;
	registered_rx_msg->msg_lock = MSG_LOCKED;
	registered_rx_msg->length = sizeof(struct test_ishtp_hdr);
	registered_event_cb(HECI_EVENT_NEW_MSG, NULL);
	k_sleep(K_MSEC(10));
	zassert_equal(0, heci_send_fake.call_count);

	/* Unknown HECI event and disconnect event. */
	registered_event_cb(0x80, NULL);
	k_sleep(K_MSEC(10));

	registered_event_cb(HECI_EVENT_DISCONN, NULL);
	k_sleep(K_MSEC(10));
	zassert_equal(1, heci_complete_disconnect_fake.call_count);

	/* Reconnect and query EC_CMD_GET_PROTOCOL_INFO. */
	registered_rx_msg->type = HECI_CONNECT;
	registered_rx_msg->connection_id = TEST_CONN_ID;
	registered_rx_msg->msg_lock = MSG_LOCKED;
	registered_event_cb(HECI_EVENT_NEW_MSG, NULL);
	k_sleep(K_MSEC(10));

	send_heci_host_cmd(EC_CMD_GET_PROTOCOL_INFO, 0, NULL, 0, 0x77);
	zassert_equal(1, heci_send_fake.call_count);
	zassert_equal(TEST_CONN_ID, last_cmd_tx_conn_id);

	resp_hdr = (const struct test_ishtp_hdr *)last_cmd_tx_buf;
	zassert_equal(CROS_EC_COMMAND_CH, resp_hdr->channel);
	zassert_equal(0x77, resp_hdr->id);

	ec_resp = (const struct ec_host_response *)(resp_hdr + 1);
	zassert_equal(EC_RES_SUCCESS, ec_resp->result);
	proto = (const struct ec_response_get_protocol_info *)(ec_resp + 1);
	zassert_equal(BIT(3), proto->protocol_versions);
}

ZTEST(host_command_heci, test_mkbp_awake_send_and_busy)
{
	uint32_t ts = 0;

	/* Verify end-to-end MKBP event header, zero-padding, and drain. */
	mkbp_send_event(EC_MKBP_EVENT_SWITCH);
	zassert_equal(1, mkbp_tx_count);
	zassert_equal(TEST_CONN_ID, last_mkbp_conn_id);
	zassert_equal(sizeof(struct test_ishtp_hdr), last_mkbp_len);
	zassert_equal(CROS_MKBP_EVENT_CH, last_mkbp_hdr.channel);
	zassert_equal(0, last_mkbp_hdr.status);
	zassert_equal(0, last_mkbp_hdr.id);
	zassert_equal(0, last_mkbp_hdr.reserved);

	drain_next_mkbp_event_over_heci(EC_MKBP_EVENT_SWITCH, TEST_SWITCH_VAL);

	/* Credit exhaustion in S0 returns EC_ERROR_BUSY for retry. */
	heci_send_timeout_fake.custom_fake = custom_heci_send_timeout_fail;
	zassert_equal(EC_ERROR_BUSY, heci_send_mkbp_event(&ts));
}

ZTEST(host_command_heci, test_mkbp_s0ix_deferral)
{
	/* Unhandled sleep event value (0) leaves chipset state ON. */
	send_sleep_event(0);
	zassert_equal(CHIPSET_STATE_ON, chipset_in_state(CHIPSET_STATE_ON));

	/*
	 * Enter S0ix via EC_CMD_HOST_SLEEP_EVENT. Raise an MKBP event and wait
	 * across two 1-second deferred retries: chipset_in_state() must block
	 * HECI doorbell sends throughout S0ix.
	 */
	send_sleep_event(HOST_SLEEP_EVENT_S0IX_SUSPEND);
	zassert_equal(CHIPSET_STATE_SUSPEND,
		      chipset_in_state(CHIPSET_STATE_SUSPEND));

	mkbp_send_event(EC_MKBP_EVENT_SWITCH);
	k_sleep(K_MSEC(2500));
	zassert_equal(0, mkbp_tx_count);

	/*
	 * Resume to S0: the 1-second deferred retry now delivers the pending
	 * MKBP event over HECI and the host can drain its payload.
	 */
	send_sleep_event(HOST_SLEEP_EVENT_S0IX_RESUME);
	zassert_equal(CHIPSET_STATE_ON, chipset_in_state(CHIPSET_STATE_ON));
	k_sleep(K_MSEC(1200));
	zassert_equal(1, mkbp_tx_count);
	drain_next_mkbp_event_over_heci(EC_MKBP_EVENT_SWITCH, TEST_SWITCH_VAL);
}
