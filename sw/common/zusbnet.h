#ifndef ZUSBNET_H
#define ZUSBNET_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A USB ethernet adapter (CDC-ECM), for apps. The driver is in the
 * kernel (sw/os/usb/usbh_ecm.c); this is the one syscall onto it,
 * Z_SYS_USBNET, with an op field, and the argument shapes it shares
 * with sw/os/usbnetapi.c. See docs/usb_ethernet.md.
 *
 * -- One owner, by convention --
 *
 * sw/apps/net is the only caller (its usb_ecm.c backend). Nothing
 * arbitrates between two apps sending frames.
 *
 * -- Frames, not packets --
 *
 * RECV returns one whole Ethernet frame (no FCS) or nothing; it never
 * waits for one to arrive. SEND takes one frame and returns once the
 * adapter has accepted it. Both run with the scheduler held, as the
 * USB CDC and FatFs calls do, because they share the one USB
 * transaction engine with mass storage.
 *
 * The kernel includes this for the structs only (ZUSBNET_STRUCTS_ONLY),
 * which keeps it usable from the co-simulation's host build.
 */

#include <stdint.h>

#define Z_USBNET_INFO   0       // buf: z_usbnet_info_t
#define Z_USBNET_RECV   1       // buf/len: frame buffer; n: bytes, 0 none
#define Z_USBNET_SEND   2       // buf/len: one frame; n: 0 ok, -1 failed
#define Z_USBNET_OPEN   3       // buf: the MAC address net will use (6)

// link: what the adapter's notification endpoint last said.
#define Z_USBNET_LINK_UNKNOWN   0
#define Z_USBNET_LINK_UP        1
#define Z_USBNET_LINK_DOWN      2

typedef struct {
	uint8_t present;        // an adapter is bound
	uint8_t mac_ok;         // mac[] was read from the adapter
	uint8_t link;           // Z_USBNET_LINK_*
	uint8_t promisc;        // the adapter was put in promiscuous mode
	uint8_t mac[6];         // the adapter's own address (iMACAddress)
	uint16_t maxseg;        // wMaxSegmentSize, 0 if not reported
	uint32_t gen;           // bumped by every bind and unbind
	uint32_t rx, tx;        // frames moved since boot
	uint32_t rx_err, tx_err;
	uint32_t rx_drop;       // frames too long for the caller's buffer
} z_usbnet_info_t;

typedef struct {
	uint32_t op;            // Z_USBNET_*
	uint8_t *buf;
	uint32_t len;
	int32_t n;              // filled in by the kernel; -1 no adapter
} z_usbnet_args_t;

#if !defined(Z_KERNEL) && !defined(ZUSBNET_STRUCTS_ONLY)

#include <stdbool.h>
#include "zeitlos.h"

static inline int32_t z_usbnet_call(uint32_t op, uint8_t *buf, uint32_t len) {
	z_usbnet_args_t a;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a.op = op; a.buf = buf; a.len = len; a.n = -1;
	k(Z_SYS_USBNET, (uint32_t *)&a, 0);
	return a.n;
}

// Fills *info. Returns false when this kernel has no USB ethernet
// support at all (an older kernel answers an unknown syscall that way).
static inline bool z_usbnet_info(z_usbnet_info_t *info) {
	return z_usbnet_call(Z_USBNET_INFO, (uint8_t *)info,
		sizeof(*info)) == 0;
}

// One frame, or 0 if the adapter has none. -1: no adapter.
static inline int32_t z_usbnet_recv(uint8_t *buf, uint32_t max) {
	return z_usbnet_call(Z_USBNET_RECV, buf, max);
}

// 0 once the adapter has the frame, -1 if it could not be sent.
static inline int32_t z_usbnet_send(const uint8_t *buf, uint32_t len) {
	return z_usbnet_call(Z_USBNET_SEND, (uint8_t *)buf, len);
}

// Tell the kernel which address net sends from. An adapter plugged in
// later whose own address differs is put in promiscuous mode, so it
// still receives frames for this one.
static inline void z_usbnet_open(const uint8_t mac[6]) {
	z_usbnet_call(Z_USBNET_OPEN, (uint8_t *)mac, 6);
}

#endif

#endif
