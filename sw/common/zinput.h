#ifndef ZINPUT_H
#define ZINPUT_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Visor pointer and keyboard, as the viewer page sends them, injected
 * the same way whichever path received the bytes.
 *
 * Mouse is 5 bytes: x low, x high, y low, y high, buttons. Bit 7 of
 * buttons means the pointer left the page: drop present so the USB
 * mouse has the sprite back. Otherwise present is set (rtl/sysctl.v;
 * wm reads it like a USB mouse) and the hold timer starts. A second of
 * silence does the same drop. A leave is not an input event.
 *
 * Key is 3 bytes: HID usage, modifiers, pressed (nonzero = down).
 *
 * zinput_mouse returns 1 only when a move was written. zinput_key
 * returns 1 when a key was injected. The caller counts.
 */

/* {present[24], buttons[22:20], y[19:10], x[9:0]}. buttons bit 7 is
 * not stored: it is the leave, and it never reaches this word. */
uint32_t zinput_pointer_word(uint32_t x, uint32_t y, uint32_t buttons);

/* mods in 9..16, usage in 1..8, pressed in bit 0. */
uint32_t zinput_key_word(uint32_t usage, uint32_t mods, int pressed);

/* True once `now - last` has reached `hold`. Signed, so a wrapped
 * tick count still expires instead of waiting forever. */
int zinput_stale(uint32_t now, uint32_t last, uint32_t hold);

int  zinput_mouse(const uint8_t *p, uint32_t len);
int  zinput_key(const uint8_t *p, uint32_t len);
void zinput_tick(void);		/* release if the visor has gone quiet */
void zinput_release(void);
int  zinput_held(void);
int  zinput_buttons(void);	/* 0 when the visor does not hold the sprite */

/* 1 if a move or a key arrived since the last call, then cleared.
 * A leave does not count. The caller uses it to scan again at once. */
int  zinput_take_motion(void);

#endif
