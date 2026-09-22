/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The tts service's voice. See tts_audio.h.
 *
 * -- Streaming --
 *
 * Exactly the shape sw/apps/play uses (docs/play_app.md): the channel
 * loops over a ring in this process's memory at the synthesiser's own
 * rate, the mixer's STEP resamples to the output rate in gateware, and
 * MIXPOS says how far it has read. We render ahead of it. The CPU
 * never touches a 44.1kHz sample.
 *
 * The mixer never stops by itself on a looping ring -- it would replay
 * the last lap forever -- so once the speech is all rendered, silence
 * is written ahead of the read position until it passes the end, and
 * then the channel is switched off.
 *
 * -- Sharing --
 *
 * Only channel TA_CHANNEL is ever written. MIXEN and EN are turned on
 * if they are off, and nothing else is touched. An audio app that
 * starts later may clear every channel (docs/audio.md, "Arbitration");
 * the read position then stops moving, which is detected, and the
 * utterance is abandoned rather than waited on forever.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"
#include "../../common/zcycles.h"
#include "synth.h"
#include "text2ph.h"
#include "tts_audio.h"
#include "dsyn.h"

// 32KB: 16384 16-bit samples, ~1.5s ahead. It was 8KB (~370ms), and
// launching a program -- `term` streaming off the card -- kept the CPU
// busy for longer than that, so speech ran dry and cut out. The size
// costs nothing in latency: a new utterance starts after TA_PREFILL,
// and a stop or an interrupt silences the channel at once.
#define TA_RING		32768
#define TA_SLICE	4410		// bytes rendered per pump at most: ~200ms
#define TA_GUARD	256			// never write this close behind the reader
#define TA_PREFILL	2048		// bytes rendered before the channel starts

static uint8_t ring[TA_RING] __attribute__((aligned(4)));

// Two ways to reach the DAC. The MIXER, when the bitstream has one:
// the hardware reads our ring by DMA, on its own channel, alongside
// whatever else is playing. Otherwise the plain FIFO, which this
// service feeds itself -- and which it shares with nothing, so speech
// and another player on a mixerless board take turns rather than mix.
// (rtl/boards.vh: `AUDIO_MIXER is optional, and the expensive half.)
enum { TA_NONE, TA_MIXER, TA_FIFO };
static int mode;
static uint32_t up;			// FIFO: output samples per synthesised one
static uint32_t rd;			// FIFO: bytes of the ring fed to the FIFO
static uint32_t underruns;

static bool fmt16;
static uint32_t bps;			// bytes per sample, 1 or 2
static uint8_t gain = 200;

static bool playing;
static bool gen_done;
static uint32_t wr;				// bytes written since the trigger
static uint32_t end_abs;		// where the speech ends, once gen_done
static uint32_t laps, lastpos;
static uint32_t start_ticks;		// when the channel was triggered
static uint32_t cleared;		// ring bytes behind the reader, zeroed up to here
static uint32_t behind_ms;		// how far the writer fell behind, all told
static uint32_t stuck_since;
static uint32_t stuck_pos;

static const char *txt;
static uint32_t txt_len, txt_pos;
static bool txt_spell;
static phon_opts_t opts;
static char phbuf[1536];

// Rendering cost, reported once per utterance -- the number that says
// whether this CPU keeps up (docs/tts.md, "Cost").
static uint32_t cpu_cycles;

// Same conversion as play.c's phys_of(): the mixer is a bus master and
// needs a physical address; our pointers are MTU-relative.
static uint32_t phys_of(const void *p) {
	return reg_mtu_base + ((uint32_t)p - 0x80000000u);
}

