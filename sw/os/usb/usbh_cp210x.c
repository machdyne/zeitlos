/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host: Silicon Labs CP210x USB-UART bridges. See usbh_ser.h and
 * docs/usb_host.md, "USB serial devices".
 *
 * Written from Silicon Labs' public application note AN571, "CP210x
 * Virtual COM Port Interface". No driver source was consulted.
 *
 * A CP210x is one vendor-class interface (0xff) with a bulk IN and a
 * bulk OUT endpoint. Received data is plain bytes -- no per-packet
 * header, unlike FTDI -- so once the UART is enabled and configured it
 * runs on the generic data path in usbh_cdc.c exactly as a CDC-ACM
 * device does. Everything specific to the chip is here, and it is
 * four control requests.
 *
 * The first user is a Heltec WiFi LoRa 32 V3 running Meshtastic
 * (docs/mesh_app.md), whose ESP32-S3 console sits behind a CP2102.
 */

#include <stddef.h>

#include "usbh_ser.h"

// Vendor request, host to device, recipient interface. Every CP210x
// request that configures the UART is addressed to its interface.
#define CP_OUT_IFACE        0x41

#define CP_IFC_ENABLE       0x00
#define CP_SET_LINE_CTL     0x03
#define CP_SET_MHS          0x07
#define CP_SET_BAUDRATE     0x1e

// SET_LINE_CTL wValue: stop bits in bits 3:0 (0 = one), parity in
// 7:4 (0 = none), word length in 15:8.
#define CP_LINE_8N1         0x0800

// SET_MHS wValue: bit 0 DTR, bit 1 RTS; bits 8 and 9 are the masks
// saying which of the two this request changes.
//
// ONE request, BOTH lines, ALWAYS. On an ESP32 board these two drive
// the EN / IO0 auto-reset transistors esptool uses: with one asserted
// and not the other the chip is held in reset, or restarted into its
// ROM bootloader. Both asserted, or both clear, leaves it running.
// Setting them in two writes passes through the dangerous state in
// between; a single write with both masks does not.
//
// Both asserted is what Linux does on open(), and so the state a
// Meshtastic node is known to work in with the Python CLI. If a board
// is found that misbehaves with it, CP_MHS_BOTH_CLEAR (0x0300) is the
// alternative -- still atomic, still safe for the auto-reset circuit.
#define CP_MHS_BOTH_SET     0x0303
#define CP_MHS_BOTH_CLEAR   0x0300
#define CP_MHS_VALUE        CP_MHS_BOTH_SET

// The CP210x family's IDs. ea60 is the default for the CP2102,
// CP2102N, CP2103, CP2104 and CP2109 -- one port, one interface --
// which is what boards like the Heltec V3 carry.
//
// Rebadged parts with their own IDs exist in quantity. They are added
// here as they turn up; the probe also checks the interface shape, so
// an ID match on the wrong kind of device still binds nothing.
// Multi-port parts (CP2105 ea70, CP2108 ea71) are deliberately absent:
// this driver binds one interface, and which port of several is "the"
// serial port is a question nobody has asked yet.
static const uint16_t cp_ids[][2] = {
    { 0x10c4, 0xea60 },
};

int z_usbh_cp210x_probe(uint16_t vid, uint16_t pid,
                        const uint8_t *cfg, int n, z_usbh_ser_t *s)
{
    unsigned i;
    int known = 0;

    for (i = 0; i < sizeof(cp_ids) / sizeof(cp_ids[0]); i++)
        if (cp_ids[i][0] == vid && cp_ids[i][1] == pid) known = 1;
    if (!known) return 0;

    // The first vendor-class interface, with its bulk pair.
    if (!z_usbh_ser_bulk_pair(cfg, n, 0xff, s)) return 0;
    s->kind = Z_USBH_SER_CP210X;
    return Z_USBH_SER_CP210X;
}

int z_usbh_cp210x_setup(const z_usbh_ser_t *s, int step, z_usbh_ser_req_t *r)
{
    r->type = CP_OUT_IFACE;
    r->idx = s->iface;
    r->len = 0;
    r->fatal = 0;

    switch (step) {
    case 0:
        // Fatal: the UART is off until this succeeds, and a bound
        // device that passes no data is worse than one that failed
        // enumeration visibly (lsusb says where).
        r->req = CP_IFC_ENABLE;
        r->val = 1;
        r->fatal = 1;
        return 1;
    case 1:
        r->req = CP_SET_BAUDRATE;
        r->val = 0;
        r->len = 4;
        r->data[0] = (uint8_t)(Z_USBH_SER_BAUD);
        r->data[1] = (uint8_t)(Z_USBH_SER_BAUD >> 8);
        r->data[2] = (uint8_t)(Z_USBH_SER_BAUD >> 16);
        r->data[3] = (uint8_t)(Z_USBH_SER_BAUD >> 24);
        return 1;
    case 2:
        r->req = CP_SET_LINE_CTL;
        r->val = CP_LINE_8N1;
        return 1;
    case 3:
        r->req = CP_SET_MHS;
        r->val = CP_MHS_VALUE;
        return 1;
    default:
        return 0;
    }
}
