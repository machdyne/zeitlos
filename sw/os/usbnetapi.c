/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Z_SYS_USBNET: the USB ethernet (CDC-ECM) driver, for the net app.
 * See sw/common/zusbnet.h and docs/usb_ethernet.md.
 *
 * One syscall with an op field rather than four, as Z_SYS_FLASH does.
 * It runs with the scheduler held (kernel.c, k_syscall_touches_fs):
 * RECV and SEND hold the one USB transaction engine for a whole frame
 * and use the packet buffer area mass storage uses from inside FatFs.
 */

#include <stdint.h>
#include <stddef.h>

#define Z_KERNEL
#include "kernel.h"
#include "usbnetapi.h"
#include "usb/usbh_ecm.h"

z_obj_t *k_usbnet(z_obj_t *args) {
	z_usbnet_args_t *a = (z_usbnet_args_t *)args;
	if (!a) return &z_fail;
	a->n = -1;
	switch (a->op) {
	case Z_USBNET_INFO:
		if (!a->buf || a->len < sizeof(z_usbnet_info_t)) break;
		z_usbh_ecm_info((z_usbnet_info_t *)a->buf);
		a->n = 0;
		break;
	case Z_USBNET_RECV:
		if (!a->buf) break;
		a->n = z_usbh_ecm_recv(a->buf, (int)a->len);
		break;
	case Z_USBNET_SEND:
		if (!a->buf) break;
		a->n = z_usbh_ecm_send(a->buf, (int)a->len);
		break;
	case Z_USBNET_OPEN:
		if (!a->buf || a->len < 6) break;
		z_usbh_ecm_want(a->buf);
		a->n = 0;
		break;
	default:
		break;
	}
	return a->n >= 0 ? &z_ok : &z_fail;
}
