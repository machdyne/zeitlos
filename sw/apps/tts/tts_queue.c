/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The tts service's utterance queue. See tts_queue.h.
 *
 * -- Storage --
 *
 * No malloc anywhere. The queue is a ring of TTSQ_ITEMS descriptors,
 * and their text lives in one TTSQ_ARENA-byte ring. Items are only
 * ever added at the tail and removed at the head (or all at once), so
 * the text arena is FIFO too: the live text is one contiguous run
 * from the head item's offset to `wr`, possibly wrapped. An utterance
 * never straddles the end of the arena -- if it does not fit before
 * the end it starts again at 0 -- so every item's text is a plain
 * NUL-terminated C string the backend can walk.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/ztts.h"
#include "tts_queue.h"

typedef struct {
	uint32_t	from;
	uint16_t	mark;
	uint8_t		flags;
	uint16_t	off;	// into arena
	uint16_t	len;	// excluding the NUL
} ttsq_item_t;

static const ttsq_ops_t *ops;

static ttsq_item_t items[TTSQ_ITEMS];
static uint32_t head;		// index of the oldest item
static uint32_t count;		// live items, current one included
static bool speaking;		// items[head] is being spoken

static char arena[TTSQ_ARENA];
static uint32_t wr;			// next free byte, when count > 0

static char last[TTSQ_LAST_MAX];
static uint32_t last_len;
static uint32_t last_flags;
static bool have_last;

void ttsq_init(const ttsq_ops_t *o) {
	ops = o;
	head = count = wr = 0;
	speaking = false;
	have_last = false;
	last_len = 0;
}

static void notify(const ttsq_item_t *it, uint32_t subject) {
	if (it->mark && ops && ops->notify)
		ops->notify(it->from, subject, it->mark);
}

// Room for `n` bytes, contiguous. Returns the offset, or -1.
static int32_t arena_alloc(uint32_t n) {

	if (n > TTSQ_ARENA) return -1;

	if (count == 0) {
		wr = 0;
		return 0;
	}

	uint32_t oldest = items[head].off;

	if (wr > oldest) {
		// live text is [oldest, wr): free is [wr, end) and [0, oldest)
		if (TTSQ_ARENA - wr >= n) return (int32_t)wr;
		if (oldest >= n) return 0;
		return -1;
	}

	if (wr < oldest) {
		// wrapped: live is [oldest, end) and [0, wr); free is between
		if (oldest - wr >= n) return (int32_t)wr;
		return -1;
	}

	// wr == oldest with items live: exactly full.
	return -1;

}

// Copy with the cleaning every backend would otherwise have to do:
// control characters become spaces (a stray escape sequence from a
// terminal line must not reach a synthesiser as bytes), newlines are
// kept because they are meaningful pauses, and it stops at `max`.
static uint32_t copy_clean(char *dst, const char *src, uint32_t max) {
	uint32_t i = 0;
	if (src) {
		for (; i < max && src[i]; i++) {
			unsigned char c = (unsigned char)src[i];
			if (c == '\n') dst[i] = '\n';
			else if (c < 0x20 || c == 0x7f) dst[i] = ' ';
			else dst[i] = (char)c;
		}
	}
	dst[i] = 0;
	return i;
}

void ttsq_cancel_all(void) {

	if (speaking && ops && ops->stop) ops->stop();
	speaking = false;

	// Report in queue order, so a sender that sent marks 1, 2, 3 hears
	// about them in that order.
	while (count) {
		notify(&items[head], Z_TTS_MARK_CANCELLED);
		head = (head + 1) % TTSQ_ITEMS;
		count--;
	}

	head = 0;
	wr = 0;

}

int ttsq_say(uint32_t from, const char *text, uint32_t flags, uint16_t mark) {

	ttsq_item_t it;
	it.from = from;
	it.mark = mark;
	it.flags = (uint8_t)(flags & 0xffu);
	it.off = 0;
	it.len = 0;

	if (flags & Z_TTS_F_INTERRUPT)
		ttsq_cancel_all();
	else if ((flags & Z_TTS_F_LOW) && count) {
		notify(&it, Z_TTS_MARK_CANCELLED);
		return TTSQ_DROPPED;
	}

	if (count >= TTSQ_ITEMS) {
		notify(&it, Z_TTS_MARK_CANCELLED);
		return TTSQ_DROPPED;
	}

	// Measure first, bounded, then allocate exactly.
	uint32_t n = 0;
	if (text)
		while (n < Z_TTS_UTTER_MAX - 1 && text[n]) n++;

	int32_t off = arena_alloc(n + 1);
	if (off < 0) {
		notify(&it, Z_TTS_MARK_CANCELLED);
		return TTSQ_DROPPED;
	}

	it.off = (uint16_t)off;
	it.len = (uint16_t)copy_clean(&arena[off], text, n);
	wr = (uint32_t)off + it.len + 1;

	items[(head + count) % TTSQ_ITEMS] = it;
	count++;

	return TTSQ_QUEUED;

}

bool ttsq_repeat(void) {
	if (!have_last) return false;
	// Through ttsq_say() with INTERRUPT, so it lands in the arena like
	// anything else and `last` is free to be overwritten when it
	// starts. Unmarked: nobody is waiting on a repeat.
	ttsq_say(0, last, (last_flags & ~(Z_TTS_F_LOW)) | Z_TTS_F_INTERRUPT, 0);
	return true;
}

bool ttsq_start_next(void) {

	if (speaking || count == 0) return false;

	speaking = true;

	ttsq_item_t *it = &items[head];
	uint32_t n = it->len < TTSQ_LAST_MAX - 1 ? it->len : TTSQ_LAST_MAX - 1;
	memcpy(last, &arena[it->off], n);
	last[n] = 0;
	last_len = n;
	last_flags = it->flags & ~Z_TTS_F_INTERRUPT;
	have_last = true;

	return true;

}

const char *ttsq_current(uint32_t *len, uint32_t *flags) {
	if (!speaking) return 0;
	ttsq_item_t *it = &items[head];
	if (len) *len = it->len;
	if (flags) *flags = it->flags;
	return &arena[it->off];
}

bool ttsq_speaking(void) {
	return speaking;
}

void ttsq_done(void) {

	if (!speaking) return;

	notify(&items[head], Z_TTS_MARK_DONE);
	head = (head + 1) % TTSQ_ITEMS;
	count--;
	speaking = false;
	if (count == 0) { head = 0; wr = 0; }

}

bool ttsq_busy(void) {
	return count != 0;
}

uint32_t ttsq_count(void) {
	return count;
}
