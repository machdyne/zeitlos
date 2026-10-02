/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zinput.h.
 *
 * net used to OR present on every mouse packet and never clear it, so
 * a single visor packet froze the USB pointer until the FPGA was
 * reset. Last writer wins: a visor packet takes the sprite, and visor
 * silence of a second (tab closed, pointer left the canvas) drops
 * present so the USB mux in rtl/sysctl.v wins without a wiggle. wm
 * also clears present on a USB move. buttons bit 7 is an explicit
 * leave from the viewer page.
 */

#include <stdint.h>

#include "zinput.h"
#include "zeitlos.h"	/* reg_vmouse, hid_inject, z_wm_wake, z_uptime_ticks */
#include "zsoc.h"	/* Z_TICK_HZ */

static uint32_t last_tick;
static int held;
static int motion;

uint32_t zinput_pointer_word(uint32_t x, uint32_t y, uint32_t buttons)
{
	return (1u << 24) | ((buttons & 7) << 20)
		| ((y & 0x3ff) << 10) | (x & 0x3ff);
}

uint32_t zinput_key_word(uint32_t usage, uint32_t mods, int pressed)
{
	return ((mods & 0xff) << 9) | ((usage & 0xff) << 1) | (pressed ? 1u : 0u);
}

int zinput_stale(uint32_t now, uint32_t last, uint32_t hold)
{
	return (int32_t)(now - last) >= (int32_t)hold;
}

void zinput_release(void)
{
	reg_vmouse = 0;
	held = 0;
	/* wm sleeps until HID or this poke -- dropping present must
	 * wake it so a held button is not stuck down. */
	z_wm_wake();
}

int zinput_held(void)
{
	return held;
}

int zinput_buttons(void)
{
	if (!held)
		return 0;
	return (int)((reg_vmouse >> 20) & 7);
}

int zinput_take_motion(void)
{
	int m = motion;
	motion = 0;
	return m;
}

int zinput_mouse(const uint8_t *p, uint32_t len)
{
	uint32_t x, y, b;

	if (len < 5)
		return 0;
	x = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
	y = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
	b = p[4];
	if (b & 0x80) {
		zinput_release();
		return 0;
	}
	reg_vmouse = zinput_pointer_word(x, y, b);
	last_tick = z_uptime_ticks();
	held = 1;
	motion = 1;
	z_wm_wake();
	return 1;
}

int zinput_key(const uint8_t *p, uint32_t len)
{
	if (len < 3)
		return 0;
	hid_inject((int32_t)zinput_key_word(p[0], p[1], p[2]));
	motion = 1;
	return 1;
}

void zinput_tick(void)
{
	if (!held)
		return;
	if (!zinput_stale(z_uptime_ticks(), last_tick, Z_TICK_HZ))
		return;
	zinput_release();
}
