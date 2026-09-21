/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for sw/apps/tts/tts_queue.c:
 *
 *     cd sw/apps/tts && make test
 *
 * The queue decides what a blind user hears and in what order, and
 * whether a reader waiting on a mark is ever told. The failures worth
 * catching are the quiet ones: a mark that is never answered (a
 * say-all that hangs), an interrupt that leaves stale speech queued
 * (holding an arrow key and hearing every item you passed), text that
 * wraps the arena and comes out as another utterance's bytes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../../common/ztts.h"
#include "../tts_queue.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// -- recorded callbacks --

typedef struct { uint32_t to, subject; uint16_t mark; } note_t;
static note_t notes[256];
static int nnotes;
static int stops;

static void cb_notify(uint32_t to, uint32_t subject, uint16_t mark) {
	if (nnotes < 256) { notes[nnotes].to = to; notes[nnotes].subject = subject; notes[nnotes].mark = mark; }
	nnotes++;
}
static void cb_stop(void) { stops++; }
static const ttsq_ops_t ops = { cb_notify, cb_stop };

static void reset(void) { ttsq_init(&ops); nnotes = 0; stops = 0; }

// Speak one: start it, check its text, finish it.
static void speak_one(const char *expect) {
	CHECK(ttsq_start_next());
	uint32_t len;
	const char *t = ttsq_current(&len, NULL);
	CHECK(t != NULL);
	if (t && expect) {
		if (strcmp(t, expect) != 0) printf("  got '%s' want '%s'\n", t, expect);
		CHECK(strcmp(t, expect) == 0);
		CHECK(len == strlen(expect));
	}
	ttsq_done();
}

static void test_fifo_and_marks(void) {
	reset();
	CHECK(!ttsq_busy());
	CHECK(ttsq_say(7, "one", 0, 1) == TTSQ_QUEUED);
	CHECK(ttsq_say(7, "two", 0, 0) == TTSQ_QUEUED);
	CHECK(ttsq_say(8, "three", 0, 3) == TTSQ_QUEUED);
	CHECK(ttsq_count() == 3);
	speak_one("one");
	speak_one("two");
	speak_one("three");
	CHECK(!ttsq_busy());
	CHECK(!ttsq_start_next());
	// unmarked "two" produces nothing
	CHECK(nnotes == 2);
	CHECK(notes[0].to == 7 && notes[0].subject == Z_TTS_MARK_DONE && notes[0].mark == 1);
	CHECK(notes[1].to == 8 && notes[1].subject == Z_TTS_MARK_DONE && notes[1].mark == 3);
}

static void test_start_is_idempotent(void) {
	reset();
	ttsq_say(1, "a", 0, 0);
	ttsq_say(1, "b", 0, 0);
	CHECK(ttsq_start_next());
	CHECK(!ttsq_start_next());		// already speaking "a"
	CHECK(strcmp(ttsq_current(NULL, NULL), "a") == 0);
}

static void test_interrupt(void) {
	reset();
	ttsq_say(5, "first", 0, 10);
	ttsq_say(5, "second", 0, 11);
	CHECK(ttsq_start_next());		// "first" is being spoken
	ttsq_say(6, "now", Z_TTS_F_INTERRUPT, 12);
	CHECK(stops == 1);				// the audio was told to stop
	CHECK(nnotes == 2);				// both earlier marks cancelled, in order
	CHECK(notes[0].mark == 10 && notes[0].subject == Z_TTS_MARK_CANCELLED);
	CHECK(notes[1].mark == 11 && notes[1].subject == Z_TTS_MARK_CANCELLED);
	CHECK(ttsq_count() == 1);
	CHECK(!ttsq_speaking());
	speak_one("now");
	CHECK(notes[2].mark == 12 && notes[2].subject == Z_TTS_MARK_DONE);
}

static void test_interrupt_while_idle_does_not_stop(void) {
	reset();
	ttsq_say(5, "x", Z_TTS_F_INTERRUPT, 0);
	CHECK(stops == 0);				// nothing was playing
	CHECK(ttsq_count() == 1);
}

static void test_low(void) {
	reset();
	CHECK(ttsq_say(1, "idle", Z_TTS_F_LOW, 0) == TTSQ_QUEUED);	// nothing busy: spoken
	CHECK(ttsq_say(1, "chatter", Z_TTS_F_LOW, 4) == TTSQ_DROPPED);
	CHECK(nnotes == 1 && notes[0].subject == Z_TTS_MARK_CANCELLED && notes[0].mark == 4);
	CHECK(ttsq_count() == 1);
}

static void test_stop(void) {
	reset();
	ttsq_say(2, "a", 0, 1);
	ttsq_say(2, "b", 0, 2);
	ttsq_start_next();
	ttsq_cancel_all();
	CHECK(stops == 1);
	CHECK(!ttsq_busy());
	CHECK(nnotes == 2);
	// cancelling an empty queue is harmless and silent
	ttsq_cancel_all();
	CHECK(stops == 1 && nnotes == 2);
}

