/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linkr Bee UART <-> rpmsg bridge (E907 side).
 *
 * UART3 (PE12/PE13, 115200 8N1) is the serial port of the target board that
 * the browser terminal talks to.  Bytes received on UART3 are packed into
 * rpmsg messages towards the A7; bytes arriving over rpmsg are written to
 * UART3.  The A7 side runs the HTTP/WebSocket front-end and implements the
 * Linkr Bee session protocol.
 *
 * Protocol on the rpmsg endpoint:
 *   A7 -> E907  "LINKR1\n"   handshake, tells this side the A7 endpoint addr
 *   E907 -> A7  "LINKR1 OK\n" handshake reply
 *   afterwards  raw serial bytes in both directions
 *
 * Before the handshake the endpoint keeps the old echo behaviour so the
 * standalone rpmsg_test / rpmsg_bench tools still work.
 */

#include "linkr_bridge.h"

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <openamp/open_amp.h>

#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(linkr_bridge, LOG_LEVEL_INF);

#define UART_NODE        DT_NODELABEL(uart3)
#define RX_RING_SIZE     4096
#define TX_RING_SIZE     4096
#define RX_CHUNK         256
#define RX_POLL_MS       20

RING_BUF_DECLARE(uart_rx_ring, RX_RING_SIZE);
RING_BUF_DECLARE(uart_tx_ring, TX_RING_SIZE);

K_SEM_DEFINE(uart_rx_sem, 0, 1);
K_SEM_DEFINE(uart_tx_sem, 0, 1);
/* Signalled by the TX thread whenever it frees space in uart_tx_ring. */
K_SEM_DEFINE(uart_tx_space_sem, 0, 1);

/* Protects the RX ring: it is fed by the UART ISR and by the polling
 * fallback thread (used while the CLIC routing of UART3 is unproven). */
static struct k_spinlock rx_lock;

static const struct device *const uart_dev = DEVICE_DT_GET(UART_NODE);

static struct rpmsg_endpoint *bridge_ept;
static uint32_t bridge_dst;
static atomic_t bridge_ready;
static atomic_t uart_rx_total;
static atomic_t uart_rx_dropped;

static void rx_push(const uint8_t *buf, uint32_t len)
{
	k_spinlock_key_t key;

	uint32_t put;

	if (!len) {
		return;
	}
	key = k_spin_lock(&rx_lock);
	put = ring_buf_put(&uart_rx_ring, buf, len);
	k_spin_unlock(&rx_lock, key);
	atomic_add(&uart_rx_total, put);
	if (put < len) {
		atomic_add(&uart_rx_dropped, len - put);
	}
}

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[64];

	ARG_UNUSED(user_data);

	uart_irq_update(dev);
	while (uart_irq_is_pending(dev)) {
		int n;

		if (!uart_irq_rx_ready(dev)) {
			break;
		}
		n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		rx_push(buf, (uint32_t)n);
	}
	k_sem_give(&uart_rx_sem);
}

/* Drain whatever is in the RX FIFO without waiting for an interrupt.
 *
 * The ns16550 driver does not serialise uart_fifo_read() between a thread and
 * its own ISR, so mask interrupts while the polling thread reads the FIFO.
 * (The ISR cannot be interrupted by the thread, so its own path needs no
 * lock.) */
static void uart_poll_rx(void)
{
	uint8_t buf[64];
	unsigned int key;
	int n;

	key = irq_lock();
	do {
		n = uart_fifo_read(uart_dev, buf, sizeof(buf));
		if (n > 0) {
			rx_push(buf, (uint32_t)n);
		}
	} while (n > 0);
	irq_unlock(key);
}

