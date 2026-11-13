/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LINKR_BRIDGE_H_
#define LINKR_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#include <openamp/open_amp.h>

/** Start the UART3 threads and enable RX. Call once at boot. */
int linkr_bridge_init(void);

/**
 * Feed one rpmsg message into the bridge.
 *
 * Returns 0 on success, or a negative errno.  Before the "LINKR1" handshake
 * the message is echoed back, which keeps the standalone rpmsg test tools
 * working.
 */
int linkr_bridge_rpmsg_recv(struct rpmsg_endpoint *ept, const void *data,
			    size_t len, uint32_t src);

/**
 * Forget the endpoint that was used for the bridge.
 *
 * Called when the A7 tears its channel down so that we never send to a
 * destroyed endpoint.  The next handshake re-arms the bridge.
 */
void linkr_bridge_rpmsg_unbind(struct rpmsg_endpoint *ept);

#endif /* LINKR_BRIDGE_H_ */
