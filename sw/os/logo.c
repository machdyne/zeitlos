/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See logo.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "logo.h"

#define VRAM_BASE 0x20000000
#define SCREEN_W 640
#define SCREEN_H 480

void z_boot_logo_show(bool invert) {

	const int x_offset = (SCREEN_W - Z_BOOT_LOGO_W) / 2;   // 64px, 8 bytes
	const int y_offset = (SCREEN_H - Z_BOOT_LOGO_H) / 2;   // 48px

	const int screen_stride_bytes = SCREEN_W / 8;   // 80
	const int logo_stride_bytes = Z_BOOT_LOGO_W / 8;   // 64

	volatile uint8_t *vram = (volatile uint8_t *)VRAM_BASE;

	for (int row = 0; row < Z_BOOT_LOGO_H; row++) {

		int dst_row_start = (y_offset + row) * screen_stride_bytes + (x_offset / 8);
		int src_row_start = row * logo_stride_bytes;

		for (int col = 0; col < logo_stride_bytes; col++) {
			uint8_t b = z_boot_logo_data[src_row_start + col];
			vram[dst_row_start + col] = invert ? (uint8_t)(~b) : b;
		}

	}

}
