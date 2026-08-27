/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See logo.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "logo.h"

#define VRAM_BASE 0x20000000

void z_boot_logo_show(void) {
	memcpy((void *)VRAM_BASE, (const void *)Z_BOOT_LOGO_ADDR,
		Z_BOOT_LOGO_BYTES);
}
