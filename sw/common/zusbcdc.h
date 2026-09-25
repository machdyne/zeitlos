#ifndef ZUSBCDC_H
#define ZUSBCDC_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A USB serial device, for apps. The driver is in the
 * kernel (sw/os/usb/usbh_cdc.c); these are the three syscalls onto it,
 * Z_SYS_USBCDC_PRESENT / _READ / _WRITE, and the argument shapes they
 * share with sw/os/usbcdcapi.c.
 *
 * -- One owner, by convention --
 *
 * As with UART1 (zuart.h): sw/apps/serial owns the device and offers it
 * on its port, `serial0`, to a CONNECT carrying Z_CONN_USBSERIAL_ARG
 * (zconnect.h) -- which is how `term`'s `usbserial` reaches it. Nothing
 * arbitrates between two apps calling these directly; their bytes
 * would interleave.
 *
 * -- What it drives --
 *
 * Any USB serial device the kernel binds: CDC-ACM, and CP210x bridges
 * (docs/usb_host.md, "USB serial devices"). FTDI, CH340 and PL2303 are
 * not bound yet. The syscalls are the same for every kind; the name
 * is historical. There is no baud rate to set: the host sets 115200
 * 8N1 at bind, which a native-USB device ignores and a bridge uses.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zeitlos.h"

// Z_SYS_USBCDC_READ / _WRITE. The kernel fills in n: bytes moved, 0 if
// the device had nothing (read) or took nothing yet, -1 on a failure or
// with no device.
typedef struct {
	uint8_t *buf;
	uint32_t len;		// read: at most this many; write: this many
	int32_t n;
} z_usbcdc_args_t;

#ifndef Z_KERNEL		// the kernel includes this for the struct only

static inline bool z_usbcdc_present(void) {
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)k(Z_SYS_USBCDC_PRESENT, 0, 0);
	return rv && rv->val.uint32 == Z_OK;
}

// One USB transaction: up to one packet (64 bytes) of whatever the
// device has. Never waits for data.
static inline int32_t z_usbcdc_read(uint8_t *buf, uint32_t max) {
	z_usbcdc_args_t a;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a.buf = buf; a.len = max; a.n = -1;
	k(Z_SYS_USBCDC_READ, (uint32_t *)&a, 0);
	return a.n;
}

// Blocks until the device has taken len bytes, or fails.
static inline int32_t z_usbcdc_write(const uint8_t *buf, uint32_t len) {
	z_usbcdc_args_t a;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a.buf = (uint8_t *)buf; a.len = len; a.n = -1;
	k(Z_SYS_USBCDC_WRITE, (uint32_t *)&a, 0);
	return a.n;
}

#endif

#endif
