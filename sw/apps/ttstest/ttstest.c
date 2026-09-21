/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * ttstest -- measure what z_speak() costs, and check the tts
 * protocol end to end, on the real machine.
 *
 * From the kernel shell (serial console):
 *
 *     > run ttstest
 *
 * UART output only; no window. Best run with speech OFF, so it can
 * measure the "nobody listening" path first. It then starts `tts`
 * itself, measures the "listening" path, runs the protocol checks,
 * and stops `tts` again if it was the one that started it.
 *
 * -- What the numbers mean --
 *
 * rdcycle and rdinstret are wall-clock counters: if this process is
 * preempted mid-call, that call's sample includes whoever ran
 * meanwhile. So the MINIMUM is the cost of the call itself, and the
 * median is what it typically costs in context. The maximum is
 * noise and is not printed. See docs/tts.md, "Cost", for the figures
 * this is expected to reproduce.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zcycles.h"
#include "../../common/ztts.h"
#include "../../common/zaudio.h"
#include "../../common/zspeak.h"

static inline uint32_t instret(void) {
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

// -- small statistics --

#define NSAMP 256
static uint32_t cyc[NSAMP], ins[NSAMP];

static void sort(uint32_t *a, int n) {
	for (int i = 1; i < n; i++) {
		uint32_t v = a[i];
		int j = i - 1;
		while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
		a[j + 1] = v;
	}
}

static void report(const char *what, int n) {
	sort(cyc, n);
	sort(ins, n);
	uint32_t c = cyc[0];
	printf("ttstest: %-34s min %6lu cyc %5lu ins (%lu.%02lu us)   median %6lu cyc\n",
		what, (unsigned long)c, (unsigned long)ins[0],
		(unsigned long)(c / 48), (unsigned long)((c % 48) * 100 / 48),
		(unsigned long)cyc[n / 2]);
}

// The empty-measurement overhead, subtracted from every sample so the
// figures are the call alone.
static uint32_t base_cyc, base_ins;

static void calibrate(void) {
	for (int i = 0; i < NSAMP; i++) {
		uint32_t c0 = z_cycles(), i0 = instret();
		__asm__ volatile ("" ::: "memory");
		uint32_t i1 = instret(), c1 = z_cycles();
		cyc[i] = c1 - c0; ins[i] = i1 - i0;
	}
	sort(cyc, NSAMP); sort(ins, NSAMP);
	base_cyc = cyc[0]; base_ins = ins[0];
}

#define MEASURE(idx, stmt) do { \
	uint32_t c0 = z_cycles(), i0 = instret(); \
	stmt; \
	uint32_t i1 = instret(), c1 = z_cycles(); \
	cyc[idx] = (c1 - c0) - base_cyc; ins[idx] = (i1 - i0) - base_ins; \
} while (0)

// -- waiting for marks --

static int fails;

// Waits for a mark reply; returns its subject, or 0 on timeout.
// Messages that are not mark replies are discarded -- nothing else
// talks to this process.
static uint32_t wait_mark(uint16_t mark, uint32_t timeout_ms) {
	uint32_t deadline = z_uptime_ticks() + timeout_ms * Z_TICK_HZ / 1000 + 1;
	for (;;) {
		z_msg_t m;
		while (z_msg_read(&m) == Z_OK) {
			if ((m.subject == Z_TTS_MARK_DONE || m.subject == Z_TTS_MARK_CANCELLED) &&
			    m.tag == mark)
				return m.subject;
		}
		int32_t left = (int32_t)(deadline - z_uptime_ticks());
		if (left <= 0) return 0;
		z_proc_wait((uint32_t)left);
	}
}

static void expect(const char *what, uint16_t mark, uint32_t want) {
	uint32_t got = wait_mark(mark, 10000);
	bool ok = got == want;
	printf("ttstest: %s %s (mark %u: %s)\n", ok ? "PASS" : "FAIL", what, mark,
		got == Z_TTS_MARK_DONE ? "done" : got == Z_TTS_MARK_CANCELLED ? "cancelled" : "no reply");
	if (!ok) fails++;
}

// -- the parts --

static void measure_absent(void) {

	for (int i = 0; i < NSAMP; i++)
		MEASURE(i, z_speak("Save, button", Z_TTS_F_INTERRUPT));
	report("z_speak, speech off (cached)", NSAMP);

	for (int i = 0; i < 64; i++) {
		z_speak_forget();
		MEASURE(i, z_speak("Save, button", Z_TTS_F_INTERRUPT));
	}
	report("z_speak, speech off (lookup)", 64);

}

static void measure_present(void) {

	// One at a time, each followed by waiting for it to be spoken, so
	// the service's mailbox never backs up and every sample is the
	// plain send path.
	for (int i = 0; i < 16; i++) {
		MEASURE(i, z_speak_mark("OK", 2, 0, (uint16_t)(100 + i)));
		wait_mark((uint16_t)(100 + i), 5000);
	}
	report("z_speak, speech on (2 bytes)", 16);

	static const char line[] =
		"The quick brown fox jumps over the lazy dog, twice, for luck.";
	for (int i = 0; i < 16; i++) {
		MEASURE(i, z_speak_mark(line, sizeof(line) - 1, 0, (uint16_t)(200 + i)));
		wait_mark((uint16_t)(200 + i), 5000);
	}
	report("z_speak, speech on (61 bytes)", 16);

}

