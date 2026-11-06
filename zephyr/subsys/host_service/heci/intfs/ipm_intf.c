/* Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "../../host_service_common.h"
#include "bsp_helper.h"
#include "heci_intf.h"

#include <string.h>

#include <zephyr/drivers/ipm.h>
#include <zephyr/kernel.h>

#include <../drivers/ipm/ipm_sedi.h>
#include <host_bsp_service.h>
#include <sedi_driver_ipc.h>
#include <sedi_soc_funcs.h>

#define IPM_NAME DEVICE_DT_NAME(DT_NODELABEL(ipmhost))

static const struct device *dev;
static int ipm_intf_init(void);
/*
 * Serialize send_host_msg() across threads so PM_VNN_DRIVER_RESET() completes
 * before the next outbound host IPC transfer begins.
 */
static K_MUTEX_DEFINE(host_write_lock);

uint32_t in_drbl;
uint8_t *in_data;

static int read_host_msg(uint32_t *drbl, uint8_t *msg, uint32_t msg_size)
{
	*drbl = in_drbl;
	if (msg && msg_size) {
		if (in_data == NULL) {
			return -1;
		}
		memcpy(msg, in_data, msg_size);
	}
	return 0;
}

static int send_host_msg(uint32_t drbl, uint8_t *msg, uint32_t msg_size)
{
	struct ipm_sedi_context *ipm_ctx = dev->data;
	int ret;

	k_mutex_lock(&host_write_lock, K_FOREVER);
	ret = ipm_send(dev, 1, drbl, msg, msg_size);
	if (ret != 0) {
		/*
		 * Clear any stale completion token left in device_write_msg_sem
		 * when ipm_sedi_send()'s timeout cleanup
		 * (sedi_ipc_write_dbl(device, 0)) triggers
		 * SEDI_IPC_EVENT_MSG_PEER_ACKED before clearing
		 * IPM_WRITE_IN_PROC_BIT.
		 */
		k_sem_reset(&ipm_ctx->device_write_msg_sem);
	}

	/*
	 * Reset VNN_ID_IPC_HOST_W so both the write-1-to-toggle PMU_VNN_REQ
	 * hardware bit and its firmware counter return to 0 even if ipm_send()
	 * timed out or raced with D0ix entry.
	 */
	PM_VNN_DRIVER_RESET(VNN_ID_IPC_HOST_W);
	k_mutex_unlock(&host_write_lock);

	return ret;
}

static int send_host_ack(void)
{
	unsigned int key = irq_lock();

	/*
	 * Reset VNN_ID_IPC_HOST_R under IRQ lock after ipm_complete() so both
	 * the hardware bit and firmware counter return to 0 before the next
	 * inbound doorbell ISR can fire.
	 */
	ipm_complete(dev);
	PM_VNN_DRIVER_RESET(VNN_ID_IPC_HOST_R);
	irq_unlock(key);

	return 0;
}

#define FWST_REG_ADDR 0x4100034
#define FWST_READY 0x3
void ipm_ready_set(uint32_t is_ready)
{
	uint32_t fwst;

	if (is_ready == 0) {
		/* Not support */
		return;
	}

	fwst = sys_read32(FWST_REG_ADDR);
	sys_write32(fwst | FWST_READY, FWST_REG_ADDR);
}

HECI_INTF_DEFINE(host)
struct heci_bsp_t ipm_bsp = { .core_id = CONFIG_HECI_CORE_ID,
			      .peer_is_host = 1,
			      .max_fragment_size = IPC_DATA_LEN_MAX,
			      .poll_write_support = 0,
			      .mng_msg_support = 1,
			      .read_msg = read_host_msg,
			      .send_msg = send_host_msg,
			      .send_ack = send_host_ack,
			      .poll_send_msg = NULL,
			      .init = ipm_intf_init,
			      .set_ready = ipm_ready_set };

static void ipm_rx_handler(const struct device *dev, void *user_data,
			   uint32_t id, volatile void *data)
{
	ARG_UNUSED(data);
	in_drbl = id;
	in_data = (void *)data;
	send_heci_newmsg_notify(&ipm_bsp);
}

static int ipm_intf_init(void)
{
	dev = device_get_binding(IPM_NAME);
	ipm_register_callback(dev, ipm_rx_handler, NULL);
	ipm_set_enabled(dev, 1);
	return 0;
}
