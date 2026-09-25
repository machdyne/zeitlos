/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: USB serial devices -- INTERNAL to sw/os/usb. See
 * docs/usb_host.md, "USB serial devices".
 *
 * "USB serial" is not one protocol. CDC-ACM is the standard class;
 * CP210x, FTDI, CH340 and PL2303 bridges are each a vendor protocol.
 * What they share is the part that matters to everything above the
 * kernel: once set up, bytes go out on one bulk OUT endpoint and come
 * back on one bulk IN endpoint. So there is ONE data path
 * (usbh_cdc.c) and one set of syscalls (Z_SYS_USBCDC_*), and each
 * kind of device contributes only what actually differs:
 *
 *   1. how it is recognised    -- a probe: class codes, or VID:PID and
 *                                 the shape of its interface
 *   2. how it is set up        -- a list of control requests, sent one
 *                                 per enumeration state by usbh.c
 *                                 (E_SER_SETUP) before binding
 *   3. what its IN packets hold -- usually the bytes; FTDI prefixes
 *                                 every packet with two status bytes,
 *                                 which rx_payload() in usbh_cdc.c is
 *                                 where they will be stripped
 *
 * Adding a kind: a Z_USBH_SER_* value, a probe and a setup generator
 * in its own file (usbh_cp210x.c is the model), one line in each of
 * z_usbh_ser_probe() and z_usbh_ser_setup(), and a case in
 * rx_payload() if its packets are not plain bytes.
 */

#ifndef Z_USBH_SER_H
#define Z_USBH_SER_H

#include <stdint.h>

#define Z_USBH_SER_NONE     0
#define Z_USBH_SER_ACM      1   // CDC-ACM, usbh_cdc.c
#define Z_USBH_SER_CP210X   2   // Silicon Labs CP210x, usbh_cp210x.c
// Z_USBH_SER_FTDI 3 is planned (docs/usb_host.md, "USB serial devices").

// The line every kind is set to at bind. 115200 8N1 is what nearly
// everything on the other end of a bridge expects by default -- the
// Meshtastic USB console among them -- and a native CDC device ignores
// it. There is no syscall to change it yet.
#define Z_USBH_SER_BAUD     115200u

// What a probe found: everything the setup requests and the data path
// need, recomputed at bind from the saved configuration descriptor, so
// nothing but the kind has to be held across enumeration states.
typedef struct {
    uint8_t kind;
    uint8_t iface;          // the interface requests are addressed to
    uint8_t ep_in, ep_out;  // endpoint numbers, direction bit stripped
    uint8_t mps_in, mps_out;
} z_usbh_ser_t;

// One control request of a setup sequence: host-to-device, with at
// most 8 bytes of data stage. Everything a USB serial device needs to
// be told fits that.
typedef struct {
    uint8_t type;           // bmRequestType
    uint8_t req;            // bRequest
    uint16_t val;           // wValue
    uint16_t idx;           // wIndex
    uint8_t len;            // data stage length, 0..8
    // 1: a failure of this request fails enumeration (and so retries
    // it). 0: a STALL is noted and setup carries on -- many devices
    // refuse optional requests they do not implement.
    uint8_t fatal;
    uint8_t data[8];
} z_usbh_ser_req_t;

// -- usbh_cdc.c: the dispatch, called by usbh.c --

// Is this a USB serial device we drive? Fills *s and returns its kind,
// or Z_USBH_SER_NONE. Class-based kinds are tried before ID-based ones.
int z_usbh_ser_probe(uint16_t vid, uint16_t pid,
                     const uint8_t *cfg, int n, z_usbh_ser_t *s);

// Setup request number `step` for this device, into *r. 1 if there is
// one, 0 once the sequence is complete.
int z_usbh_ser_setup(const z_usbh_ser_t *s, int step, z_usbh_ser_req_t *r);

// "acm", "cp210x", ... for lsusb and the bind message.
const char *z_usbh_ser_name(int kind);

// For vendor probes: the first interface (alternate setting 0) of
// class cls that has both a bulk IN and a bulk OUT endpoint. Fills
// s->iface, the endpoints and their packet sizes; returns 1, or 0 if
// there is none.
int z_usbh_ser_bulk_pair(const uint8_t *cfg, int n, int cls, z_usbh_ser_t *s);

// -- one file per vendor kind --

int z_usbh_cp210x_probe(uint16_t vid, uint16_t pid,
                        const uint8_t *cfg, int n, z_usbh_ser_t *s);
int z_usbh_cp210x_setup(const z_usbh_ser_t *s, int step,
                        z_usbh_ser_req_t *r);

#endif
