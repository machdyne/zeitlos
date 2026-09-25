/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: USB serial devices -- CDC-ACM and the vendor bridges (see
 * usbh_ser.h for which) -- on one data path. See docs/usb_host.md,
 * "CDC" and "USB serial devices".
 *
 * One device at a time. The data path runs in process context and holds
 * the transaction engine for each transaction (z_usbh_bus_reserve() in
 * usbh.h), like mass storage.
 *
 * The name is historical: this began as the CDC-ACM driver, and the
 * syscalls above it are still Z_SYS_USBCDC_*. What it binds now is
 * "a USB serial device" of any kind usbh_ser.h knows.
 */

#ifndef Z_USBH_CDC_H
#define Z_USBH_CDC_H

#include <stdint.h>

#include "usbh_ser.h"

// -- called by usbh.c --
//
// Recognition and setup are in usbh_ser.h (z_usbh_ser_probe(),
// z_usbh_ser_setup()); usbh.c runs the setup and then binds here.

// Claim it. 1 if bound; 0 if another USB serial device already is.
int z_usbh_cdc_bind(uint8_t addr, uint8_t xa_flags, uint8_t port,
                    const z_usbh_ser_t *s);

// The device went away (ISR context). Touches no bus.
void z_usbh_cdc_unbind(void);

// -- for the kernel --

int z_usbh_cdc_present(void);

// Which kind is bound (Z_USBH_SER_*), Z_USBH_SER_NONE for none.
int z_usbh_cdc_kind(void);

// Send len bytes. Blocks until they are accepted or the device fails.
// Returns the count sent, or -1.
int z_usbh_cdc_write(const uint8_t *buf, int len);

// One IN transaction: whatever the device has, up to max bytes (one
// packet), or 0 if it has nothing (NAK). Never waits for data. -1 on a
// failure or no device.
int z_usbh_cdc_read(uint8_t *buf, int max);

#endif
