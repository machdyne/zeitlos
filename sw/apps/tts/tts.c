/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tts -- the text-to-speech service.
 *
 * Registers as "tts0", waits for Z_TTS_* messages (sw/common/ztts.h),
 * and speaks them. Apps reach it through sw/common/zspeak.h; the
 * window manager's Super keys (Super+S to start and stop it) are the
 * user's way in. See docs/tts.md.
 *
 * -- The backend --
 *
 * Every utterance is written to the UART as a transcript line (the
 * hook a braille display or a remote listener would use), and spoken
 * through the hardware mixer (tts_audio.c). On a bitstream without the
 * mixer, "speaking" takes as long as saying it would at the current
 * rate, so the queue, interruption and the Z_TTS_MARK_* replies behave
 * the same with or without sound.
 *
 * -- Lifetime --
 *
 * Started by wm on Super+S (or, later, at boot from /zeitlos.cfg).
 * Exits on Z_TTS_QUIT after saying "Speech off". A second copy
 * started while one is running exits at once.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/ztts.h"
#include "../../common/zcfg.h"
#include "tts_queue.h"
#include "tts_audio.h"
#include "pack.h"
#include "dsyn.h"

// -- voice settings (Z_TTS_SET) --

static uint32_t rate_wpm = 180;
static uint32_t pitch_hz = 110;
static uint32_t formant_pct = 100;
static bool voice_recorded = true;	// the recorded voice, when the pack has one
static bool voice_female;		// the formant voice's, when it speaks
static bool pitch_set, formants_set;	// given explicitly in the config
static uint32_t expression_pct = 100;
static uint32_t volume = 200;

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static void backend_volume(void);

// system.tts.* from /zeitlos.cfg (docs/config.md). Read at startup and
// again whenever the file has been reloaded -- checked at the start of
// each utterance, which is when a new rate or pitch can take effect,
// and costs one syscall. Z_TTS_SET changes last until the next reload.
static uint32_t cfg_gen = 0xffffffffu;

static void load_cfg(void) {
	uint32_t g = z_cfg_generation();
	if (g == cfg_gen) return;
	cfg_gen = g;
	rate_wpm = clamp((uint32_t)z_cfg_get_int("system.tts.rate", 180), 80, 450);

	// The voice: "recorded" (the default), "male" or "female".
	//
	// Recorded is a real person's voice, from the speech pack's
	// diphones (dsyn.c): measured as intelligible as the formant voice
	// in sentences and more so in single words (docs/tts_data.md).
	// Without a pack that has one, the formant voice speaks instead --
	// male, because the female formant voice measured 10-15 points less
	// intelligible.
	//
	// A female formant voice is a higher pitch AND a shorter vocal tract
	// -- formants about 17% higher -- because pitch alone only makes a
	// squeaky male voice. An explicit system.tts.pitch or
	// system.tts.formants wins over the voice.
	char v[16];
	bool have = z_cfg_get("system.tts.voice", v, sizeof(v));
	voice_female = have && (v[0] == 'f' || v[0] == 'F');
	voice_recorded = !have || v[0] == 'r' || v[0] == 'R';
	pitch_set = z_cfg_get("system.tts.pitch", v, sizeof(v));
	formants_set = z_cfg_get("system.tts.formants", v, sizeof(v));
	pitch_hz = clamp((uint32_t)z_cfg_get_int("system.tts.pitch", 110), 50, 300);
	formant_pct = clamp((uint32_t)z_cfg_get_int("system.tts.formants", 100), 85, 120);
	expression_pct = clamp((uint32_t)z_cfg_get_int("system.tts.expression", 100), 0, 200);
	volume   = clamp((uint32_t)z_cfg_get_int("system.tts.volume", 200), 0, 255);
	backend_volume();
}

// -- the backend: transcript + timing --

static bool backend_on;
static uint32_t backend_end;	// z_uptime_ticks() when the current one ends

static void transcript(const char *text, uint32_t len, uint32_t flags) {
	// One logical line per utterance, "tts: " on every physical line,
	// so the log greps cleanly and a multi-line clipboard stays
	// readable.
	fputs((flags & Z_TTS_F_SPELL) ? "tts: [spell] " : "tts: ", stdout);
	// A trailing " ..." marks a line that runs on into the next, so
	// the transcript of a read paragraph shows where the joins are.
	for (uint32_t i = 0; i < len; i++) {
		putchar(text[i]);
		if (text[i] == '\n' && i + 1 < len) fputs("tts: ", stdout);
	}
	if (flags & Z_TTS_F_CONTINUES) fputs(" ...", stdout);
	putchar('\n');
}

