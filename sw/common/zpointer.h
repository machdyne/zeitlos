#ifndef ZPOINTER_H
#define ZPOINTER_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Pointer speed: system.mouse.speed in zeitlos.cfg, applied by wm at
 * start and whenever the file changes, and by settings at once
 * (docs/user_input.md, "Pointer speed").
 *
 * The cursor is moved in hardware -- rtl/usb_hid.v, or the USB host
 * controller's compat blocks (rtl/usb/usb_hid_compat.v) -- so the speed
 * is a register of each HID port: register 5 (0xc0000014, 0xc0000034),
 * a multiplier in sixteenths in bits 5:0 -- 16 the board's own speed,
 * 8 half, 32 double, 1 to 63 -- under a signature, 0x535045 ("SPE"), in
 * bits 31:8. The config value is the multiplier.
 *
 * FOUND BY READING, NEVER BY WRITING. Before this register the old HID
 * block acked no write at all, so a write to it on an older bitstream
 * would hang the bus. A read is always safe: on such a bitstream
 * register 5 returns whatever the last read did, so register 0 is read
 * first -- its top half is never the signature.
 *
 * settings steps through Z_POINTER_LEVELS of them, each about 19%
 * (2^(1/4)) faster than the last: a quarter to (nearly) four times.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zsoc.h"

#define Z_POINTER_LEVELS		17
#define Z_POINTER_LEVEL_NORMAL	8		// the index of 16

static const uint8_t z_pointer_levels[Z_POINTER_LEVELS] = {
	4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 23, 27, 32, 38, 45, 54, 63,
};

#define Z_POINTER_SIG		0x535045u

// Port `p`'s (0 or 1) register `r`.
#define Z_HID_REG(p, r)		(*(volatile uint32_t *)(0xc0000000u + (uint32_t)(p) * 0x20u + (uint32_t)(r) * 4u))

// Port p has the speed register.
static inline bool z_pointer_port_has(int p) {
	(void)Z_HID_REG(p, 0);			// the last read, if 5 is not there
	return (Z_HID_REG(p, 5) >> 8) == Z_POINTER_SIG;
}

static inline bool z_pointer_speed_available(void) {
	return z_soc_has_feature(Z_FEATURE_USB_HID) && z_pointer_port_has(0);
}

// The level nearest a multiplier (a value someone typed into the file
// may be between two).
static inline int z_pointer_level_of(int32_t m) {
	int best = Z_POINTER_LEVEL_NORMAL;
	int32_t bd = 1000;
	if (m < 1 || m > 63) m = 16;
	for (int i = 0; i < Z_POINTER_LEVELS; i++) {
		int32_t d = m - z_pointer_levels[i];
		if (d < 0) d = -d;
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

// Both ports, each only if it has the register.
static inline void z_pointer_speed_set(int32_t m) {
	if (m < 1 || m > 63 || !z_soc_has_feature(Z_FEATURE_USB_HID)) return;
	for (int p = 0; p < 2; p++)
		if (z_pointer_port_has(p)) Z_HID_REG(p, 5) = (uint32_t)m;
}

#endif
