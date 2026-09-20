/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: CDC-ACM ("USB serial" in the standard class). See
 * docs/usb_host.md, "CDC".
 *
 * One device at a time. The data path runs in process context and holds
 * the transaction engine for each transaction (z_usbh_bus_reserve() in
 * usbh.h), like mass storage.
 *
 * Covers Arduinos, the Pico, most dev boards and modems -- anything that
 * works on Linux without a vendor driver. FTDI, CP210x, CH340 and PL2303
 * adapters are vendor protocols, not CDC, and are not bound here.
 */

#ifndef Z_USBH_CDC_H
#define Z_USBH_CDC_H

#include <stdint.h>

// -- called by usbh.c --

// Does this configuration hold a CDC-ACM function: a communications
// interface (class 2, subclass 2) and a data interface (class 0x0a) with
// bulk IN and OUT? Returns the communications interface number, the
// target of the class requests, or -1.
int z_usbh_cdc_probe(const uint8_t *cfg, int n);

// Claim it. 1 if bound; 0 if another CDC device already is.
int z_usbh_cdc_bind(uint8_t addr, uint8_t xa_flags, uint8_t port,
                    const uint8_t *cfg, int n);

// The device went away (ISR context). Touches no bus.
void z_usbh_cdc_unbind(void);

// -- for the kernel --

int z_usbh_cdc_present(void);

// Send len bytes. Blocks until they are accepted or the device fails.
// Returns the count sent, or -1.
int z_usbh_cdc_write(const uint8_t *buf, int len);

// One IN transaction: whatever the device has, up to max bytes (one
// packet), or 0 if it has nothing (NAK). Never waits for data. -1 on a
// failure or no device.
int z_usbh_cdc_read(uint8_t *buf, int max);

#endif
