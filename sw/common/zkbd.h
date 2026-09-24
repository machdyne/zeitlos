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
 * Keysyms are Unicode codepoints for characters and values above
 * 0x10FFFF for named keys -- see "keysyms" below.
 */

#include <stdint.h>
#include <stdbool.h>

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

// -- lock keys --
//
// Usage codes of the three lock keys, and the lock STATE the kernel
// keeps from them (sw/os/hid.c): toggled on each press, shown on the
// keyboard's LEDs, and carried in every key event at bits 19:17 --
// Z_KBD_EV_LOCKS() below. The bit order is the HID keyboard LED output
// report's, so the kernel sends the same byte to the keyboard.
//
// Caps Lock is applied by wm when translating: letters only, Shift
// inverted. All three start OFF. Num Lock especially: on a compact
// keyboard the keyboard's OWN firmware turns a block of letter keys
// (7-8-9/U-I-O/J-K-L/M...) into an embedded keypad while the host says
// Num Lock is on, so on by default took those keys away. A full-size
// keypad still types digits with Num Lock off -- zkbd maps keypad keys
// to digits regardless; Num Lock does not (yet) switch them to
// navigation keys. Scroll Lock is state and an LED, nothing more.
#define Z_HID_USAGE_CAPSLOCK    0x39
#define Z_HID_USAGE_SCROLLLOCK  0x47
#define Z_HID_USAGE_NUMLOCK     0x53

#define Z_KBD_LOCK_NUM      0x01
#define Z_KBD_LOCK_CAPS     0x02
#define Z_KBD_LOCK_SCROLL   0x04

// The lock state carried in a raw key event (bits 19:17).
#define Z_KBD_EV_LOCKS(ev)  (((uint32_t)(ev) >> 17) & 0x07)

// The keyboard layout active when the key was pressed (bits 24:20) --
// an index into z_kbd_layouts[]. The kernel stamps it into every event
// (sw/os/hid.c), so a layout switch can never fall between a key and
// its translation, and a program that reads raw events itself gets the
// same layout wm does.
#define Z_KBD_EV_LAYOUT(ev) (((uint32_t)(ev) >> 20) & 0x1F)
#define Z_KBD_LAYOUT_MAX    32

// -- raw event fields --
//
// hid_read_key() (and the kernel's HID_EVENT(), sw/os/hid.c) packs one
// key edge into a non-negative int32. Use these rather than open-coding
// the shifts: the usage code is at bits 8:1, NOT 7:0 -- bit 0 is the
// pressed flag -- and getting that wrong turns every key into a
// different one.
#define Z_KBD_EV_PRESSED(ev)  ((uint32_t)(ev) & 1u)
#define Z_KBD_EV_USAGE(ev)    ((uint8_t)(((uint32_t)(ev) >> 1) & 0xFF))
#define Z_KBD_EV_MODS(ev)     ((uint8_t)(((uint32_t)(ev) >> 9) & 0xFF))

// -- keysyms --
//
// A keysym is one of two things, in disjoint ranges:
//
//   0x000000-0x10FFFF  a character, as its Unicode codepoint, already
//                      shift/ctrl/layout-resolved. ASCII is simply the
//                      bottom of this range, so every existing check
//                      like `keysym >= 0x20 && keysym < 0x7f` still
//                      means exactly what it did. Control characters
//                      stay where ASCII puts them: Enter = 0x0d,
//                      Escape = 0x1b, Tab = 0x09, Backspace = 0x7f,
//                      Ctrl+A..Z = 0x01..0x1a.
//
//   0x110000+          a named key with no character: arrows, F-keys,
//                      the navigation cluster. 0x110000 is the first
//                      value above the last Unicode codepoint, so no
//                      character a keyboard layout can ever produce
//                      lands here.
//
// Before keyboard layouts (docs/keyboard_layouts.md) the named keys
// were at 0x100+, which is Latin Extended-A -- A-macron, C-caron and
// friends. Harmless while every keysym was ASCII; a collision the
// moment a layout can type those letters. Code that uses the Z_KEY_*
// names is unaffected by the move; code that compared against 0x100
// directly is not, and there was none in the tree when it moved.
//
// Keysyms travel to apps in Z_WM_KEY (zwm.h), whose keysym field is 23
// bits wide -- room for every value here, with Z_KEY_MAX to spare.

