/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenAMP rpmsg endpoint on the E907 (virtio device / remote side).
 *
 * Linux remoteproc reads the .resource_table built by
 * CONFIG_OPENAMP_RSC_TABLE, fills the vring addresses from the
 * vdev0vring0/vdev0vring1 carveouts and copies the table back into this
 * image before it starts the core.  rpmsg buffers come from the
 * vdev0buffer carveout at 0x43000000.
 *
 * The remote announces an rpmsg endpoint named "rpmsg_chrdev"; the A7
 * creates /dev/rpmsg_ctrl0 from it.  Endpoints created by the A7 are kept
 * in a small pool so a client reconnect never clobbers a live endpoint.
 * Messages are handed to the Linkr Bee UART bridge (linkr_bridge.c);
 * before the A7 handshake they are echoed back so the standalone rpmsg
 * test tools keep working.
 *
 * Kick/notify goes over the V851S MSGBOX via the IPM device (ipm0).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/logging/log.h>

#include <metal/io.h>
#include <metal/device.h>
#include <openamp/open_amp.h>
#include <resource_table.h>

#include "linkr_bridge.h"

LOG_MODULE_REGISTER(e907_openamp, LOG_LEVEL_INF);

#define SHM_NODE	DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR	DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE	DT_REG_SIZE(SHM_NODE)

/* One slot per channel the A7 announces back to us (each /dev/rpmsgN open). */
#define LINKR_MAX_EPTS	4

static const struct device *const ipm_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_ipc));

static struct metal_io_region shm_io;
static struct metal_io_region rsc_io;
static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_physmap;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_endpoint ns_ept;
static struct rpmsg_endpoint linkr_epts[LINKR_MAX_EPTS];
static bool linkr_ept_used[LINKR_MAX_EPTS];

K_MSGQ_DEFINE(ipm_msgq, sizeof(uint32_t), 8, 4);
/* Set when the queue is full: the RX thread then rescans both vrings, so a
 * dropped notification costs a little work instead of a stalled virtqueue. */
static atomic_t ipm_overflow;

/* --- rpmsg endpoint: Linkr Bee UART bridge --- */

static int bridge_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		     uint32_t src, void *priv)
{
	ARG_UNUSED(priv);

	return linkr_bridge_rpmsg_recv(ept, data, len, src);
}

static void linkr_ept_unbind(struct rpmsg_endpoint *ept)
{
	LOG_INF("rpmsg NS unbind addr 0x%x", ept->addr);

	linkr_bridge_rpmsg_unbind(ept);
	rpmsg_destroy_ept(ept);
	linkr_ept_used[ept - linkr_epts] = false;
}

static struct rpmsg_endpoint *linkr_ept_alloc(void)
{
	for (int i = 0; i < LINKR_MAX_EPTS; i++) {
		if (!linkr_ept_used[i]) {
			return &linkr_epts[i];
		}
	}

	return NULL;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name,
		       uint32_t dest)
{
	struct rpmsg_endpoint *ept = linkr_ept_alloc();
	int ret;

	LOG_INF("rpmsg NS bind: '%s' dest=%u", name, dest);

	if (ept == NULL) {
		LOG_ERR("no free endpoint slot for '%s'", name);
		return;
	}

	ret = rpmsg_create_ept(ept, rdev, name, RPMSG_ADDR_ANY, dest,
			       bridge_cb, linkr_ept_unbind);
	if (ret != 0) {
		LOG_ERR("rpmsg_create_ept('%s') failed: %d", name, ret);
		return;
	}

	linkr_ept_used[ept - linkr_epts] = true;
}

/* --- IPM: A7 kicks a virtqueue --- */

static int mailbox_notify(void *priv, uint32_t id)
{
	/* The A7 mbox callback reads the payload as the vring notifyid.  Keep it
	 * in static storage: the sunxi mailbox copies it into the data register
	 * synchronously, but a driver that defers the transfer must not get a
	 * pointer into this stack frame. */
	static uint32_t notify_id;

	ARG_UNUSED(priv);
	notify_id = id;
	return ipm_send(ipm_dev, 0, id, &notify_id, sizeof(notify_id));
}