static void protocol(void) {

	// In order.
	z_speak_mark("one", 3, 0, 1);
	z_speak_mark("two", 3, 0, 2);
	z_speak_mark("three", 5, 0, 3);
	expect("queued utterance 1 spoken", 1, Z_TTS_MARK_DONE);
	expect("queued utterance 2 spoken", 2, Z_TTS_MARK_DONE);
	expect("queued utterance 3 spoken", 3, Z_TTS_MARK_DONE);

	// Interrupt.
	z_speak_mark("This is a long sentence that will not be allowed to finish.",
		UINT32_MAX, 0, 10);
	z_speak_mark("queued behind it", UINT32_MAX, 0, 11);
	z_speak_mark("Interrupted.", UINT32_MAX, Z_TTS_F_INTERRUPT, 12);
	expect("interrupt cancels the current one", 10, Z_TTS_MARK_CANCELLED);
	expect("interrupt cancels the queued one", 11, Z_TTS_MARK_CANCELLED);
	expect("the interrupting one is spoken", 12, Z_TTS_MARK_DONE);

	// Low priority.
	z_speak_mark("Busy speaking this.", UINT32_MAX, 0, 20);
	z_speak_mark("low priority chatter", UINT32_MAX, Z_TTS_F_LOW, 21);
	expect("low priority is dropped while busy", 21, Z_TTS_MARK_CANCELLED);
	expect("and what was busy still finishes", 20, Z_TTS_MARK_DONE);
	z_speak_mark("low priority when idle", UINT32_MAX, Z_TTS_F_LOW, 22);
	expect("low priority is spoken when idle", 22, Z_TTS_MARK_DONE);

	// Stop.
	z_speak_mark("This will be stopped.", UINT32_MAX, 0, 30);
	z_speak_stop();
	expect("stop cancels", 30, Z_TTS_MARK_CANCELLED);

	// Spelling and repeat have no reply to check; they are here so the
	// transcript shows them.
	z_speak("Zeitlos", Z_TTS_F_SPELL);
	z_speak_repeat();
	z_speak_mark("end of protocol checks", UINT32_MAX, 0, 40);
	expect("repeat and spell leave the queue working", 40, Z_TTS_MARK_DONE);

}

int main(void) {

	printf("ttstest: z_speak() cost and tts protocol checks\n");

	// What the voice has to come out of. If speech is silent, this is
	// the first thing to know: tts needs audio, and uses the mixer when
	// the bitstream has one and the plain FIFO when it does not.
	if (!z_audio_present()) {
		printf("ttstest: audio: NONE in this bitstream -- tts can only print\n");
	} else {
		uint32_t f = z_audio_formats();
		printf("ttstest: audio: present, mixer %s, DAC%s%s%s, FIFO %lu frames\n",
			z_audio_mixer_present() ? "YES" : "no (tts uses the FIFO)",
			(f & Z_AUDIO_FORMAT_SD) ? " sigma-delta" : "",
			(f & Z_AUDIO_FORMAT_PT8211) ? " PT8211" : "",
			(f & Z_AUDIO_FORMAT_SPDIF) ? " S/PDIF" : "",
			(unsigned long)z_audio_depth());
	}

	calibrate();
	printf("ttstest: measurement overhead %lu cyc %lu ins (subtracted)\n",
		(unsigned long)base_cyc, (unsigned long)base_ins);

	bool started = false;
	uint32_t pid;

	if (z_pid_lookup(Z_TTS_SERVICE, &pid)) {
		printf("ttstest: tts already running (pid %lu) -- skipping the speech-off\n"
			"ttstest: measurements; turn speech off (Super+S) and run again for those\n",
			(unsigned long)pid);
	} else {
		measure_absent();
		printf("ttstest: starting tts\n");
		if (!z_proc_run("tts")) {
			printf("ttstest: FAIL could not start tts -- is it installed?\n");
			return 1;
		}
		started = true;
		// Wait for it to register (it has to load first).
		uint32_t deadline = z_uptime_ticks() + 3 * Z_TICK_HZ;
		while (!z_pid_lookup(Z_TTS_SERVICE, &pid)) {
			if ((int32_t)(z_uptime_ticks() - deadline) > 0) {
				printf("ttstest: FAIL tts started but never registered\n");
				return 1;
			}
			z_proc_wait(Z_TICK_HZ / 50);
		}
		z_speak_forget();
	}

	// Fast, so the checks do not take all day at the default rate.
	z_speak_set(Z_TTS_PARAM_RATE, 450);

	measure_present();
	protocol();

	z_speak_set(Z_TTS_PARAM_RATE, 180);

	if (started) {
		printf("ttstest: stopping tts\n");
		z_msg_new_send(pid, Z_TTS_QUIT, 0, z_obj_none());
	}

	if (fails) printf("ttstest: %d check(s) FAILED\n", fails);
	else printf("ttstest: all checks passed\n");

	return fails ? 1 : 0;

}