static void uart_rx_thread(void *a, void *b, void *c)
{
	uint8_t buf[RX_CHUNK];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint32_t got;

		/* Wait for the ISR, but also poll so a missing CLIC IRQ only
		 * costs latency instead of losing the bridge entirely. */
		(void)k_sem_take(&uart_rx_sem, K_MSEC(RX_POLL_MS));
		uart_poll_rx();

		while ((got = ring_buf_get(&uart_rx_ring, buf, sizeof(buf))) > 0) {
			if (!atomic_get(&bridge_ready) || !bridge_ept ||
			    bridge_dst == 0) {
				continue;
			}
			if (rpmsg_sendto(bridge_ept, buf, got, bridge_dst) < 0) {
				LOG_WRN("rpmsg tx failed, dropped %u bytes",
					(unsigned int)got);
			}
		}
	}
}

static void uart_tx_thread(void *a, void *b, void *c)
{
	uint8_t buf[RX_CHUNK];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		uint32_t got;

		k_sem_take(&uart_tx_sem, K_FOREVER);
		while ((got = ring_buf_get(&uart_tx_ring, buf, sizeof(buf))) > 0) {
			for (uint32_t i = 0; i < got; i++) {
				uart_poll_out(uart_dev, buf[i]);
			}
			/* Wake a producer that is waiting for ring space. */
			k_sem_give(&uart_tx_space_sem);
		}
	}
}

K_THREAD_STACK_DEFINE(uart_rx_stack, 1536);
K_THREAD_STACK_DEFINE(uart_tx_stack, 1536);
static struct k_thread uart_rx_thread_data;
static struct k_thread uart_tx_thread_data;

static bool handshake_match(const uint8_t *data, size_t len)
{
	return (len == 7 && !memcmp(data, "LINKR1\n", 7)) ||
	       (len == 6 && !memcmp(data, "LINKR1", 6));
}

/* ---- UART configuration control lines ----------------------------------
 * The A7 may ask this side to reprogram UART3 at runtime:
 *
 *   A7 -> E907  "LINKRCFG <baud>,<data>,<parity>,<stop>,<flow>\n"
 *               data: 5..8, parity: n|e|o, stop: 1|2, flow: n|rtscts
 *   E907 -> A7  "LINKRCFG OK uart=<baud>,<data>,<parity>,<stop>,<flow>\n"
 *              "LINKRCFG ERR=<errno>\n"
 *
 * The control line is consumed here and never written to UART3, so the
 * target board does not see it.  The reply travels back over the same
 * rpmsg endpoint; the A7 daemon strips it from the serial stream.
 */
#define LINKRCFG_PREFIX "LINKRCFG "
#define LINKRCFG_PREFIX_LEN (sizeof(LINKRCFG_PREFIX) - 1)

static const char *uart_data_name(enum uart_config_data_bits bits)
{
	switch (bits) {
	case UART_CFG_DATA_BITS_5:
		return "5";
	case UART_CFG_DATA_BITS_6:
		return "6";
	case UART_CFG_DATA_BITS_7:
		return "7";
	case UART_CFG_DATA_BITS_8:
	default:
		return "8";
	}
}

static const char *uart_parity_name(enum uart_config_parity parity)
{
	switch (parity) {
	case UART_CFG_PARITY_ODD:
		return "o";
	case UART_CFG_PARITY_EVEN:
		return "e";
	case UART_CFG_PARITY_NONE:
	default:
		return "n";
	}
}

static const char *uart_stop_name(enum uart_config_stop_bits stop)
{
	return stop == UART_CFG_STOP_BITS_2 ? "2" : "1";
}

static const char *uart_flow_name(enum uart_config_flow_control flow)
{
	return flow == UART_CFG_FLOW_CTRL_RTS_CTS ? "rtscts" : "n";
}

/* Last configuration successfully programmed on UART3.  The DT default is
 * 115200 8N1 without flow control; `LINKRCFG ?` reports this. */
static struct uart_config active_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

static int cfg_reply(char *reply, size_t reply_len, const char *prefix)
{
	return snprintk(reply, reply_len, "%s uart=%u,%s,%s,%s,%s\n",
			prefix, active_cfg.baudrate,
			uart_data_name(active_cfg.data_bits),
			uart_parity_name(active_cfg.parity),
			uart_stop_name(active_cfg.stop_bits),
			uart_flow_name(active_cfg.flow_ctrl));
}