static void ipm_rx_cb(const struct device *dev, void *ctx, uint32_t id,
		      volatile void *data)
{
	uint32_t notifyid = VRING1_ID;

	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);
	ARG_UNUSED(id);

	if (data != NULL) {
		notifyid = *(volatile uint32_t *)data;
	}

	/* ISR context: defer rpmsg processing to the RX thread.  A full queue
	 * must not lose the kick: mark it and let the thread rescan. */
	if (k_msgq_put(&ipm_msgq, &notifyid, K_NO_WAIT) != 0) {
		atomic_set(&ipm_overflow, 1);
	}
}

static void ipm_rx_thread(void *a, void *b, void *c)
{
	uint32_t notifyid;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		if (k_msgq_get(&ipm_msgq, &notifyid, K_FOREVER) != 0) {
			continue;
		}

		do {
			(void)rproc_virtio_notified(rvdev.vdev, notifyid);
		} while (k_msgq_get(&ipm_msgq, &notifyid, K_NO_WAIT) == 0);

		if (atomic_cas(&ipm_overflow, 1, 0)) {
			/* At least one kick was dropped while the queue was
			 * full; draining both vrings is cheap and restores
			 * progress. */
			(void)rproc_virtio_notified(rvdev.vdev, VRING0_ID);
			(void)rproc_virtio_notified(rvdev.vdev, VRING1_ID);
		}
	}
}

K_THREAD_STACK_DEFINE(ipm_rx_stack, 2048);
static struct k_thread ipm_rx_thread_data;

/* --- bring-up --- */

static int openamp_slave_init(void)
{
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	void *rsc_table;
	int rsc_size;
	int ret;

	ret = metal_init(&metal_params);
	if (ret != 0) {
		LOG_ERR("metal_init failed: %d", ret);
		return ret;
	}

	/* Shared memory window (DA == PA) covering buffers and both vrings. */
	metal_io_init(&shm_io, (void *)SHM_START_ADDR, &shm_physmap,
		      SHM_SIZE, -1, 0, NULL);

	/* The resource table is in this image and is updated by Linux. */
	rsc_table_get(&rsc_table, &rsc_size);
	rsc_physmap = (metal_phys_addr_t)(uintptr_t)rsc_table;
	metal_io_init(&rsc_io, rsc_table, &rsc_physmap, rsc_size, -1, 0, NULL);

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table), &rsc_io,
					NULL, mailbox_notify, NULL);
	if (vdev == NULL) {
		LOG_ERR("failed to create vdev");
		return -ENODEV;
	}

	/* Wait until Linux has set DRIVER_OK in the resource table. */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, &shm_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret != 0) {
		LOG_ERR("failed to init vring 0: %d", ret);
		goto failed;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, &shm_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret != 0) {
		LOG_ERR("failed to init vring 1: %d", ret);
		goto failed;
	}

	ret = rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, &shm_io, NULL);
	if (ret != 0) {
		LOG_ERR("rpmsg_init_vdev failed: %d", ret);
		goto failed;
	}

	/* Only enable the MSGBOX/IPM RX interrupt once the vrings exist. */
	ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);
	ipm_set_enabled(ipm_dev, 1);

	/* Announce the channel once; the A7 answers with an NS bind. */
	ret = rpmsg_create_ept(&ns_ept, &rvdev.rdev, "rpmsg_chrdev",
			       RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, bridge_cb, NULL);
	if (ret != 0) {
		LOG_ERR("rpmsg_create_ept(rpmsg_chrdev) failed: %d", ret);
		goto failed;
	}

	LOG_INF("OpenAMP rpmsg_chrdev ready (addr 0x%x)", ns_ept.addr);
	return 0;

failed:
	rproc_virtio_remove_vdev(vdev);
	return ret;
}

static K_THREAD_STACK_DEFINE(openamp_stack, 2048);
static struct k_thread openamp_thread;

static void openamp_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (openamp_slave_init() != 0) {
		return;
	}

	k_thread_create(&ipm_rx_thread_data, ipm_rx_stack,
			K_THREAD_STACK_SIZEOF(ipm_rx_stack),
			ipm_rx_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&ipm_rx_thread_data, "openamp_rx");
}

void e907_openamp_start(void)
{
	k_thread_create(&openamp_thread, openamp_stack,
			K_THREAD_STACK_SIZEOF(openamp_stack),
			openamp_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	k_thread_name_set(&openamp_thread, "openamp");
}