bool ta_init(void) {

	if (!z_audio_present()) {
		mode = TA_NONE;
		printf("tts: no audio hardware in this bitstream\n");
		return false;
	}

	if (z_audio_mixer_present()) {
		mode = TA_MIXER;
		fmt16 = z_audio_mixer_fmt16();
		bps = fmt16 ? 2 : 1;
		printf("tts: audio: mixer, channel %d, %s samples\n",
			TA_CHANNEL, fmt16 ? "16-bit" : "8-bit");
		return true;
	}

	// The FIFO takes 16-bit stereo frames at a rate set globally. The
	// voice is made at 11025Hz, which the hardware can run at directly
	// (11029Hz, 0.04% off) -- except over S/PDIF, which cannot go that
	// low, so there each sample is sent four times at 44.1kHz.
	mode = TA_FIFO;
	fmt16 = true;
	bps = 2;
	up = (z_audio_formats() & Z_AUDIO_FORMAT_SPDIF) ? 4 : 1;
	printf("tts: audio: no mixer, feeding the FIFO at %s (%lu frames deep)\n",
		up == 4 ? "44.1kHz" : "11kHz", (unsigned long)z_audio_depth());
	return true;

}

// FIFO mode: move what has been rendered into the FIFO, as much as it
// will take. Cheap enough to call after every frame rendered, which is
// what keeps a shallow FIFO from running dry while the synthesiser is
// busy.
static void fifo_feed(void) {
	uint32_t space = z_audio_space() / up;
	while (space && rd + 2 <= wr) {
		uint32_t at = rd % TA_RING;
		int32_t v = (int16_t)((uint16_t)ring[at] | ((uint16_t)ring[at + 1] << 8));
		v = v * (int32_t)gain / 256;
		for (uint32_t k = 0; k < up; k++)
			z_audio_push_unchecked((int16_t)v, (int16_t)v);
		rd += 2;
		space--;
	}
	if (z_audio_underrun()) {
		underruns++;
		z_audio_clear_underrun();
	}
}

void ta_set_volume(uint32_t v) {
	gain = (uint8_t)(v > 255 ? 255 : v);
	if (playing)
		// EN without TRIG: change the gain without restarting the ring.
		Z_AUDIO_CH_CTRL(TA_CHANNEL) = z_audio_ch_ctrl_fmt(gain, gain, true, false, 0, fmt16);
}

uint32_t ta_wait_ticks(void) {
	if (mode != TA_FIFO) return Z_TICK_HZ / 25;	// 40ms, well inside the ring
	// A quarter of the FIFO's playing time: it holds tens of
	// milliseconds, not the ring's hundreds.
	uint32_t ms = z_audio_depth() * 1000u / (SYNTH_FS * up) / 4;
	uint32_t t = ms * Z_TICK_HZ / 1000u;
	return t ? t : 1;
}

static uint32_t read_abs(void) {
	if (mode == TA_FIFO) return rd;
	uint32_t pos = z_audio_ch_pos_bytes(TA_CHANNEL) % TA_RING;
	// Which lap of the ring the reader is on, from the time since the
	// trigger rather than by counting wraps: counting needs a look at
	// least once a lap, and a stall longer than the ring (1.5s) lost the
	// count -- after which the writer wrote where nobody would hear it.
	uint64_t expect = (uint64_t)(z_uptime_ticks() - start_ticks) * SYNTH_FS * bps / Z_TICK_HZ;
	uint32_t l = expect > pos ? (uint32_t)((expect - pos + TA_RING / 2) / TA_RING) : 0;
	laps = l;
	lastpos = pos;
	return laps * TA_RING + pos;
}

static void put(const int16_t *s, int n) {
	for (int i = 0; i < n; i++) {
		uint32_t at = wr % TA_RING;
		if (fmt16) {
			ring[at] = (uint8_t)s[i];
			ring[at + 1] = (uint8_t)((uint16_t)s[i] >> 8);
		} else {
			ring[at] = (uint8_t)(s[i] >> 8);
		}
		wr += bps;
	}
}

// The recorded voice (dsyn.c) instead of the formant synthesiser, for
// the utterances that follow. tts.c decides, per utterance.
static bool recorded;

void ta_use_recorded(bool on) {
	recorded = on;
}

