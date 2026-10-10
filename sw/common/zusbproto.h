#ifndef ZUSBPROTO_H
#define ZUSBPROTO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The USB driver's protocol: what a client sends to `usb`
 * (sw/apps/usb) and what comes back. Since Phase 3 the USB host stack
 * -- host, hub, HID setup, mass storage, ethernet, serial -- is that
 * process, not the kernel. docs/userspace_drivers.md, "Phase 3";
 * docs/usb_driver.md.
 *
 * The clients: `fs` for mass storage (its /usb volume), `net` for the
 * ethernet adapter, `serial` for a USB serial device, and the kernel
 * shell for `lsusb` and the debug commands. All through one small
 * client, sw/common/zusbc.c.
 *
 * -- A request --
 *
 *   subject  Z_USB_REQ
 *   tag      the client's sequence number; the reply carries the same
 *   obj      a Z_LIST of two Z_BLOBs, both in the client's memory:
 *              [0] the z_usbreq_t below
 *              [1] data: sectors or bytes to write, a command line,
 *                  the client's ring (NET_OPEN); length 0 when none
 *
 * -- A reply --
 *
 *   subject  Z_USB_REPLY
 *   tag      the request's
 *   obj      a Z_BLOB in `usb`'s memory: a z_usbrep_t, then any data.
 *            Borrowed: the client copies what it needs before it sends
 *            anything else (docs/messaging.md). NET_OPEN's reply is the
 *            exception -- see zusbnet.h.
 *
 * A result >= 0 is success; < 0 is -Z_E*.
 *
 * -- Events --
 *
 *   subject  Z_USB_EVENT,  obj a Z_UINT32 of Z_USBEV_* bits
 *
 * Sent by `usb`, unasked, to the client of a kind of device when one
 * comes or goes, and to `net` and `serial` when data is waiting that
 * was not before. Scalars, so nothing is borrowed. They only say
 * "look": the client asks. A client that reads its mail with
 * z_msg_read() should ignore what it does not know, and these are such.
 * The client of a kind is whoever last asked about it: MSC_INFO,
 * NET_OPEN, CDC_INFO or CDC_READ.
 *
 *   subject  Z_USB_KICK, obj none
 *
 * Sent by `net` to `usb` when it puts a frame into an empty send ring
 * (zusbnet.h). No reply.
 */

#include <stdint.h>
#include "zsubjects.h"
#include "zfsproto.h"		// the -Z_E* results, which are the system's

#define Z_USB_REQ		(Z_SUBJ_USB + 0)
#define Z_USB_REPLY		(Z_SUBJ_USB + 1)
#define Z_USB_EVENT		(Z_SUBJ_USB + 2)
#define Z_USB_KICK		(Z_SUBJ_USB + 3)
Z_SUBJECTS_IN(Z_SUBJ_USB, Z_USB_KICK);

// The name `usb` registers under, as z_pid_register() completes it.
#define Z_USB_SERVICE	"usb0"

// Z_USB_EVENT bits.
#define Z_USBEV_MSC		1	// a drive came or went
#define Z_USBEV_NET		2	// the adapter came or went, or frames wait
#define Z_USBEV_CDC		4	// the serial device came or went, or bytes wait

// Operations (z_usbreq_t.op).
#define Z_USBOP_MSC_INFO	1	//                         -> 0, a z_usb_msc_info_t
#define Z_USBOP_MSC_START	2	// bring a new drive up    -> 0
#define Z_USBOP_MSC_READ	3	// lba, count              -> 0, count sectors
#define Z_USBOP_MSC_WRITE	4	// lba, count, data        -> 0
#define Z_USBOP_NET_INFO	5	//                         -> 0, a z_usbnet_info_t
#define Z_USBOP_NET_OPEN	6	// mac, data: the ring     -> zusbnet.h
#define Z_USBOP_NET_CLOSE	7	//                         -> 0
#define Z_USBOP_CDC_INFO	8	//                         -> present | kind << 1
#define Z_USBOP_CDC_READ	9	// len                     -> bytes (0 none), the bytes
#define Z_USBOP_CDC_WRITE	10	// data                    -> bytes written
#define Z_USBOP_CMD			11	// data: a command line    -> 0; prints on the console

// Sectors per MSC_READ / MSC_WRITE. A sector is one SCSI command on
// the wire anyway (usbh_msc.c), so a bigger request would save only
// round trips; this bounds `usb`'s reply buffer.
#define Z_USB_MSC_MAX		4
#define Z_USB_SECTOR		512

// Bytes per CDC_READ / CDC_WRITE.
#define Z_USB_CDC_MAX		256

typedef struct {
	uint32_t	op;		// Z_USBOP_*
	uint32_t	lba;
	uint32_t	count;		// sectors
	uint32_t	len;		// CDC_READ: the most wanted
	uint8_t		mac[6];		// NET_OPEN: the address net sends from
	uint8_t		pad[2];
} z_usbreq_t;

typedef struct {
	int32_t		result;
	uint32_t	len;		// bytes of data after this
} z_usbrep_t;

typedef struct {
	uint8_t		present;	// a drive is bound
	uint8_t		ready;		// MSC_START succeeded: sectors known
	uint8_t		pad[2];
	uint32_t	sectors;
	uint32_t	gen;		// bumped by every bind and unbind
} z_usb_msc_info_t;

#endif
