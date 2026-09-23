/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Syscalls onto the USB CDC-ACM driver. See sw/common/zusbcdc.h.
 *
 * These run with the scheduler held (kernel.c, k_syscall_touches_fs):
 * the CDC driver holds the one USB transaction engine for each
 * transaction, as mass storage does inside FatFs, and a process
 * switched out while holding it would let another process's storage
 * or CDC transfer start on top of it.
 */

#include <stdint.h>
#include <stddef.h>

#define Z_KERNEL
#include "kernel.h"
#include "usbcdcapi.h"
#include "usb/usbh_cdc.h"
#include "../common/zusbcdc.h"

z_obj_t *k_usbcdc_present(z_obj_t *args) {
	(void)args;
	return z_usbh_cdc_present() ? &z_ok : &z_fail;
}

z_obj_t *k_usbcdc_read(z_obj_t *args) {
	z_usbcdc_args_t *a = (z_usbcdc_args_t *)args;
	if (!a || !a->buf) return &z_fail;
	// k_user_ok(): the kernel must not write where the app has no memory (docs/mpu.md)
	if (!k_user_ok(a->buf, a->len)) return &z_fail;
	a->n = z_usbh_cdc_read(a->buf, (int)a->len);
	return a->n >= 0 ? &z_ok : &z_fail;
}

z_obj_t *k_usbcdc_write(z_obj_t *args) {
	z_usbcdc_args_t *a = (z_usbcdc_args_t *)args;
	if (!a || !a->buf) return &z_fail;
	a->n = z_usbh_cdc_write(a->buf, (int)a->len);
	return a->n >= 0 ? &z_ok : &z_fail;
}