/* Split a comma separated token list in place.  Returns token count. */
static int split_commas(char *text, char **out, int max)
{
	int n = 0;

	while (n < max) {
		out[n++] = text;
		while (*text && *text != ',') {
			text++;
		}
		if (!*text) {
			break;
		}
		*text++ = '\0';
	}
	return n;
}

static int linkr_apply_cfg(char *args, char *reply, size_t reply_len)
{
	char *tok[5];
	struct uart_config cfg;
	unsigned long baud;
	int ntok;
	int ret;

	if (!strcmp(args, "?")) {
		return cfg_reply(reply, reply_len, "LINKRCFG OK");
	}

	ntok = split_commas(args, tok, 5);

	if (ntok != 5) {
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	baud = strtoul(tok[0], NULL, 10);
	if (baud < 300 || baud > 4000000) {
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	cfg.baudrate = (uint32_t)baud;

	switch (tok[1][0]) {
	case '5':
		cfg.data_bits = UART_CFG_DATA_BITS_5;
		break;
	case '6':
		cfg.data_bits = UART_CFG_DATA_BITS_6;
		break;
	case '7':
		cfg.data_bits = UART_CFG_DATA_BITS_7;
		break;
	case '8':
		cfg.data_bits = UART_CFG_DATA_BITS_8;
		break;
	default:
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	switch (tok[2][0]) {
	case 'n':
	case 'N':
		cfg.parity = UART_CFG_PARITY_NONE;
		break;
	case 'e':
	case 'E':
		cfg.parity = UART_CFG_PARITY_EVEN;
		break;
	case 'o':
	case 'O':
		cfg.parity = UART_CFG_PARITY_ODD;
		break;
	default:
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	switch (tok[3][0]) {
	case '1':
		cfg.stop_bits = UART_CFG_STOP_BITS_1;
		break;
	case '2':
		cfg.stop_bits = UART_CFG_STOP_BITS_2;
		break;
	default:
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	if (!strcmp(tok[4], "n") || !strcmp(tok[4], "none") ||
	    !strcmp(tok[4], "0")) {
		cfg.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
	} else if (!strcmp(tok[4], "rtscts") || !strcmp(tok[4], "r") ||
		   !strcmp(tok[4], "1")) {
		cfg.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;
	} else {
		return snprintk(reply, reply_len, "LINKRCFG ERR=-22\n");
	}

	LOG_INF("reconfiguring UART3: %lu %s%s%s flow=%s", baud, tok[1],
		tok[2], tok[3], tok[4]);
	ret = uart_configure(uart_dev, &cfg);
	if (ret < 0) {
		LOG_WRN("uart_configure failed: %d", ret);
		return snprintk(reply, reply_len, "LINKRCFG ERR=%d\n", ret);
	}

	/* uart_configure() programs the port directly and leaves the
	 * interrupt enable register cleared; the polling thread would still
	 * drain RX, but re-enable the IRQ path for low latency. */
	uart_irq_rx_disable(uart_dev);
	uart_irq_callback_set(uart_dev, uart_isr);
	uart_irq_rx_enable(uart_dev);

	active_cfg = cfg;
	return cfg_reply(reply, reply_len, "LINKRCFG OK");
}

int linkr_bridge_rpmsg_recv(struct rpmsg_endpoint *ept, const void *data,
			    size_t len, uint32_t src)
{
	const uint8_t *p = data;
	uint32_t put;

	if (!ept || !data) {
		return -EINVAL;
	}

	bridge_ept = ept;
	bridge_dst = src;

	if (handshake_match(p, len)) {
		char reply[128];
		int n;

		atomic_set(&bridge_ready, 1);
		n = snprintk(reply, sizeof(reply), "LINKR1 OK rx=%u\n",
			     (unsigned int)atomic_get(&uart_rx_total));
		if (atomic_get(&uart_rx_dropped)) {
			LOG_WRN("%u UART RX bytes dropped (ring full)",
				(unsigned int)atomic_get(&uart_rx_dropped));
		}
		LOG_INF("A7 handshake from addr 0x%x, uart rx=%u", src,
			(unsigned int)atomic_get(&uart_rx_total));
		if (n < 0) {
			n = sizeof("LINKR1 OK\n") - 1;
			memcpy(reply, "LINKR1 OK\n", n);
		}
		return rpmsg_sendto(ept, reply, n, src);
	}

	if (atomic_get(&bridge_ready) && len > LINKRCFG_PREFIX_LEN &&
	    !memcmp(p, LINKRCFG_PREFIX, LINKRCFG_PREFIX_LEN)) {
		char line[96];
		char reply[96];
		size_t copy = len - LINKRCFG_PREFIX_LEN;
		int n;

		if (copy > sizeof(line) - 1) {
			copy = sizeof(line) - 1;
		}
		memcpy(line, p + LINKRCFG_PREFIX_LEN, copy);
		line[copy] = '\0';
		/* Trim the trailing newline the A7 adds. */
		while (copy > 0 && (line[copy - 1] == '\n' ||
				    line[copy - 1] == '\r')) {
			line[--copy] = '\0';
		}
		n = linkr_apply_cfg(line, reply, sizeof(reply));
		if (n < 0) {
			n = sizeof("LINKRCFG ERR=-5\n") - 1;
			memcpy(reply, "LINKRCFG ERR=-5\n", n);
		}
		return rpmsg_sendto(ept, reply, n, src);
	}

	if (!atomic_get(&bridge_ready)) {
		/* Legacy echo mode: keeps rpmsg_test / rpmsg_bench usable. */
		return rpmsg_sendto(ept, data, len, src);
	}

	put = ring_buf_put(&uart_tx_ring, p, len);
	if (put < len) {
		/* The line is 115200 baud: 100 KB takes ~9 s to leave the
		 * wire. Rather than silently dropping a paste, apply
		 * backpressure: block this endpoint's RX callback until the
		 * TX thread has drained some of the ring. That in turn stops
		 * the A7 from writing more rpmsg messages, and the browser
		 * sees the write slow down instead of losing data. Bound the
		 * wait so a wedged UART cannot hang the rpmsg endpoint.
		 */
		uint32_t left = len - put;

		p += put;
		while (left > 0) {
			if (k_sem_take(&uart_tx_space_sem, K_MSEC(100)) != 0) {
				LOG_WRN("UART TX stalled, dropped %u bytes",
					(unsigned int)left);
				break;
			}
			put = ring_buf_put(&uart_tx_ring, p, left);
			p += put;
			left -= put;
		}
	}
	k_sem_give(&uart_tx_sem);

	return 0;
}

void linkr_bridge_rpmsg_unbind(struct rpmsg_endpoint *ept)
{
	if (bridge_ept == ept) {
		bridge_ept = NULL;
		bridge_dst = 0;
		atomic_set(&bridge_ready, 0);
	}
}

int linkr_bridge_init(void)
{
	uint8_t drain[64];

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART3 not ready");
		return -ENODEV;
	}

	uart_irq_rx_disable(uart_dev);
	/* Flush anything the bootloader/console left behind. */
	while (uart_fifo_read(uart_dev, drain, sizeof(drain)) > 0) {
	}

	k_thread_create(&uart_rx_thread_data, uart_rx_stack,
			K_THREAD_STACK_SIZEOF(uart_rx_stack),
			uart_rx_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&uart_rx_thread_data, "uart_rx");

	k_thread_create(&uart_tx_thread_data, uart_tx_stack,
			K_THREAD_STACK_SIZEOF(uart_tx_stack),
			uart_tx_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&uart_tx_thread_data, "uart_tx");

	uart_irq_callback_set(uart_dev, uart_isr);
	uart_irq_rx_enable(uart_dev);

	LOG_INF("Linkr Bee bridge ready on %s", uart_dev->name);
	return 0;
}
