/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See kusb.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "kusb.h"
#include "../common/zusbc.h"

// rtl/usb/usb_host.v's IRQEN (sw/apps/usb/usbh_hw.h, Z_USBH_IRQEN).
#define K_USBH_IRQEN	0xc000010cu

z_obj_t *k_usb_retired(z_obj_t *args) {
	(void)args;
	return (&z_fail);
}

void k_usb_irq_off(void) {
	*(volatile uint32_t *)K_USBH_IRQEN = 0;
}

bool k_usb_cmd(const char *line) {
	int32_t r = zusb_cmd(line);
	if (r == -Z_ENODEV) {
		printf("usb: no USB driver running\n");
		return false;
	}
	if (r == -Z_ENOENT) printf("usb: unknown command (lsusb, usbrescan; "
		"the rest need `make USBH_DEBUG=1` in sw/apps/usb)\n");
	else if (r < 0) printf("usb: no answer (%ld)\n", (long)r);
	return r == 0;
}
