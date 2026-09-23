/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: CDC-ECM, "USB ethernet" in the standard class. See
 * docs/usb_ethernet.md.
 *
 * One adapter at a time. Enumeration (usbh.c) reads the MAC string,
 * selects the data interface's alternate setting with the bulk
 * endpoints and sets the packet filter, then binds here. The data path
 * runs in process context -- the net app, through Z_SYS_USBNET -- and
 * holds the transaction engine for a whole frame, as mass storage does
 * for a whole SCSI command.
 *
 * Covers the RTL8152/8153 family's ECM configuration, Linux and other
 * USB gadgets, and anything else that works under Linux's cdc_ether
 * without a vendor quirk. ASIX, and Realtek's own vendor mode, are
 * vendor protocols and are not bound.
 */

#ifndef Z_USBH_ECM_H
#define Z_USBH_ECM_H

#include <stdint.h>

#define ZUSBNET_STRUCTS_ONLY
#include "../../common/zusbnet.h"

// -- called by usbh.c, from the enumeration state machine --

// Does this configuration hold an ECM function: a communications
// interface (class 2, subclass 6) and a data interface with an
// alternate setting carrying bulk IN and OUT? Returns the
// communications interface number and claims the function for device
// `dev` until it binds or is torn down; -1 if not, or if another
// adapter is already bound or enumerating.
int z_usbh_ecm_probe(int dev, const uint8_t *cfg, int n);

// What enumeration needs next, from the configuration probed above:
// the MAC string index (0 for none), the data interface and the
// alternate setting that has the endpoints.
void z_usbh_ecm_ids(uint8_t *imac, uint8_t *data, uint8_t *alt);

// The MAC string descriptor has landed at `addr` in the packet buffer.
void z_usbh_ecm_mac(uint32_t addr);

// SET_ETHERNET_PACKET_FILTER's wValue for this adapter.
uint16_t z_usbh_ecm_filter(void);

void z_usbh_ecm_bind(int dev, uint8_t addr, uint8_t xa_flags, uint8_t port);

// Device `dev` went away or failed (ISR context). Touches no bus.
void z_usbh_ecm_forget(int dev);

// lsusb's line for a bound adapter.
void z_usbh_ecm_dump(const char *pre);

// -- for the kernel (process context) --

void z_usbh_ecm_info(z_usbnet_info_t *info);

// One frame into buf, returning its length; 0 if the adapter had none
// (never waits for one); -1 with no adapter.
int z_usbh_ecm_recv(uint8_t *buf, int max);

// One frame. 0 once the adapter has it; -1 if it could not be sent.
int z_usbh_ecm_send(const uint8_t *buf, int len);

// The address net sends from; see z_usbnet_open() in zusbnet.h.
void z_usbh_ecm_want(const uint8_t mac[6]);

#endif