// How long saying it takes. Words per minute at five letters and a
// space per word -- the convention every wpm figure uses -- plus a
// short pause at the end, which is what separates two utterances in a
// queue. Spelling is paced per character. Phase 2 replaces this with
// the length of the audio actually rendered.
static uint32_t duration_ticks(uint32_t len, uint32_t flags) {
	uint32_t ms;
	if (flags & Z_TTS_F_SPELL)
		ms = len * 400u * 180u / rate_wpm;
	else
		ms = len * 60000u / (rate_wpm * 6u);
	if (!(flags & Z_TTS_F_CONTINUES)) ms += 150;	// end-of-utterance pause
	return ms * Z_TICK_HZ / 1000u + 1;
}

static bool audio_ok;

static void backend_volume(void) {
	if (audio_ok) ta_set_volume(volume);
}

static void backend_start(const char *text, uint32_t len, uint32_t flags) {
	load_cfg();
	transcript(text, len, flags);
	backend_on = true;
	if (audio_ok) {
		// Which voice speaks is settled here, per utterance: the pack can
		// come and go with the card.
		bool rec = voice_recorded && dsyn_ready();
		bool high = rec || voice_female;	// the recorded voice is a woman's
		uint32_t pitch = pitch_set ? pitch_hz : (high ? 200u : 110u);
		uint32_t fmt = formants_set ? formant_pct : (voice_female ? 117u : 100u);
		ta_use_recorded(rec);
		phon_opts_t o = { rate_wpm, pitch, (flags & Z_TTS_F_CONTINUES) != 0,
			fmt, expression_pct + 1 };
		ta_start(text, len, (flags & Z_TTS_F_SPELL) != 0, &o);
	} else {
		backend_end = z_uptime_ticks() + duration_ticks(len, flags);
	}
}

static bool backend_busy(void) {
	if (!backend_on) return false;
	if (audio_ok) return ta_busy();
	return (int32_t)(z_uptime_ticks() - backend_end) < 0;
}

static void backend_stop(void) {
	if (backend_on) printf("tts: [stop]\n");
	if (audio_ok) ta_stop();
	backend_on = false;
}

// Ticks until the current utterance ends, for the wait. Never 0: that
// means "block indefinitely" to z_proc_wait().
static uint32_t backend_remaining(void) {
	int32_t r = (int32_t)(backend_end - z_uptime_ticks());
	return r > 0 ? (uint32_t)r : 1;
}

// -- queue callbacks --

static void notify(uint32_t to, uint32_t subject, uint16_t mark) {
	// Scalar payload, nothing borrowed. A full mailbox loses it; the
	// sender's own timeout is the backstop (ztts.h).
	if (to) z_msg_new_send(to, subject, mark, z_obj_uint32(mark));
}

static const ttsq_ops_t queue_ops = { notify, backend_stop };

// -- messages --

static bool quitting;

// -- narration (Z_TTS_NARRATE, ztts.h) --
static uint32_t narrator_pid;
static uint32_t narrator_lease;	// ticks
static uint32_t narrator_until;

static bool narrator_blocks(uint32_t from) {
	if (!narrator_pid) return false;
	if ((int32_t)(z_uptime_ticks() - narrator_until) >= 0) {
		printf("tts: [narrator %lu lease expired]\n", (unsigned long)narrator_pid);
		narrator_pid = 0;
		return false;
	}
	if (from == narrator_pid) {
		narrator_until = z_uptime_ticks() + narrator_lease;
		return false;
	}
	return true;
}

