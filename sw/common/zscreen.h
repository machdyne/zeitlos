#ifndef ZSCREEN_H
#define ZSCREEN_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The half of the remote desktop that does not know who is watching.
 * It reads the 1bpp framebuffer as one instant, says which stripes
 * changed, and encodes one stripe. UDP, the WebSocket and the viewer
 * count stay with the caller.
 *
 * A stripe is 1280 bytes of the framebuffer in VRAM order: stripe n is
 * VRAM words 320n..320n+319, the same words as DIRTY's bit n. At
 * 640x480 that is rows 16n..16n+15, and the thirty of them are the
 * screen. In the other screen modes (docs/screen_modes.md) a stripe is
 * the same 1280 bytes -- 8, 10 or 12 rows, or a row and a part -- and
 * there are up to 100 of them (1280x800). A mode's last stripe can run
 * past its framebuffer.
 *
 * The stripes are taken thirty at a time, a WINDOW: window w is
 * stripes 30w..30w+29, and slot i of the copy holds stripe 30w+i. A
 * scan reads one window, the next one with something to send, so the
 * copy, every mask here and the ESP32's shadow stay thirty stripes
 * whatever the mode. At 640x480 there is one window, and everything
 * is as it was, on the wire too. In any other mode the trailer's flags
 * say which window and which mode (ZSCREEN_TRAILER_GEOM).
 *
 * DIRTY covers VRAM words 0..9599, window 0, and on a bitstream with
 * DIRTY1..3 (DIRTY's bit 31) windows 1..3 have a register each too,
 * and are read the same way. A window with no register is read whole
 * every ZSCREEN_UNTRACKED_TICKS, and what changed is read again on
 * every scan until it has been still for ZSCREEN_HOT_TICKS.
 *
 * One consumer of the DIRTY register at a time. A second reader would
 * clear bits the first still needed.
 *
 * A zeroed zscreen_t is not probed yet, which is what a static one
 * is, so the snapshot stays in .bss. dirty_hw is then 1 when this
 * bitstream has the DIRTY register and 2 when it was probed and has
 * not, and tracked how many windows have one: 0, 1 or all four.
 * zscreen_init() returns it to "not probed".
 */

#define ZSCREEN_STRIPES       30			/* a window; all of 640x480 */
#define ZSCREEN_WINDOWS       4				/* 32,768 VRAM words */
#define ZSCREEN_STRIPE_WORDS  320			/* 640/8 * 16 rows / 4 */
#define ZSCREEN_STRIPE_BYTES  (ZSCREEN_STRIPE_WORDS * 4)	/* 1280 */
/* PackBits worst case is 4n/3, not "a shade over raw". See zscreen.c. */
#define ZSCREEN_PACK_WORST    ((ZSCREEN_STRIPE_BYTES * 4 + 2) / 3 + 2)
/* Plain literal runs: 128 bytes at a time, ten control bytes on 1280. */
#define ZSCREEN_LITERAL_LEN   (ZSCREEN_STRIPE_BYTES + ZSCREEN_STRIPE_BYTES / 128)
#define ZSCREEN_ALL           0x3fffffffu		/* bit n = stripe n */
#define ZSCREEN_TRAILER       4
#define ZSCREEN_TRAILER_MAGIC 0x5A
/* The trailer's flags. At 640x480 only LAST is ever set. In any other
 * mode GEOM is set, bits 3:2 are the window, and bits 6:4 the mode:
 * Z_VMODE_*, or 7 for 1280x720 shown from a 640x360 desktop. The
 * desktop's size for each, in that order: 640x480, 800x600, 1024x768,
 * 1280x720, 800x480, 1280x800, 960x540 (1920x1080 shown twice each
 * way), 640x360. */
#define ZSCREEN_TRAILER_LAST  0x01			/* last of frame */
#define ZSCREEN_TRAILER_GEOM  0x02			/* window and mode follow */
#define ZSCREEN_TRAILER_WIN(f)  (((f) >> 2) & 3)
#define ZSCREEN_TRAILER_MODE(f) (((f) >> 4) & 7)
/* A window no DIRTY register covers is read whole this often, and a stripe
 * of it that changed is read on every scan until it has been still this
 * long (Z_TICK_HZ is 732 a second). */
#define ZSCREEN_UNTRACKED_TICKS 366
#define ZSCREEN_HOT_TICKS       1464

/* What the next scan of each window reads (zscreen_full()). NONE:
 * only stripes its DIRTY register names (or as it has no register).
 * RESET: every stripe, and a change is not a miss -- the hashes were
 * just forgotten. VERIFY: every stripe, and a stripe of a window with
 * a register that changed with no DIRTY bit is a miss. */
#define ZSCREEN_FULL_NONE     0
#define ZSCREEN_FULL_RESET    1
#define ZSCREEN_FULL_VERIFY   2

typedef struct {
	int      dirty_hw;		/* 0 not probed, 1 present, 2 absent */
	uint32_t verify_passes;
	uint32_t verify_missed;		/* stripes, summed over the passes */
	/* The last scan: the stripes of its window it returned because
	 * they were owed (zscreen_owe()), which window, and the flags its
	 * stripes carry. */
	uint32_t owed_out;
	uint8_t  win;
	uint8_t  flags;
	/* The screen mode the copy is of (ZSCREEN_TRAILER_MODE's 0-7,
	 * plus one; 0 = not read yet), and how many stripes that is. */
	uint8_t  mode;
	uint8_t  stripes;
	uint8_t  full;			/* bit w: read window w whole next time */
	uint8_t  tracked;		/* windows 0..tracked-1 have DIRTY bits */
	/* the next whole read of each tracked window is a verify; a new
	 * mode clears it, and everything after it */
	uint8_t  verify;
	uint32_t owed[ZSCREEN_WINDOWS];
	uint32_t hot[ZSCREEN_WINDOWS];	/* untracked: what changed lately */
	uint32_t hot_tick[ZSCREEN_WINDOWS];	/* untracked: tick of the last change */
	uint32_t seen[ZSCREEN_WINDOWS];	/* untracked: tick of the last whole read */
	uint32_t hash[ZSCREEN_WINDOWS * ZSCREEN_STRIPES];	/* of the last copy; 0 means never */
	/* last, so that everything above is a short offset away */
	uint32_t snap[ZSCREEN_STRIPES * ZSCREEN_STRIPE_WORDS];
} zscreen_t;

void zscreen_init(zscreen_t *s);

/* Forget the hashes, so the next scan treats every stripe as new.
 * The copy and the miss counters stay. */
void zscreen_forget(zscreen_t *s);

/* The next scan of every window reads all its stripes. RESET forgets
 * the hashes too. */
static inline void zscreen_full(zscreen_t *s, int full)
{
	if (full == ZSCREEN_FULL_RESET)
		zscreen_forget(s);
	s->full = (1u << ZSCREEN_WINDOWS) - 1;
	s->verify = full == ZSCREEN_FULL_VERIFY;
}

/* Window w's stripes in mask m are sent whatever their hash says, the
 * next time that window is scanned. zscreen_owe_all(): every stripe. */
static inline void zscreen_owe(zscreen_t *s, int w, uint32_t m)
{
	s->owed[w] |= m;
}

static inline void zscreen_owe_all(zscreen_t *s)
{
	for (int w = 0; w < ZSCREEN_WINDOWS; w++)
		s->owed[w] = ZSCREEN_ALL;
}

/* Read the screen mode, and pick the next window with something to
 * send. Read its DIRTY bits, if it has them, and clear what was read,
 * copy the stripes it needs into the slots, and return which of them
 * differ from their last copy. The stripes owed are in s->owed_out;
 * s->win is the window (slot i is stripe 30*win + i) and s->flags what
 * its trailers carry. Returns 0, with owed_out 0, when no window has
 * anything. *missed is the verify miss mask, or 0 when this scan is
 * not a verify; it may be NULL. now is z_uptime_ticks(). */
uint32_t zscreen_scan(zscreen_t *s, uint32_t now, uint32_t *missed);

/* Hash of one stripe's words, the same mix the scan uses. Never 0. */
uint32_t zscreen_hash(const uint32_t *words, int idx);

/* Which stripes a scan of window 0 reads. Pure: the scan applies it to
 * the DIRTY register it just sampled. */
uint32_t zscreen_want(int dirty_hw, uint32_t marked, int full);

/* A verify's misses: changed, but neither in the sample taken before
 * the copy nor in the register read after it. */
uint32_t zscreen_miss(uint32_t changed, uint32_t marked, uint32_t reg_after);

/* PackBits, or literal runs when PackBits would grow past
 * ZSCREEN_LITERAL_LEN. dst must hold ZSCREEN_PACK_WORST bytes.
 * Returns the payload length, which is at most ZSCREEN_LITERAL_LEN. */
int zscreen_pack(const uint8_t *raw, uint8_t *dst);

/* Four bytes: magic, frame low, frame high, flags: last, and in any
 * mode but 640x480 the window and the mode (s->flags of the last scan). */
void zscreen_trailer(const zscreen_t *s, uint8_t dst[ZSCREEN_TRAILER],
	uint16_t frame, int last);

/* Slot i's payload, then the trailer. dst must hold ZSCREEN_PACK_WORST
 * + ZSCREEN_TRAILER. Returns the total. */
int zscreen_encode(const zscreen_t *s, int i, uint8_t *dst, uint16_t frame, int last);

/* How many windows the last scan's screen mode has: 1 at 640x480. */
static inline int zscreen_windows(const zscreen_t *s)
{
	return (s->stripes + ZSCREEN_STRIPES - 1) / ZSCREEN_STRIPES;
}

/* Slot i's bytes. */
static inline const uint8_t *zscreen_slot(const zscreen_t *s, int i)
{
	return (const uint8_t *)(s->snap + i * ZSCREEN_STRIPE_WORDS);
}

#endif
