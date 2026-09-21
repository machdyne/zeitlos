/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for sw/common/zspeak.c, against a stubbed kernel:
 *
 *     cd sw/apps/tts && make test
 *
 * What matters about the client is what it does NOT do: look the
 * service up on every call, copy text when nobody is listening, or
 * keep sending to a pid that has gone away. Each of those is counted
 * here rather than timed, because a count is exact on any machine.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// The client's clock, under test control.
static uint32_t test_now;
#define Z_SPEAK_NOW()			(test_now)
#define Z_SPEAK_CACHE_CYCLES	1000u

#include "../../../common/zspeak.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// -- the stubbed kernel --

static bool svc_up;
static uint32_t svc_pid = 12;
static bool mailbox_full;
static int lookups, sends;
static uint32_t last_to, last_subject, last_tag;
static z_obj_t last_obj;

bool z_pid_lookup(const char *name, uint32_t *pid) {
	lookups++;
	if (strcmp(name, "tts0") != 0 || !svc_up) return false;
	*pid = svc_pid;
	return true;
}

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	// A send to a pid that is no longer the service fails, as the
	// kernel's does for a dead pid.
	if (!svc_up || to != svc_pid || mailbox_full) return Z_FAIL;
	sends++;
	last_to = to; last_subject = subject; last_tag = tag; last_obj = obj;
	return Z_OK;
}

z_obj_t z_obj_none(void) { z_obj_t o; memset(&o, 0, sizeof(o)); o.type = Z_NONE; return o; }
z_obj_t z_obj_uint32(uint32_t u) { z_obj_t o; memset(&o, 0, sizeof(o)); o.type = Z_UINT32; o.val.uint32 = u; return o; }

static void reset(bool up) {
	svc_up = up; mailbox_full = false;
	lookups = sends = 0;
	test_now = 5000;
	z_speak_forget();
}

static void test_absent_is_cached(void) {
	reset(false);
	for (int i = 0; i < 100; i++) CHECK(!z_speak("hello", 0));
	CHECK(lookups == 1);			// one scan, then the cache
	CHECK(sends == 0);
	test_now += 999;
	CHECK(!z_speak("x", 0));
	CHECK(lookups == 1);			// still inside the window
	test_now += 1;
	CHECK(!z_speak("x", 0));
	CHECK(lookups == 2);			// window expired: asks again
}

static void test_absent_does_not_touch_text(void) {
	reset(false);
	// A pointer that would crash if dereferenced: the absent path must
	// return before looking at the text at all.
	CHECK(!z_speak((const char *)(uintptr_t)1, 0));
	CHECK(!z_speak_n((const char *)(uintptr_t)1, 5, 0));
}

static void test_present(void) {
	reset(true);
	CHECK(z_speak("Save, button", Z_TTS_F_INTERRUPT));
	CHECK(lookups == 1 && sends == 1);
	CHECK(last_subject == Z_TTS_SAY);
	CHECK(Z_TTS_TAG_FLAGS(last_tag) == Z_TTS_F_INTERRUPT);
	CHECK(Z_TTS_TAG_MARK(last_tag) == 0);
	CHECK(last_obj.type == Z_STR && strcmp(last_obj.val.str, "Save, button") == 0);
	for (int i = 0; i < 10; i++) z_speak("again", 0);
	CHECK(lookups == 1 && sends == 11);
}

static void test_turns_on_within_window(void) {
	reset(false);
	CHECK(!z_speak("a", 0));
	svc_up = true;				// Super+S, somewhere else
	CHECK(!z_speak("b", 0));		// cached absent: not yet seen
	test_now += 1000;
	CHECK(z_speak("c", 0));		// found at the next lookup
}

static void test_exit_forgets_immediately(void) {
	reset(true);
	CHECK(z_speak("a", 0));
	svc_up = false;				// the service exits
	CHECK(!z_speak("b", 0));		// send fails...
	int l = lookups;
	CHECK(!z_speak("c", 0));		// ...so this one looks again at once
	CHECK(lookups == l + 1);
	CHECK(!z_speak("d", 0));		// and now "absent" is cached
	CHECK(lookups == l + 1);
}

static void test_ring_copies(void) {
	reset(true);
	char buf[32];
	const char *sent[Z_SPEAK_RING];
	for (int i = 0; i < Z_SPEAK_RING; i++) {
		snprintf(buf, sizeof(buf), "line %d", i);
		z_speak(buf, 0);
		sent[i] = last_obj.val.str;
		CHECK(sent[i] != buf);		// never the caller's own buffer
	}
	// Each of the last Z_SPEAK_RING sends still reads as itself after
	// the caller's buffer has been overwritten.
	strcpy(buf, "clobbered");
	for (int i = 0; i < Z_SPEAK_RING; i++) {
		snprintf(buf, sizeof(buf), "line %d", i);
		CHECK(strcmp(sent[i], buf) == 0);
	}
}

static void test_truncates_and_n(void) {
	reset(true);
	char big[400];
	memset(big, 'z', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	z_speak(big, 0);
	CHECK(strlen(last_obj.val.str) == Z_SPEAK_SLOT_MAX - 1);
	z_speak_n("abcdef", 3, 0);
	CHECK(strcmp(last_obj.val.str, "abc") == 0);
}

static void test_mark_and_static(void) {
	reset(true);
	z_speak_mark("m", 1, 0, 0xbeef);
	CHECK(Z_TTS_TAG_MARK(last_tag) == 0xbeef);
	static const char lit[] = "Speech on";
	z_speak_static(lit, 0);
	CHECK(last_obj.val.str == lit);	// no copy
}

static void test_commands(void) {
	reset(true);
	z_speak_stop();
	CHECK(last_subject == Z_TTS_STOP);
	z_speak_repeat();
	CHECK(last_subject == Z_TTS_REPEAT);
	z_speak_set(Z_TTS_PARAM_RATE, 300);
	CHECK(last_subject == Z_TTS_SET);
	CHECK(Z_TTS_SET_PARAM(last_obj.val.uint32) == Z_TTS_PARAM_RATE);
	CHECK(Z_TTS_SET_VALUE(last_obj.val.uint32) == 300);
	uint32_t pid = 0;
	CHECK(z_speak_service_pid(&pid) && pid == svc_pid);
	// commands with speech off are silent no-ops
	reset(false);
	z_speak_stop();
	z_speak_repeat();
	CHECK(sends == 0);
}

static void test_full_mailbox(void) {
	reset(true);
	mailbox_full = true;
	CHECK(!z_speak("dropped", 0));
	mailbox_full = false;
	CHECK(z_speak("fine", 0));		// found again, one extra lookup
	CHECK(lookups == 2);
}

int main(void) {
	test_absent_is_cached();
	test_absent_does_not_touch_text();
	test_present();
	test_turns_on_within_window();
	test_exit_forgets_immediately();
	test_ring_copies();
	test_truncates_and_n();
	test_mark_and_static();
	test_commands();
	test_full_mailbox();
	if (fails) { printf("zspeak_test: %d FAILED\n", fails); return 1; }
	printf("zspeak_test: all passed\n");
	return 0;
}
