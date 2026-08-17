/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zkbd.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zkbd.h"

// USB HID Keyboard/Keypad usage page (0x07), the number-row and
// punctuation keys -- usage code plus its unshifted/shifted ASCII.
// Letters (0x04-0x1D) and Enter/Escape/Backspace/Tab/Space/F-keys/
// nav cluster (0x28 onward) are handled directly in
// z_kbd_usage_to_keysym() below instead of this table, since they're
// either a simple offset from a known base (letters, F-keys) or a
// single fixed keysym with no shift variant.
typedef struct {
	uint8_t usage;
	char unshifted;
	char shifted;
} zkbd_punct_t;

static const zkbd_punct_t punct_map[] = {
	{ 0x1E, '1', '!' }, { 0x1F, '2', '@' }, { 0x20, '3', '#' },
	{ 0x21, '4', '$' }, { 0x22, '5', '%' }, { 0x23, '6', '^' },
	{ 0x24, '7', '&' }, { 0x25, '8', '*' }, { 0x26, '9', '(' },
	{ 0x27, '0', ')' },
	{ 0x2D, '-', '_' }, { 0x2E, '=', '+' },
	{ 0x2F, '[', '{' }, { 0x30, ']', '}' },
	{ 0x31, '\\', '|' },
	{ 0x33, ';', ':' }, { 0x34, '\'', '"' },
	{ 0x35, '`', '~' },
	{ 0x36, ',', '<' }, { 0x37, '.', '>' }, { 0x38, '/', '?' },
};
#define PUNCT_MAP_LEN (sizeof(punct_map) / sizeof(punct_map[0]))

uint32_t z_kbd_usage_to_keysym(uint8_t usage, uint8_t modifiers) {

	bool shift = (modifiers & Z_KBD_MOD_SHIFT) != 0;
	bool ctrl  = (modifiers & Z_KBD_MOD_CTRL)  != 0;

	// letters: 0x04..0x1D -> a..z
	if (usage >= 0x04 && usage <= 0x1D) {
		if (ctrl) return (uint32_t)(usage - 0x04 + 1);   // Ctrl+A..Z -> 0x01..0x1A
		return (uint32_t)(shift ? ('A' + (usage - 0x04)) : ('a' + (usage - 0x04)));
	}

	// number row / punctuation
	for (uint32_t i = 0; i < PUNCT_MAP_LEN; i++) {
		if (punct_map[i].usage == usage)
			return (uint32_t)(shift ? punct_map[i].shifted : punct_map[i].unshifted);
	}

	switch (usage) {

		case 0x28: return 0x0d;   // Enter -> CR
		case 0x29: return 0x1b;   // Escape
		case 0x2A: return 0x7f;   // Backspace -- DEL, matching zeitlos.c's
		                          // readline(), which accepts either CH_BS
		                          // or CH_DEL
		case 0x2B: return '\t';  // Tab
		case 0x2C: return ' ';   // Space

		case 0x3A: return Z_KEY_F1;
		case 0x3B: return Z_KEY_F2;
		case 0x3C: return Z_KEY_F3;
		case 0x3D: return Z_KEY_F4;
		case 0x3E: return Z_KEY_F5;
		case 0x3F: return Z_KEY_F6;
		case 0x40: return Z_KEY_F7;
		case 0x41: return Z_KEY_F8;
		case 0x42: return Z_KEY_F9;
		case 0x43: return Z_KEY_F10;
		case 0x44: return Z_KEY_F11;
		case 0x45: return Z_KEY_F12;

		case 0x49: return Z_KEY_INSERT;
		case 0x4A: return Z_KEY_HOME;
		case 0x4B: return Z_KEY_PAGEUP;
		case 0x4C: return Z_KEY_DELETE;
		case 0x4D: return Z_KEY_END;
		case 0x4E: return Z_KEY_PAGEDOWN;
		case 0x4F: return Z_KEY_RIGHT;
		case 0x50: return Z_KEY_LEFT;
		case 0x51: return Z_KEY_DOWN;
		case 0x52: return Z_KEY_UP;

		default: return Z_KEY_NONE;

	}

}