static void handle(z_msg_t *m) {

	switch (m->subject) {

	case Z_TTS_SAY:
		if (narrator_blocks(m->from)) {
			uint16_t mk = (uint16_t)Z_TTS_TAG_MARK(m->tag);
			if (mk) notify(m->from, Z_TTS_MARK_CANCELLED, mk);
			break;
		}
		// Copied inside ttsq_say(), before anything else here sends a
		// message and lets the sender run again.
		ttsq_say(m->from,
			(m->obj.type == Z_STR) ? m->obj.val.str : "",
			Z_TTS_TAG_FLAGS(m->tag), (uint16_t)Z_TTS_TAG_MARK(m->tag));
		break;

	case Z_TTS_STOP:
		if (narrator_blocks(m->from)) break;
		ttsq_cancel_all();
		break;

	case Z_TTS_REPEAT:
		if (narrator_blocks(m->from)) break;
		ttsq_repeat();
		break;

	case Z_TTS_NARRATE:
		if (m->obj.type != Z_UINT32) break;
		if (m->obj.val.uint32) {
			uint32_t s = m->obj.val.uint32 > 600 ? 600 : m->obj.val.uint32;
			if (narrator_pid != m->from)
				printf("tts: [narrator is pid %lu]\n", (unsigned long)m->from);
			narrator_pid = m->from;
			narrator_lease = s * Z_TICK_HZ;
			narrator_until = z_uptime_ticks() + narrator_lease;
		} else if (m->from == narrator_pid) {
			printf("tts: [narrator released]\n");
			narrator_pid = 0;
		}
		break;

	case Z_TTS_QUIT:
		ttsq_cancel_all();
		ttsq_say(0, "Speech off", 0, 0);
		quitting = true;
		break;

	case Z_TTS_SET:
		if (m->obj.type == Z_UINT32) {
			uint32_t v = Z_TTS_SET_VALUE(m->obj.val.uint32);
			switch (Z_TTS_SET_PARAM(m->obj.val.uint32)) {
			case Z_TTS_PARAM_RATE:   rate_wpm = clamp(v, 80, 450); break;
			// Explicit, like the config key: otherwise the voice's own
			// default pitch would win and this would be ignored.
			case Z_TTS_PARAM_PITCH:  pitch_hz = clamp(v, 50, 300); pitch_set = true; break;
			case Z_TTS_PARAM_VOICE: {
				// Super+E: the recorded voice and the formant one, back and
				// forth, for the rest of the session. The service says
				// which it now is, in that voice -- or that there is no
				// recorded voice to switch to.
				const char *said;
				bool quiet = (v & Z_TTS_VOICE_QUIET) != 0;
				v &= ~Z_TTS_VOICE_QUIET;
				if (v == Z_TTS_VOICE_NEXT)
					v = voice_recorded && dsyn_ready() ? Z_TTS_VOICE_MALE : Z_TTS_VOICE_RECORDED;
				voice_recorded = (v == Z_TTS_VOICE_RECORDED);
				voice_female = (v == Z_TTS_VOICE_FEMALE);
				if (voice_recorded && !dsyn_ready()) said = "No recorded voice. Synthesised voice";
				else if (voice_recorded) said = "Recorded voice";
				else said = "Synthesised voice";
				if (quiet) printf("tts: [voice: %s]\n", said);
				else ttsq_say(0, said, Z_TTS_F_INTERRUPT, 0);
				break;
			}
			case Z_TTS_PARAM_VOLUME:
				volume = clamp(v, 0, 255);
				if (audio_ok) ta_set_volume(volume);
				break;
			}
			printf("tts: [rate %lu wpm, pitch %lu Hz, volume %lu]\n",
				(unsigned long)rate_wpm, (unsigned long)pitch_hz,
				(unsigned long)volume);
		}
		break;

	default:
		// Not ours. Ignored, like every well-behaved process ignores
		// subjects it does not know.
		break;

	}

}

int main(void) {

	uint32_t other;
	if (z_pid_lookup(Z_TTS_SERVICE, &other)) {
		printf("tts: already running as pid %lu, exiting\n", (unsigned long)other);
		return 0;
	}

	char name[24];
	if (!z_pid_register(Z_TTS_SERVICE_BASE, name, sizeof(name))) {
		printf("tts: could not register a name, exiting\n");
		return 1;
	}

	// Two copies started at the same instant can both pass the lookup
	// above; only the one that got "tts0" stays. The other's
	// registration is released with it on exit.
	if (strcmp(name, Z_TTS_SERVICE) != 0) {
		printf("tts: registered as '%s', not '%s' -- another copy won, exiting\n",
			name, Z_TTS_SERVICE);
		return 0;
	}

	audio_ok = ta_init();
	load_cfg();

	// The speech pack is optional in every sense -- see pack.h. With
	// no card, or no pack on it, the built-in dictionary and rules do
	// the work and nothing here changes.
	if (pack_open(PACK_PATH))
		printf("tts: lexicon from %s\n", PACK_PATH);

	printf("tts: running as pid %lu (%s), %s\n",
		(unsigned long)z_getpid(), name,
		audio_ok ? "voice on mixer channel 7" : "no audio mixer: transcript only");

	// Which build this is. A board running a stale binary looks exactly
	// like a change that did nothing -- which is what happened once,
	// when unzip restored old timestamps and make rebuilt nothing.
	printf("tts: build %s %s, synth %s\n", __DATE__, __TIME__, synth_build());

	ttsq_init(&queue_ops);
	ttsq_say(0, "Speech on", 0, 0);

	for (;;) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK)
			handle(&msg);

		if (ttsq_speaking() && audio_ok) ta_pump();

		if (ttsq_speaking() && !backend_busy()) {
			backend_on = false;
			ttsq_done();
		}

		if (!ttsq_speaking() && ttsq_start_next()) {
			uint32_t len, flags;
			const char *t = ttsq_current(&len, &flags);
			backend_start(t, len, flags);
		}

		if (quitting && !ttsq_busy()) break;

		// Idle: block until someone speaks. Speaking: wake when the
		// current utterance ends, or sooner for a message.
		z_proc_wait(!ttsq_speaking() ? 0 :
			audio_ok ? ta_wait_ticks() : backend_remaining());

	}

	// The mixer reads our memory: it must be stopped before that
	// memory is given back.
	if (audio_ok) ta_stop();
	pack_close();
	printf("tts: exiting\n");
	return 0;

}