static void test_repeat(void) {
	reset();
	CHECK(!ttsq_repeat());			// nothing said yet
	ttsq_say(1, "hello", Z_TTS_F_SPELL, 9);
	speak_one("hello");
	ttsq_say(1, "queued", 0, 0);
	CHECK(ttsq_repeat());
	CHECK(ttsq_count() == 1);		// the queued one was discarded
	CHECK(ttsq_start_next());
	uint32_t flags;
	CHECK(strcmp(ttsq_current(NULL, &flags), "hello") == 0);
	CHECK(flags & Z_TTS_F_SPELL);	// spelled again, as it was
	// repeat carries no mark: nobody hears about it twice
	int before = nnotes;
	ttsq_done();
	CHECK(nnotes == before);
}

static void test_sanitise(void) {
	reset();
	ttsq_say(1, "a\tb\x1b[1mc\nd\x7f", 0, 0);
	speak_one("a b [1mc\nd ");
}

static void test_empty_and_null(void) {
	reset();
	// An empty marked line must still be answered: a reader pacing
	// itself on marks would otherwise stop at the first blank line.
	CHECK(ttsq_say(3, "", 0, 20) == TTSQ_QUEUED);
	CHECK(ttsq_say(3, NULL, 0, 21) == TTSQ_QUEUED);
	speak_one("");
	speak_one("");
	CHECK(nnotes == 2 && notes[1].mark == 21 && notes[1].subject == Z_TTS_MARK_DONE);
}

static void test_truncation(void) {
	reset();
	static char big[Z_TTS_UTTER_MAX * 2];
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	CHECK(ttsq_say(1, big, 0, 0) == TTSQ_QUEUED);
	CHECK(ttsq_start_next());
	uint32_t len;
	ttsq_current(&len, NULL);
	CHECK(len == Z_TTS_UTTER_MAX - 1);
}

static void test_item_limit(void) {
	reset();
	for (int i = 0; i < TTSQ_ITEMS; i++)
		CHECK(ttsq_say(1, "w", 0, 0) == TTSQ_QUEUED);
	CHECK(ttsq_say(1, "over", 0, 99) == TTSQ_DROPPED);
	CHECK(nnotes == 1 && notes[0].mark == 99 && notes[0].subject == Z_TTS_MARK_CANCELLED);
}

// The arena is the part most likely to be subtly wrong: fill it,
// drain part of it, wrap, and check every utterance still comes out
// with its own text. Distinct lengths and contents so that any
// overlap shows up as a mismatch rather than as identical bytes.
static void test_arena_wrap(void) {
	reset();
	char buf[1200];
	char expect[64][1200];
	int queued = 0, spoken = 0;
	uint32_t seed = 1;

	for (int round = 0; round < 4000; round++) {
		// Add until full or item-limited.
		for (;;) {
			seed = seed * 1103515245u + 12345u;
			int n = (int)((seed >> 16) % 1100);
			for (int i = 0; i < n; i++) buf[i] = (char)('a' + ((round + i + queued) % 26));
			buf[n] = 0;
			if (queued - spoken >= TTSQ_ITEMS) break;
			if (ttsq_say(1, buf, 0, 0) != TTSQ_QUEUED) break;
			strcpy(expect[queued % 64], buf);
			queued++;
		}
		// Speak a few.
		seed = seed * 1103515245u + 12345u;
		int k = 1 + (int)((seed >> 16) % 5);
		for (int j = 0; j < k && spoken < queued; j++) {
			CHECK(ttsq_start_next());
			const char *t = ttsq_current(NULL, NULL);
			if (strcmp(t, expect[spoken % 64]) != 0) {
				printf("  arena mismatch at utterance %d\n", spoken);
				fails++;
				return;
			}
			ttsq_done();
			spoken++;
		}
	}
	// It must also have actually used the whole arena, or the test
	// proved nothing about wrapping.
	CHECK(queued > 1000);
}

// A full arena must refuse, not overwrite what is being spoken.
static void test_arena_full_protects_current(void) {
	reset();
	static char big[3000];
	memset(big, 'q', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	CHECK(ttsq_say(1, big, 0, 0) == TTSQ_QUEUED);
	CHECK(ttsq_start_next());
	CHECK(ttsq_say(1, big, 0, 0) == TTSQ_QUEUED);
	// 3000+3000 used of 8192: a third does not fit
	CHECK(ttsq_say(1, big, 0, 7) == TTSQ_DROPPED);
	const char *t = ttsq_current(NULL, NULL);
	CHECK(t[0] == 'q' && t[2998] == 'q' && t[2999] == 0);
}

int main(void) {
	test_fifo_and_marks();
	test_start_is_idempotent();
	test_interrupt();
	test_interrupt_while_idle_does_not_stop();
	test_low();
	test_stop();
	test_repeat();
	test_sanitise();
	test_empty_and_null();
	test_truncation();
	test_item_limit();
	test_arena_wrap();
	test_arena_full_protects_current();
	if (fails) { printf("tts_test: %d FAILED\n", fails); return 1; }
	printf("tts_test: all passed\n");
	return 0;
}
