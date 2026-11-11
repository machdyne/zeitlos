#ifndef Z_USBH_DIG_H
#define Z_USBH_DIG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * USB host stack -- digitizers: touch screens and pen tablets
 * (docs/touch.md).
 *
 * A touch screen or a pen tablet is a HID device that reports where,
 * not how far: X and Y as absolute values in a range of its own. That
 * is not a boot report, so the hardware's byte mux cannot carry it the
 * way it carries a mouse; the device gets a RAW poll slot and each
 * report is read here, in software, and written to the virtual mouse
 * (reg_vmouse, rtl/sysctl.v) scaled to the screen -- with its TOUCH bit
 * for a finger, so wm reads tap, drag and hold, and without it for a
 * pen, which hovers and is as precise as a mouse.
 *
 * What is understood, from the report descriptor:
 *
 *   - a Touch Screen (or Touch Pad) collection (Digitizers 0x04/0x05):
 *     the first contact's Tip Switch, X and Y; with a Contact Count,
 *     two fingers turn the wheel;
 *   - a Pen collection (Digitizers 0x02): Tip Switch, Barrel Switch
 *     (the right button), In Range (hover), X and Y;
 *   - a mouse whose X and Y are absolute (Generic Desktop 0x02): its
 *     buttons, X and Y -- the "mouse mode" many touch monitors and
 *     tablets start in.
 *
 * Untested on hardware (November 2026): the parser and the decoding
 * are tested on the host against descriptors written to the HID usage
 * tables (tests/test_dig.c). Windows-style multi-touch devices start
 * in single-touch mode until the host sets their Device Mode feature;
 * nothing sets it, so they are read as they start.
 *
 * Pure: no registers here, so the host tests run it as it is.
 */

#include <stdint.h>

#define DIG_NONE    0
#define DIG_TOUCH   1   // a finger: TOUCH in the virtual mouse
#define DIG_PEN     2   // a pen: hovers, a mouse's precision
#define DIG_ABS     3   // a mouse with absolute X and Y

// Where the first contact's fields are in its input report, in bits
// from the report's start (after the report ID byte, if any). -1: not
// there.
typedef struct {
    uint8_t  kind;          // DIG_*
    uint8_t  id;            // the report's ID, 0 if the device uses none
    int16_t  x_bit, y_bit;
    uint8_t  x_sz, y_sz;
    int32_t  x_min, x_max, y_min, y_max;
    int16_t  tip_bit;       // Tip Switch, or a mouse's button 1
    int16_t  btn2_bit;      // Barrel Switch, or a mouse's button 2
    int16_t  range_bit;     // In Range (a pen)
    int16_t  cnt_bit;       // Contact Count (a touch screen)
    uint8_t  cnt_sz;
    uint16_t bytes;         // the report's length, ID byte included
} z_usbh_dlay_t;

// What decoding remembers between reports.
typedef struct {
    uint8_t  wheel;         // the six-bit count in the virtual mouse
    uint8_t  two;           // two fingers down
    int16_t  two_y;         // where they were, the last notch
    uint32_t last;          // the last word written
} z_usbh_dstate_t;

// Parses a HID report descriptor. Returns 1 and fills `l` if it is a
// digitizer this file understands, 0 if not.
int z_usbh_dig_parse(const uint8_t *rd, int n, z_usbh_dlay_t *l);

// The virtual mouse word for one input report (Z_VMOUSE_MAKE layout,
// sw/common/zsoc.h): x and y scaled to 640x480, the button(s), present,
// TOUCH for a finger, the wheel. 0 hands the pointer back to the mouse
// (a pen out of range). Returns 1 and sets *word if the report is the
// digitizer's, 0 if it is another report (another ID) to ignore.
int z_usbh_dig_decode(const z_usbh_dlay_t *l, z_usbh_dstate_t *st,
                      const uint8_t *r, int n, uint32_t *word);

// The interface a digitizer is on, if the configuration has a HID
// interface that is not a boot keyboard, with an interrupt IN endpoint:
// its number, and the endpoint, its packet size and interval; and the
// report descriptor's length. -1 if there is none.
int z_usbh_dig_iface(const uint8_t *cfg, int n, int skip_iface,
                     uint8_t *ep, uint8_t *mps, uint8_t *ival, int *rdlen);

#endif
