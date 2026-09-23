#ifndef USB_ECM_H
#define USB_ECM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net's backend for a USB ethernet adapter (CDC-ECM). The driver is in
 * the kernel (sw/os/usb/usbh_ecm.c); this is the thin side of
 * Z_SYS_USBNET (sw/common/zusbnet.h). See docs/usb_ethernet.md.
 *
 * Unlike the three MAC backends, the hardware here can arrive and
 * leave while net runs, and it has an address of its own that net must
 * use -- a receive filter set to the adapter's own MAC is what stops
 * it flooding us with a switch's traffic, and it cannot be set to
 * anything else. So:
 *
 *   - usb_ecm_init() WAITS for an adapter rather than failing;
 *   - usb_ecm_mac() reports the adapter's address, which net adopts
 *     (net_phy_t's get_mac), and tells the kernel about it;
 *   - an adapter unplugged mid-session makes send fail and receive
 *     return nothing, and one plugged back in simply resumes. If it is
 *     a DIFFERENT adapter, the kernel puts it in promiscuous mode so
 *     net keeps the address it already has -- and with it its DHCP
 *     lease and every peer's ARP entry.
 *
 * There is no receive interrupt, so this polls: every tick while
 * traffic is moving, backing off to about 22 ms when the link is idle.
 * See usb_ecm_idle_ticks().
 */

#include <stdbool.h>
#include <stdint.h>

// Waits for an adapter, then reports it. mac is net's own address, used
// only if the adapter has none of its own to offer.
bool usb_ecm_init(const uint8_t mac[6]);

// The address net should send from: the adapter's, or the one passed to
// usb_ecm_init() if it had none. false before init.
bool usb_ecm_mac(uint8_t mac[6]);

uint16_t usb_ecm_recv(uint8_t *buf, uint16_t maxlen);
bool usb_ecm_send(const uint8_t *buf, uint16_t len);
void usb_ecm_debug_dump(void);
uint32_t usb_ecm_idle_ticks(void);

// Is there a USB host controller in this bitstream at all? net_phy.c
// asks before choosing this backend.
bool usb_ecm_supported(void);

#endif
