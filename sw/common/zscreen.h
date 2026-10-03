#ifndef ZSCREEN_H
#define ZSCREEN_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The half of the remote desktop that does not know who is watching.
 * It reads the 640x480 1bpp framebuffer as one instant, says which of
 * the 30 stripes changed, and encodes one stripe. UDP, the WebSocket
 * and the viewer count stay with the caller.
 *
 * One consumer of the DIRTY register at a time. A second reader would
 * clear bits the first still needed.
 *
 * A zeroed zscreen_t is not probed yet, which is what a static one
 * is, so the snapshot stays in .bss. dirty_hw is then 1 when this
 * bitstream has the DIRTY register and 2 when it was probed and has
 * not. zscreen_init() returns it to "not probed".
 */

#define ZSCREEN_STRIPES       30
#define ZSCREEN_STRIPE_WORDS  320			/* 640/8 * 16 rows / 4 */
#define ZSCREEN_STRIPE_BYTES  (ZSCREEN_STRIPE_WORDS * 4)	/* 1280 */
/* PackBits worst case is 4n/3, not "a shade over raw". See zscreen.c. */
#define ZSCREEN_PACK_WORST    ((ZSCREEN_STRIPE_BYTES * 4 + 2) / 3 + 2)
/* Plain literal runs: 128 bytes at a time, ten control bytes on 1280. */
#define ZSCREEN_LITERAL_LEN   (ZSCREEN_STRIPE_BYTES + ZSCREEN_STRIPE_BYTES / 128)
#define ZSCREEN_ALL           0x3fffffffu		/* bit n = stripe n */
#define ZSCREEN_TRAILER       4
#define ZSCREEN_TRAILER_MAGIC 0x5A
#define ZSCREEN_TRAILER_LAST  0x01			/* flags bit 0: last of frame */

/* What the next scan reads. NONE: only stripes DIRTY names (or every
 * stripe, when the bitstream has no DIRTY register). RESET: every
 * stripe, and a change is not a miss -- the hashes were just forgotten.
 * VERIFY: every stripe, and a stripe that changed with no DIRTY bit is
 * a miss. */
#define ZSCREEN_FULL_NONE     0
#define ZSCREEN_FULL_RESET    1
#define ZSCREEN_FULL_VERIFY   2

typedef struct {
	uint32_t snap[ZSCREEN_STRIPES * ZSCREEN_STRIPE_WORDS];
	uint32_t hash[ZSCREEN_STRIPES];	/* of the last copy; 0 means never */
	int      dirty_hw;		/* 0 not probed, 1 present, 2 absent */
	uint32_t verify_passes;
	uint32_t verify_missed;		/* stripes, summed over the passes */
} zscreen_t;

void zscreen_init(zscreen_t *s);

/* Forget the hashes, so the next scan treats every stripe as new.
 * The copy and the miss counters stay. */
void zscreen_forget(zscreen_t *s);

/* Read DIRTY (the first time, probe for it), clear what was read,
 * copy those stripes, and return which of them differ from the last
 * copy. *missed is the verify miss mask, or 0 when this scan is not a
 * verify; it may be NULL. */
uint32_t zscreen_scan(zscreen_t *s, int full, uint32_t *missed);

/* Hash of one stripe's words, the same mix the scan uses. Never 0. */
uint32_t zscreen_hash(const uint32_t *words, int idx);

/* Which stripes a scan reads. Pure: the scan applies it to the DIRTY
 * register it just sampled. */
uint32_t zscreen_want(int dirty_hw, uint32_t marked, int full);

/* A verify's misses: changed, but neither in the sample taken before
 * the copy nor in the register read after it. */
uint32_t zscreen_miss(uint32_t changed, uint32_t marked, uint32_t reg_after);

/* PackBits, or literal runs when PackBits would grow past
 * ZSCREEN_LITERAL_LEN. dst must hold ZSCREEN_PACK_WORST bytes.
 * Returns the payload length, which is at most ZSCREEN_LITERAL_LEN. */
int zscreen_pack(const uint8_t *raw, uint8_t *dst);

/* Four bytes: magic, frame low, frame high, flags. */
void zscreen_trailer(uint8_t dst[ZSCREEN_TRAILER], uint16_t frame, int last);

/* Payload then trailer. dst must hold ZSCREEN_PACK_WORST +
 * ZSCREEN_TRAILER. Returns the total. */
int zscreen_encode(const uint8_t *raw, uint8_t *dst, uint16_t frame, int last);

#endif
