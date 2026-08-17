#ifndef ZKBD_H
#define ZKBD_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * USB HID keyboard usage codes -> keysyms.
 *
 * sw/os/hid.c turns the raw USB HID boot-protocol keyboard report
 * (rtl/usb_hid.v) into press/release *events* carrying a raw HID
 * usage code + the report's modifier byte -- see its own file header
 * comment. This header is the next layer up: translating a usage
 * code into something an app actually wants (an ASCII character, or
 * a named key for things with no ASCII representation, like arrow
 * keys). That translation happens in wm.c (see docs/window_manager.md
 * for why wm owns input dispatch), not the kernel -- keyboard layout
 * knowledge doesn't need to live in the trusted, harder-to-iterate-on
 * kernel, and this way it can change without a kernel rebuild+reflash.
 *
 * US QWERTY only -- there's no layout selection mechanism yet.
 */

#include <stdint.h>

// -- USB HID boot-report modifier byte -- one bit per modifier key,
// matches byte 0 of a standard USB HID keyboard boot report exactly
// (and rtl/usb_hid.v's key_modifiers field, which is that same byte).

#define Z_KBD_MOD_LCTRL   0x01
#define Z_KBD_MOD_LSHIFT  0x02
#define Z_KBD_MOD_LALT    0x04
#define Z_KBD_MOD_LGUI    0x08
#define Z_KBD_MOD_RCTRL   0x10
#define Z_KBD_MOD_RSHIFT  0x20
#define Z_KBD_MOD_RALT    0x40
#define Z_KBD_MOD_RGUI    0x80

#define Z_KBD_MOD_SHIFT   (Z_KBD_MOD_LSHIFT | Z_KBD_MOD_RSHIFT)
#define Z_KBD_MOD_CTRL    (Z_KBD_MOD_LCTRL  | Z_KBD_MOD_RCTRL)
#define Z_KBD_MOD_ALT     (Z_KBD_MOD_LALT   | Z_KBD_MOD_RALT)
#define Z_KBD_MOD_GUI     (Z_KBD_MOD_LGUI   | Z_KBD_MOD_RGUI)

// USB HID usage IDs (Keyboard/Keypad page, 0x07) for the modifier
// keys themselves -- 0xE0..0xE7, fixed by the USB HID spec. These
// never appear in a report's key1..key4 fields (only in the modifier
// byte), but sw/os/hid.c synthesizes a pseudo key-usage event in this
// same range when a modifier bit itself changes, so a caller that
// cares which physical modifier changed (not just the current
// modifier byte alongside some other key) can still see that.
#define Z_HID_USAGE_LCTRL   0xE0
#define Z_HID_USAGE_LSHIFT  0xE1
#define Z_HID_USAGE_LALT    0xE2
#define Z_HID_USAGE_LGUI    0xE3
#define Z_HID_USAGE_RCTRL   0xE4
#define Z_HID_USAGE_RSHIFT  0xE5
#define Z_HID_USAGE_RALT    0xE6
#define Z_HID_USAGE_RGUI    0xE7

// -- keysyms --
//
// 0x00-0x7f: ordinary ASCII, already shift/ctrl-resolved by
// z_kbd_usage_to_keysym() below. 0x100+: named keys with no ASCII
// representation. Deliberately disjoint ranges -- a keysym is either
// printable ASCII or a special key, never ambiguous -- so a caller
// can just check `keysym < 0x100` to tell the two apart.

#define Z_KEY_NONE      0x0000   // usage had no mapping (media keys,
                                  // non-US layout keys, or a bare
                                  // modifier press -- see below)

#define Z_KEY_UP        0x0100
#define Z_KEY_DOWN      0x0101
#define Z_KEY_LEFT      0x0102
#define Z_KEY_RIGHT     0x0103
#define Z_KEY_HOME      0x0104
#define Z_KEY_END       0x0105
#define Z_KEY_PAGEUP    0x0106
#define Z_KEY_PAGEDOWN  0x0107
#define Z_KEY_INSERT    0x0108
#define Z_KEY_DELETE    0x0109

#define Z_KEY_F1        0x0110
#define Z_KEY_F2        0x0111
#define Z_KEY_F3        0x0112
#define Z_KEY_F4        0x0113
#define Z_KEY_F5        0x0114
#define Z_KEY_F6        0x0115
#define Z_KEY_F7        0x0116
#define Z_KEY_F8        0x0117
#define Z_KEY_F9        0x0118
#define Z_KEY_F10       0x0119
#define Z_KEY_F11       0x011a
#define Z_KEY_F12       0x011b

// translate a USB HID keyboard-page usage code (as found in a boot
// report's key1..key4 fields, or as synthesized by sw/os/hid.c for a
// modifier-only change) plus the report's modifier byte into a
// keysym. returns Z_KEY_NONE for a usage this table doesn't know
// about, and always for the synthesized modifier pseudo-usages
// (0xE0-0xE7) themselves, since a bare modifier press has no keysym
// of its own -- a caller that wants to react to modifier presses
// specifically should check the raw usage code before calling this.
//
// Ctrl+letter returns the usual control-code mapping (Ctrl+A = 0x01
// .. Ctrl+Z = 0x1A), matching every other terminal convention.
uint32_t z_kbd_usage_to_keysym(uint8_t usage, uint8_t modifiers);

#endif
