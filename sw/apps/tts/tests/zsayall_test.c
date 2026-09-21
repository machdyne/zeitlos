/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for sw/common/zsayall.c:
 *
 *     cd sw/apps/tts && make test
 *
 * Drives the reader against a scripted service: every z_speak_mark()
 * is recorded, and the test decides when each is DONE or CANCELLED.
 * The failures that matter are a read that stalls (never sends the
 * next line), one that talks over an interruption, one that loses its
 * place, and one confused by the replies to a previous read.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static uint32_t test_now;
#define Z_SAYALL_NOW()	(test_now)

#include "../../../common/zsayall.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// -- the scripted service --

typedef struct { char text[64]; uint32_t flags; uint16_t mark; } sent_t;
static sent_t sent[64];
static int nsent;
static int stops;
static bool speech_on = true;

bool z_speak_mark(const char *text, uint32_t len, uint32_t flags, uint16_t mark) {
	if (!speech_on) return false;
	sent_t *s = &sent[nsent++ % 64];
	uint32_t n = len < 63 ? len : 63;
	memcpy(s->text, text, n);
	s->text[n] = 0;
	s->flags = flags;
	s->mark = mark;
	return true;
}
void z_speak_stop(void) { stops++; }

static z_msg_t reply(uint32_t subject, uint16_t mark) {
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	m.subject = subject;
	m.tag = mark;
	return m;
}

// -- the document --

static const char *doc[] = { "one", "two", "", "four", "five" };
static int ndoc = 5;
static int at_calls[64], nat;

static int get(void *u, int n, const char **t, uint32_t *f) {
	(void)u;
	if (n < 0 || n >= ndoc) return -1;
	*t = doc[n];
	*f = (n == 0) ? Z_TTS_F_CONTINUES : 0;
	return (int)strlen(doc[n]);
}
static void at(void *u, int n) { (void)u; at_calls[nat++ % 64] = n; }

static z_sayall_t sa;

static void reset(void) {
	memset(&sa, 0, sizeof(sa));
	sa.get = get; sa.at = at;
	nsent = stops = nat = 0;
	speech_on = true;
	test_now = 100;
}

static void done(int i) { z_msg_t m = reply(Z_TTS_MARK_DONE, sent[i].mark); CHECK(z_sayall_msg(&sa, &m)); }

static void test_reads_to_the_end(void) {
	reset();
	CHECK(z_sayall_start(&sa, 0));
	CHECK(nsent == Z_SAYALL_AHEAD);				// two queued ahead
	CHECK(sent[0].flags & Z_TTS_F_INTERRUPT);	// first cuts off what was playing
	CHECK(!(sent[1].flags & Z_TTS_F_INTERRUPT));
	CHECK(sent[0].flags & Z_TTS_F_CONTINUES);	// the unit's own flag is kept
	CHECK(nat == 1 && at_calls[0] == 0);
	for (int i = 0; i < ndoc; i++) done(i);
	CHECK(nsent == ndoc);
	CHECK(strcmp(sent[2].text, "") == 0);		// blank line still sent
	CHECK(!z_sayall_active(&sa));				// finished by itself
	CHECK(stops == 0);							// ...without stopping anyone's speech
	// followed the voice: 0 at start, then 1..4 as each became current
	CHECK(nat == ndoc);
	for (int i = 0; i < ndoc; i++) CHECK(at_calls[i] == i);
}

static void test_from_middle_and_past_end(void) {
	reset();
	CHECK(z_sayall_start(&sa, 3));
	CHECK(strcmp(sent[0].text, "four") == 0);
	reset();
	CHECK(!z_sayall_start(&sa, 99));			// nothing there
	CHECK(nsent == 0 && !z_sayall_active(&sa));
}

