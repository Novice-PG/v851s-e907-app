/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Early E907 board init: bring the UART3 console pins + clock up BEFORE the
 * ns16550 console driver initialises.  The V851S Linux side keeps &uart3
 * disabled (UART3 belongs to the E907), and CONFIG_DEVMEM is off on the A7,
 * so the E907 core must set the pin-mux and clock gate itself.
 *
 * Register values are taken from the vendor Linux 4.9 kernel drivers:
 *   - PIO base 0x02000000, bank spacing 0x30, CFG regs at +0x00
 *     (drivers/pinctrl/sunxi/pinctrl-sun8iw21p1.c, BANK_MEM_SIZE=0x30)
 *   - PE12 = uart3_tx, PE13 = uart3_rx, mux 7 (pinctrl-sun8iw21p1.c)
 *     PE bank = pin 128.. -> bank 4 -> 4*0x30 = 0xC0; pins 8..15 in CFG1
 *     at 0xC4; pin12 -> bits16..19, pin13 -> bits20..23 (byte at 0xC6).
 *   - UART3 clock gate: CCU (sunxi_clk_base = 0x02001000) + UART_GATE(0x090C)
 *     = 0x0200190C; enable bit 19, reset bit 3 (active-low: 1 = deassert)
 *     (drivers/clk/sunxi/clk-sun8iw21.c, clk-periph.c).
 *
 * NOTE: this is board-specific bring-up glue, not a general driver.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>

#define V851S_PIO_CFG_PE12_PE13   0x020000C4u  /* PE_CFG1 word (pins 8..15) */
#define V851S_CCU_UART_GATE       0x0200190Cu  /* UART_GATE: uart0..3 gates  */

#define PE12_PE13_MUX7            0x00770000u  /* bits16..19=7, bits20..23=7 */
#define UART3_GATE_EN             (1u << 19)   /* module clock enable        */
#define UART3_RESET_DEASSERT      (1u << 3)    /* reset is active-low        */

static int v851s_e907_uart3_early_init(void)
{
	uint32_t v;

	/* 1. Pin-mux PE12/PE13 -> UART3 (mux 7).  Only bits 16..23 are
	 *    touched, so pins 8..11 of the same CFG1 word are preserved.
	 */
	v = sys_read32(V851S_PIO_CFG_PE12_PE13);
	v = (v & ~PE12_PE13_MUX7) | PE12_PE13_MUX7;
	sys_write32(v, V851S_PIO_CFG_PE12_PE13);

	/* 2. Enable the UART3 clock gate and deassert its reset. */
	v = sys_read32(V851S_CCU_UART_GATE);
	v |= (UART3_GATE_EN | UART3_RESET_DEASSERT);
	sys_write32(v, V851S_CCU_UART_GATE);

	return 0;
}

/* Priority 0 runs before the ns16550/UART console driver (which inits at
 * CONFIG_SERIAL_INIT_PRIORITY, typically 50+), so the clock is on and the
 * pins muxed before the driver touches the UART3 registers.
 */
SYS_INIT(v851s_e907_uart3_early_init, PRE_KERNEL_1, 0);
