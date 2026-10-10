#ifndef ZUSBCDC_H
#define ZUSBCDC_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A USB serial device, for apps. The driver is in the `usb` process
 * (sw/apps/usb/usbh_cdc.c; until Phase 3 it was the kernel's, behind
 * the Z_SYS_USBCDC_* syscalls). These are requests to it, through
 * sw/common/zusbc.c: link zusbc.o. docs/usb_driver.md.
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
 * Any USB serial device `usb` binds: CDC-ACM, and CP210x bridges
 * (docs/usb_host.md, "USB serial devices"). FTDI, CH340 and PL2303 are
 * not bound yet. The calls are the same for every kind; the name is
 * historical. There is no baud rate to set: the host sets 115200 8N1
 * at bind, which a native-USB device ignores and a bridge uses.
 *
 * -- Reading --
 *
 * `usb` reads the device for its client -- whoever called these last
 * -- and keeps what arrives, so a read is a request answered from
 * memory, never a wait on the wire. When bytes arrive with none kept,
 * it sends the client a Z_USB_EVENT (zusbproto.h), which a loop
 * sleeping in z_proc_wait() wakes for and should otherwise ignore.
 */

#include <stdint.h>
#include <stdbool.h>

// A device is bound.
bool z_usbcdc_present(void);

// Up to `max` of the bytes the device has sent (Z_USB_CDC_MAX at a
// time). 0: none yet. -1: no device, or no `usb`.
int32_t z_usbcdc_read(uint8_t *buf, uint32_t max);

// Returns once the device has taken len bytes: len, or fewer (-1 if
// none) on a failure.
int32_t z_usbcdc_write(const uint8_t *buf, uint32_t len);

#endif