#define Z_KEY_NONE      0x0000   // usage had no mapping (media keys,
                                  // keys the layout leaves empty, or a
                                  // bare modifier press -- see below)

#define Z_KEY_NAMED_BASE  0x110000u

#define Z_KEY_UP        (Z_KEY_NAMED_BASE + 0x00)
#define Z_KEY_DOWN      (Z_KEY_NAMED_BASE + 0x01)
#define Z_KEY_LEFT      (Z_KEY_NAMED_BASE + 0x02)
#define Z_KEY_RIGHT     (Z_KEY_NAMED_BASE + 0x03)
#define Z_KEY_HOME      (Z_KEY_NAMED_BASE + 0x04)
#define Z_KEY_END       (Z_KEY_NAMED_BASE + 0x05)
#define Z_KEY_PAGEUP    (Z_KEY_NAMED_BASE + 0x06)
#define Z_KEY_PAGEDOWN  (Z_KEY_NAMED_BASE + 0x07)
#define Z_KEY_INSERT    (Z_KEY_NAMED_BASE + 0x08)
#define Z_KEY_DELETE    (Z_KEY_NAMED_BASE + 0x09)

#define Z_KEY_F1        (Z_KEY_NAMED_BASE + 0x10)
#define Z_KEY_F2        (Z_KEY_NAMED_BASE + 0x11)
#define Z_KEY_F3        (Z_KEY_NAMED_BASE + 0x12)
#define Z_KEY_F4        (Z_KEY_NAMED_BASE + 0x13)
#define Z_KEY_F5        (Z_KEY_NAMED_BASE + 0x14)
#define Z_KEY_F6        (Z_KEY_NAMED_BASE + 0x15)
#define Z_KEY_F7        (Z_KEY_NAMED_BASE + 0x16)
#define Z_KEY_F8        (Z_KEY_NAMED_BASE + 0x17)
#define Z_KEY_F9        (Z_KEY_NAMED_BASE + 0x18)
#define Z_KEY_F10       (Z_KEY_NAMED_BASE + 0x19)
#define Z_KEY_F11       (Z_KEY_NAMED_BASE + 0x1a)
#define Z_KEY_F12       (Z_KEY_NAMED_BASE + 0x1b)

// Dead keys: a key that types nothing itself but changes the next
// letter (dead acute then e is e-acute). Only z_kbd_translate() and
// z_kbd_event_to_keysym() return these, and wm consumes them -- it
// composes the result and forwards only that (docs/
// keyboard_layouts.md, "Dead keys"). An app never receives one.
#define Z_KEY_DEAD_BASE (Z_KEY_NAMED_BASE + 0x100)
#define Z_KEY_DEAD(n)   (Z_KEY_DEAD_BASE + (uint32_t)(n))
#define Z_KEY_IS_DEAD(k) \
	((uint32_t)(k) >= Z_KEY_DEAD_BASE && (uint32_t)(k) < Z_KEY_DEAD_BASE + 0x100)

// The largest keysym Z_WM_KEY can carry (23 bits).
#define Z_KEY_MAX       0x7FFFFFu

// A named key (arrow, F-key, ...), as opposed to a character.
#define Z_KEY_IS_NAMED(k)  ((uint32_t)(k) >= Z_KEY_NAMED_BASE)

// A character that belongs in text: not a control character (C0,
// DEL, or the C1 range 0x80-0x9f), not a named key. This is the
// Unicode-aware form of the `k >= 0x20 && k < 0x7f` test that ASCII-
// only code uses; an app that stores text in something wider than
// ASCII should use this one instead. Surrogates (0xd800-0xdfff) are
// not characters and never come from a keyboard, but are excluded
// anyway so this can vet codepoints from any source.
#define Z_KEY_IS_TEXT(k) \
	((uint32_t)(k) >= 0x20 && (uint32_t)(k) != 0x7f && \
	 !((uint32_t)(k) >= 0x80 && (uint32_t)(k) < 0xa0) && \
	 !((uint32_t)(k) >= 0xd800 && (uint32_t)(k) < 0xe000) && \
	 (uint32_t)(k) < Z_KEY_NAMED_BASE)