// Next chunk of text into the phoneme generator. False when the text
// is finished.
static bool next_chunk(void) {
	bool more;
	while (text2ph_chunk(txt, txt_len, &txt_pos, txt_spell, phbuf, sizeof(phbuf), &more)) {
		phon_opts_t o = opts;
		// Every chunk but the last runs on into the next.
		if (more) o.continues = true;
		if (!phon_begin(phbuf, &o)) continue;
		// The recorded voice takes the chunk's phones, timing and pitch
		// from the phoneme layer, all at once, and makes the sound.
		if (recorded && !dsyn_begin()) continue;
		return true;
	}
	return false;
}

// Render until `limit` absolute bytes are written or the speech ends.
static void render_to(uint32_t limit) {
	static int16_t buf[SYNTH_FRAME];
	synth_frame_t fr;
	while (!gen_done && wr + SYNTH_FRAME * bps <= limit) {
		if (recorded) {
			uint32_t c0 = z_cycles();
			int got = dsyn_render(buf, SYNTH_FRAME);
			cpu_cycles += z_cycles() - c0;
			if (got == 0) {
				// The next chunk's words, lexicon lookups and units, all
				// read from the card: counted, because it is where the
				// time goes, and it used to be left out.
				c0 = z_cycles();
				bool more = next_chunk();
				cpu_cycles += z_cycles() - c0;
				if (!more) { gen_done = true; end_abs = wr; break; }
				continue;
			}
			put(buf, (uint32_t)got);
			if (mode == TA_FIFO) fifo_feed();
			continue;
		}
		if (!phon_next(&fr)) {
			uint32_t c0 = z_cycles();
			bool more = next_chunk();
			cpu_cycles += z_cycles() - c0;
			if (!more) { gen_done = true; end_abs = wr; break; }
			continue;
		}
		uint32_t c0 = z_cycles();
		synth_render(&fr, buf, SYNTH_FRAME);
		put(buf, SYNTH_FRAME);
		cpu_cycles += z_cycles() - c0;
		if (mode == TA_FIFO) fifo_feed();
	}
	if (gen_done) {
		// Silence ahead of the reader, so the ring's old contents are
		// never heard again.
		static const int16_t zero[64];
		while (wr + 64 * bps <= limit) put(zero, 64);
	}
}

static void report(void) {
	uint32_t samples = end_abs / bps;
	if (!samples) return;
	uint32_t audio_ms = samples * 1000u / SYNTH_FS;
	uint32_t cpu_ms = cpu_cycles / (Z_SYSCLK_HZ / 1000u);
	printf("tts: [%lu ms of speech, %lu ms to render: %lu%% of one CPU%s]\n",
		(unsigned long)audio_ms, (unsigned long)cpu_ms,
		(unsigned long)(audio_ms ? cpu_ms * 100u / audio_ms : 0),
		underruns ? ", FIFO RAN DRY" : "");
	if (behind_ms)
		printf("tts: fell behind by %lu ms in all -- heard as gaps\n", (unsigned long)behind_ms);
}

void ta_stop(void) {
	if (mode == TA_FIFO) {
		if (playing) z_audio_stop();
	} else {
		Z_AUDIO_CH_CTRL(TA_CHANNEL) = 0;
	}
	if (playing && gen_done) report();
	playing = false;
}

