/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal bring-up firmware for the V851S E907 co-processor.
 *
 * It is loaded by the A7-side Linux remoteproc driver into the reserved DRAM
 * window at 0x43c00000 (DA == PA, 4 MiB).  The console goes to the Zephyr
 * RAM console; UART3 is owned by the Linkr Bee bridge that forwards the
 * target console to the A7 over rpmsg.
 * A valid .resource_table section lets Linux remoteproc load and start it.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#include "linkr_bridge.h"

extern void e907_openamp_start(void);

int main(void)
{
	unsigned int n = 0;

	printk("\n");
	printk("*** YuzukiPI-Lizard V851S E907 co-processor ***\n");
	printk("Zephyr %s | RV32IMAC + xthead @ 600 MHz\n", KERNEL_VERSION_STRING);
	printk("CLINT 0x30000000 | CLIC 0x30800000 | UART3 0x02500c00\n");
	printk("firmware DRAM 0x43c00000 (4 MiB)\n");
	printk("rpmsg buffers 0x43000000, vrings 0x43040000 / 0x43060000\n");
	printk("resource table: 1 vdev + 2 vrings + RAM console trace\n");
	printk("mode: Linkr Bee UART3 bridge over rpmsg\n");

	(void)linkr_bridge_init();
	e907_openamp_start();

	while (1) {
		printk("e907 heartbeat %u cyc %llu\n", n++,
		       (unsigned long long)k_cycle_get_64());
		k_msleep(30000);
	}

	return 0;
}