// -- layouts --
//
// The tables are generated from xkeyboard-config -- the database every
// Linux desktop uses -- by tools/gen_kbd_layouts.py, into
// zkbd_layouts.c. See docs/keyboard_layouts.md.
//
// A layout covers the keys that differ between layouts: the main block,
// the ISO key beside left Shift (usage 0x64), the Brazilian key beside
// right Shift (0x87) and the keypad decimal. Each key has four levels:
// plain, Shift, AltGr, Shift+AltGr. A level holds a Unicode codepoint
// (all of these fit 16 bits), 0 for nothing, or a dead key
// (Z_KBD_DEAD_BASE + n). Everything else -- Enter, the arrows, F-keys,
// the keypad digits -- is the same on every layout and lives in zkbd.c.

#define Z_KBD_DEAD_BASE  0xF000		// table value for dead key 0 (private use)

typedef struct {
	uint8_t		usage;		// USB HID usage, keyboard page
	uint16_t	sym[4];		// plain, Shift, AltGr, Shift+AltGr
} z_kbd_key_t;

typedef struct {
	const char	*name;		// config name: "us", "de", "de-nodeadkeys"
	const char	*label;		// two letters, for the dock: "US", "DE"
	const char	*desc;		// "German (no dead keys)"
	uint8_t		altgr;		// right Alt is AltGr (else it is Alt)
	char		kp_decimal;	// what the keypad's decimal key types
	uint8_t		nkeys;
	const z_kbd_key_t *keys;	// sorted by usage
	// An input method on top of these keys (see "input methods"
	// below): Z_KBD_IME_NONE for an ordinary layout.
	uint8_t		ime;
} z_kbd_layout_t;

#define Z_KBD_IME_NONE      0
#define Z_KBD_IME_HIRAGANA  1
#define Z_KBD_IME_KATAKANA  2

typedef struct {
	uint16_t	dead;		// Z_KBD_DEAD_BASE + n
	uint16_t	base;		// the character typed after it
	uint16_t	result;
} z_kbd_compose_t;

extern const z_kbd_layout_t z_kbd_layouts[];
extern const int z_kbd_layout_count;
extern const uint16_t z_kbd_dead_spacing[];	// per dead key; 0 = none
extern const int z_kbd_dead_count;
extern const z_kbd_compose_t z_kbd_compose_table[];
extern const int z_kbd_compose_count;

// Layout id for a config name ("de"), or -1. Case-insensitive.
int z_kbd_layout_find(const char *name);

// The layout for an id; id 0 (US) for anything out of range.
const z_kbd_layout_t *z_kbd_layout_info(int id);

// -- translation --

// Translates one key in layout `layout`, with the event's modifier byte
// and lock state (Z_KBD_LOCK_*), into a keysym -- a character, a named
// key, a dead key (Z_KEY_DEAD(n)) or Z_KEY_NONE.
//
//   - Shift, AltGr and Caps Lock pick the level. Caps Lock acts on any
//     key whose Shift character is the capital of its plain one -- a-z,
//     but also a-umlaut, e-acute, o-slash -- and never on digits or
//     punctuation, as on every desktop.
//   - Ctrl with a key that types a letter gives its control code
//     (Ctrl+A = 0x01), following the LAYOUT's letter: on German,
//     Ctrl+Z is the key marked Z, where US has Y.
//   - On a layout with AltGr, right Alt is AltGr, not Alt. *mods_out
//     (may be NULL) gets the modifier byte to hand on: without the
//     right-Alt bit when AltGr was used to pick a character, so that an
//     app does not mistake AltGr+Q ('@' on German) for an Alt shortcut.
//     A key with nothing on its AltGr levels types its plain character
//     and keeps the bit, like a two-level key under xkb.
uint32_t z_kbd_translate(int layout, uint8_t usage, uint8_t modifiers,
	uint8_t locks, uint8_t *mods_out);