void ta_start(const char *text, uint32_t len, bool spell, const phon_opts_t *o) {

	ta_stop();

	txt = text;
	txt_len = len;
	txt_pos = 0;
	txt_spell = spell;
	opts = *o;

	synth_init();
	synth_set_volume(200);
	cpu_cycles = 0;
	behind_ms = 0;
	uint32_t c0 = z_cycles();
	gen_done = !next_chunk();
	cpu_cycles += z_cycles() - c0;
	if (gen_done) return;		// nothing audible: done at once

	wr = 0;
	laps = 0;
	lastpos = 0;
	cleared = 0;
	end_abs = 0;
	render_to(TA_PREFILL);

	if (mode == TA_FIFO) {
		rd = 0;
		underruns = 0;
		z_audio_start(up == 4 ? Z_AUDIO_RATE_44K : Z_AUDIO_RATE_11K);
		fifo_feed();
		playing = true;
		stuck_since = z_uptime_ticks();
		stuck_pos = 0;
		return;
	}

	uint32_t out_hz = z_audio_rate_hz();
	Z_AUDIO_CH_BASE(TA_CHANNEL) = phys_of(ring);
	Z_AUDIO_CH_LEN(TA_CHANNEL) = TA_RING;
	Z_AUDIO_CH_LOOPST(TA_CHANNEL) = 0;
	Z_AUDIO_CH_LOOPLEN(TA_CHANNEL) = TA_RING;
	Z_AUDIO_CH_STEP(TA_CHANNEL) = z_audio_step(SYNTH_FS * bps, out_hz);
	Z_AUDIO_CH_CTRL(TA_CHANNEL) = z_audio_ch_ctrl_fmt(gain, gain, true, true, 0, fmt16);
	start_ticks = z_uptime_ticks();

	// The DAC listens to the mixer and is running. Other channels are
	// not touched.
	uint32_t c = reg_audio_ctrl & 0xFFu;
	c |= Z_AUDIO_CTRL_MIXEN | Z_AUDIO_CTRL_EN;
	reg_audio_ctrl = c;

	playing = true;
	stuck_since = z_uptime_ticks();
	stuck_pos = 0;

}

void ta_pump(void) {

	if (!playing) return;

	uint32_t ra = read_abs();
	uint32_t now = z_uptime_ticks();

	if (mode == TA_FIFO) fifo_feed();

	// Somebody cleared our channel (a player starting up takes them
	// all), or the DAC is not consuming: the reader has stopped. Give
	// up rather than wait forever -- and SAY so, because this looks
	// exactly like working speech with the sound off.
	if (ra != stuck_pos) { stuck_pos = ra; stuck_since = now; }
	else if (now - stuck_since > Z_TICK_HZ / 2) {
		printf("tts: %s is not playing (stuck at %lu of %lu bytes) -- giving up on this utterance\n",
			mode == TA_FIFO ? "the audio FIFO" : "mixer channel 7",
			(unsigned long)ra, (unsigned long)wr);
		ta_stop();
		return;
	}

	// FIFO: done once everything is in it and it has nearly drained.
	if (gen_done && ra >= end_abs) {
		if (mode == TA_FIFO && z_audio_level() > 16) return;
		ta_stop();
		return;
	}

	// Up to the ring's limit, but at most a slice per call: rendering
	// the whole 1.5s at once would leave messages -- a stop, an
	// interrupt -- waiting half a second for their answer.
	// Behind the reader, the ring is cleared: if the writer ever falls
	// behind, the mixer -- which loops over the ring -- then plays
	// silence, not the last 1.5s over again. (A long passage in the
	// recorded voice, rendering too slowly, repeated whole phrases.)
	if (mode != TA_FIFO && ra > cleared) {
		uint32_t n = ra - cleared;
		if (n > TA_RING) { cleared = ra - TA_RING; n = TA_RING; }
		while (n--) ring[cleared++ % TA_RING] = 0;
	}

	// And if it has fallen behind, it skips ahead to where the reader is,
	// so what it writes next is heard; the log says by how much.
	if (mode != TA_FIFO && !gen_done && wr < ra + 64) {
		uint32_t skip = ra + 256 - wr;
		behind_ms += skip * 1000u / (SYNTH_FS * bps);
		wr = ra + 256;
		wr -= wr % bps;
	}

	uint32_t limit = ra + TA_RING - TA_GUARD;
	if (limit > wr + TA_SLICE) limit = wr + TA_SLICE;
	render_to(limit);

}

bool ta_busy(void) {
	return playing;
}
