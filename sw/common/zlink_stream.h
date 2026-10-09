#ifndef ZLINK_STREAM_H
#define ZLINK_STREAM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's stream transport: a GPIO stream engine in zlink mode
 * (sw/common/zgpio_stream.h, docs/gpio.md). docs/zlink.md.
 *
 * Full duplex, one wire each way, up to 12 Mbit/s. A frame goes out as
 * K27.7 (start), its bytes, K29.7 (end); the engine fills the gaps with
 * K28.5, which the receiving engine uses to align and never passes on.
 * A start symbol in the middle of a frame abandons it and starts the
 * next; a frame longer than ZL_FRAME_MAX is dropped. Damage inside a
 * frame is the link layer's CRC's to find.
 *
 * The engine must already be claimed and configured -- zlink mode,
 * Z_GS_RXEN, TX and RX on their pins, the rate -- by whoever decided
 * which wire is which (sw/apps/zlink does it after the soft link has
 * found out). This only moves frames.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zlink.h"

typedef struct {
	int engine;
	zl_transport_t t;
	bool in_frame;
	uint8_t rx[ZL_FRAME_MAX];
	uint16_t rx_len;
	uint32_t frames_rx, dropped;
} zl_stream_t;

void zl_stream_init(zl_stream_t *s, int engine);

#endif