// The same for a raw event from hid_read_key(): its own usage,
// modifiers, locks and layout (Z_KBD_EV_*). This is the function to
// use for a raw event.
uint32_t z_kbd_event_to_keysym(int32_t ev, uint8_t *mods_out);

// US layout, no locks: what this has always returned. Kept for callers
// that have no event to hand -- anything with an event should use
// z_kbd_event_to_keysym(), which follows the active layout.
//
// A bare modifier (usage 0xE0-0xE7) is Z_KEY_NONE everywhere; a caller
// that wants to react to a modifier changing by itself should check the
// raw usage code before calling any of these.
uint32_t z_kbd_usage_to_keysym(uint8_t usage, uint8_t modifiers);

// -- dead keys --

// What dead key `dead` (a Z_KEY_DEAD() keysym) and the character `cp`
// typed after it combine into, or 0 if they do not combine.
uint32_t z_kbd_compose(uint32_t dead, uint32_t cp);

// The dead key's accent on its own -- what dead key then Space types,
// or the dead key pressed twice. 0 if it has none.
uint32_t z_kbd_dead_spacing_of(uint32_t dead);

// -- input methods --
//
// A layout with `ime` set (Z_KBD_IME_HIRAGANA, _KATAKANA) types kana:
// wm feeds the letters its keys produce to this converter and forwards
// the kana that come out, as ordinary characters (docs/
// keyboard_layouts.md, "Japanese input"). Pure, with no I/O, so the
// whole of it is tested on the host.
//
// Romaji to kana: Hepburn and kunrei-shiki spellings both (shi/si,
// tsu/tu, ja/zya), every ya/yu/yo combination, small kana with x- or
// l- (xtu, lya), f/v/w extensions (fa, vu, wi), a doubled consonant for
// the small tsu (kka -> っか), and n before a consonant, nn or n' for
// the syllabic n. - , . [ ] ~ / type the long-vowel mark and Japanese
// punctuation. Up to four letters wait while they could still become a
// kana; a letter that cannot passes through as itself.
//
// No kanji: conversion needs a dictionary, and which one is a
// licensing decision still to be made.

#define Z_KBD_IME_PEND  4

typedef struct {
	char		pend[Z_KBD_IME_PEND + 1];	// letters waiting, NUL-terminated
	uint8_t		n;
	uint8_t		mode;						// Z_KBD_IME_HIRAGANA / _KATAKANA
} z_kbd_ime_t;

// Most codepoints one call can produce: the pending letters given up
// as themselves, plus a kana of two.
#define Z_KBD_IME_OUT   8

// True if `cp` is something the converter takes: a letter, or one of
// its punctuation keys. Anything else should flush first and then go
// through as itself.
bool z_kbd_ime_takes(uint32_t cp);

// Feeds one character (z_kbd_ime_takes() must be true). Writes what is
// now finished to `out` and returns how many; 0 means the letters are
// still pending.
int z_kbd_ime_feed(z_kbd_ime_t *ime, uint32_t cp, uint32_t *out);

// Finishes whatever is pending -- a lone n becomes the syllabic n, any
// other leftover letters themselves -- for a key that is not romaji.
int z_kbd_ime_flush(z_kbd_ime_t *ime, uint32_t *out);

// Takes back the last pending letter. False if there was none, in which
// case the Backspace is the app's.
bool z_kbd_ime_backspace(z_kbd_ime_t *ime);

// The modifier keys held down right now, read from the USB keyboard's
// own report -- for code that must know about a modifier that is merely
// HELD, which generates no key events between its press and its release
// (wm's Alt+double-click, gpu3d's Alt+drag, the viewport following the
// pointer with Super). Only a port that reports a keyboard counts. Not
// in host builds; injected keys (the on-screen keyboard) are not seen.
uint8_t z_kbd_live_mods(void);

// -- the active layout (syscalls; not in host builds) --

// The layout the kernel is stamping into key events now.
int z_kbd_active_layout(void);

// Makes `id` the active layout for every key pressed from now on.
// Returns false for an id with no layout.
bool z_kbd_set_active_layout(int id);

#endif