static void test_cancel_stops_and_keeps_place(void) {
	reset();
	z_sayall_start(&sa, 0);
	done(0);									// now on "two"
	z_msg_t m = reply(Z_TTS_MARK_CANCELLED, sent[1].mark);
	CHECK(z_sayall_msg(&sa, &m));
	CHECK(!z_sayall_active(&sa));
	int sent_before = nsent;
	m = reply(Z_TTS_MARK_CANCELLED, sent[2].mark);	// the queued one's cancel
	CHECK(!z_sayall_msg(&sa, &m));				// ignored: read is over
	CHECK(nsent == sent_before);				// and nothing more was sent
	CHECK(at_calls[nat - 1] == 1);				// the place is the line being heard
}

static void test_stop(void) {
	reset();
	z_sayall_start(&sa, 0);
	z_sayall_stop(&sa);
	CHECK(stops == 1 && !z_sayall_active(&sa));
	z_sayall_stop(&sa);							// idempotent, no second stop
	CHECK(stops == 1);
}

static void test_toggle(void) {
	reset();
	z_sayall_toggle(&sa, 0);
	CHECK(z_sayall_active(&sa));
	z_sayall_toggle(&sa, 0);
	CHECK(!z_sayall_active(&sa) && stops == 1);
}

static void test_restart_ignores_old_replies(void) {
	reset();
	z_sayall_start(&sa, 0);
	uint16_t old0 = sent[0].mark, old1 = sent[1].mark;
	z_sayall_start(&sa, 3);						// Super+A twice-restart
	CHECK(z_sayall_active(&sa));
	z_msg_t m = reply(Z_TTS_MARK_CANCELLED, old0);
	CHECK(!z_sayall_msg(&sa, &m));
	m = reply(Z_TTS_MARK_CANCELLED, old1);
	CHECK(!z_sayall_msg(&sa, &m));
	CHECK(z_sayall_active(&sa));				// the new read survives
}

static void test_not_ours(void) {
	reset();
	z_sayall_start(&sa, 0);
	z_msg_t m = reply(Z_TTS_MARK_DONE, 0x7777);
	CHECK(!z_sayall_msg(&sa, &m));
	m = reply(12345, sent[0].mark);				// wrong subject
	CHECK(!z_sayall_msg(&sa, &m));
	CHECK(z_sayall_active(&sa));
}

static void test_lost_reply_skips_ahead(void) {
	reset();
	z_sayall_start(&sa, 0);
	done(1);									// reply for 0 was lost
	CHECK(z_sayall_active(&sa));
	CHECK(nsent == 4);							// refilled two ahead
}

static void test_speech_off(void) {
	reset();
	speech_on = false;
	CHECK(!z_sayall_start(&sa, 0));
	CHECK(nat == 0);							// did not move the caret
	reset();
	z_sayall_start(&sa, 0);
	speech_on = false;							// Super+S mid-read
	done(0);
	CHECK(!z_sayall_active(&sa));
}

static void test_timeout(void) {
	reset();
	z_sayall_start(&sa, 0);
	test_now += 29 * Z_TICK_HZ;
	z_sayall_poll(&sa);
	CHECK(z_sayall_active(&sa));
	test_now += 2 * Z_TICK_HZ;
	z_sayall_poll(&sa);
	CHECK(!z_sayall_active(&sa));
}

static void test_mark_zero_skipped(void) {
	reset();
	sa.seq = 0xffff;							// about to wrap
	z_sayall_start(&sa, 0);
	CHECK(sent[0].mark != 0 && sent[1].mark != 0);
}

int main(void) {
	test_reads_to_the_end();
	test_from_middle_and_past_end();
	test_cancel_stops_and_keeps_place();
	test_stop();
	test_toggle();
	test_restart_ignores_old_replies();
	test_not_ours();
	test_lost_reply_skips_ahead();
	test_speech_off();
	test_timeout();
	test_mark_zero_skipped();
	if (fails) { printf("zsayall_test: %d FAILED\n", fails); return 1; }
	printf("zsayall_test: all passed\n");
	return 0;
}
