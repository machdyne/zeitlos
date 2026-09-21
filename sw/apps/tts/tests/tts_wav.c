/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Renders phoneme strings to WAV files on the build machine, with the
 * REAL phon.c and synth.c -- the same code the service runs:
 *
 *     cd sw/apps/tts && make wav        # -> WAV files in /tmp/tts_wav
 *
 * Input: a test set, one utterance per line, "name|phonemes".
 * Lines starting with # are comments. Also reports the synthesiser's
 * internal headroom over everything rendered (SYNTH_STATS), and fails
 * if any 32-bit accumulator came within 2x of overflowing or any
 * output sample clipped -- so tuning the voice louder cannot quietly
 * make it wrap on the target.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../synth.h"
#include "../phon.h"
#include "../text2ph.h"
#include "../pack.h"

static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

static int16_t pcm[SYNTH_FS * 60];

// Renders `in` -- phonemes, or with `text` set, plain text through the
// same text2ph.c chunking the service uses.
// Phone boundaries, collected while rendering (ZTTS_MARKS).
static FILE *marks_out;
static int mark_tok = -2, mark_chunk = -1, chunk_no;
static char mark_name[8];
static uint32_t mark_start;

static void mark_close(uint32_t end) {
	if (!marks_out || mark_tok == -2) return;
	fprintf(marks_out, "%s %u %u\n", mark_name,
		(unsigned)(mark_start * 1000u / SYNTH_FS), (unsigned)(end * 1000u / SYNTH_FS));
}

static uint32_t render(const char *in, bool text, uint32_t rate) {
	static char ph[2048];
	uint32_t len = (uint32_t)strlen(in), pos = 0, n = 0;
	bool more = true;
	synth_init();
	while (more) {
		if (text) {
			if (!text2ph_chunk(in, len, &pos, false, ph, sizeof(ph), &more)) break;
		} else {
			snprintf(ph, sizeof(ph), "%s", in);
			more = false;
		}
		// ZTTS_VOICE=female, ZTTS_PITCH, ZTTS_FORMANTS, ZTTS_EXPRESSION:
		// the system.tts.* settings, for rendering comparisons.
		const char *ev = getenv("ZTTS_VOICE");
		bool female = ev && (ev[0] == 'f' || ev[0] == 'F');
		uint32_t pitch = getenv("ZTTS_PITCH") ? (uint32_t)atoi(getenv("ZTTS_PITCH")) : (female ? 200 : 110);
		uint32_t fmt = getenv("ZTTS_FORMANTS") ? (uint32_t)atoi(getenv("ZTTS_FORMANTS")) : (female ? 117 : 100);
		uint32_t expr = getenv("ZTTS_EXPRESSION") ? (uint32_t)atoi(getenv("ZTTS_EXPRESSION")) : 100;
		phon_opts_t o = { rate, pitch, more, fmt, expr + 1 };
		phon_begin(ph, &o);
		synth_frame_t fr;
		while (phon_next(&fr) && n + SYNTH_FRAME <= sizeof(pcm) / 2) {
			synth_render(&fr, &pcm[n], SYNTH_FRAME);
			if (marks_out) {
				const char *nm;
				int tok = phon_current(&nm);
				// a new phone starts where the token changes
				if (tok != mark_tok || chunk_no != mark_chunk) {
					mark_close(n);
					mark_tok = tok;
					mark_chunk = chunk_no;
					snprintf(mark_name, sizeof(mark_name), "%s", nm[0] ? nm : "_");
					mark_start = n;
				}
			}
			n += SYNTH_FRAME;
		}
		chunk_no++;
	}
	return n;
}

static void write_wav(const char *path, uint32_t n) {
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }
	fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2);
	fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 1);
	put32(f, SYNTH_FS); put32(f, SYNTH_FS * 2); put16(f, 2); put16(f, 16);
	fwrite("data", 1, 4, f); put32(f, n * 2);
	for (uint32_t i = 0; i < n; i++) put16(f, (uint16_t)pcm[i]);
	fclose(f);
}

int main(int argc, char **argv) {

	if (argc < 3) {
		fprintf(stderr, "usage: tts_wav testset outdir [wpm] [pack]\n");
		return 2;
	}

	// With ZTTS_MARKS set, a .phones file is written beside each wav:
	// one line per phone, "name start_ms end_ms". tools/speech uses it
	// to align this voice against a real recording of the same words
	// (the speech-data pipeline docs, published with it) -- the reference has known boundaries, the
	// recording does not, and warping one onto the other moves them
	// across.
	const char *marks = getenv("ZTTS_MARKS");
	uint32_t rate = argc > 3 ? (uint32_t)atoi(argv[3]) : 180;

	// With a speech pack, the front end has 391,159 pronunciations and
	// the trained letter-to-sound rules behind it; without one it has
	// the built-in dictionary and the hand-written rules. Rendering
	// the same text both ways is how to hear what the pack is worth.
	if (argc > 4 && argv[4][0]) {
		if (pack_open(argv[4]))
			printf("tts_wav: using the lexicon in %s\n", argv[4]);
		else
			printf("tts_wav: no usable pack at %s -- built-ins only\n", argv[4]);
	}

	// ZTTS_OPEN, ZTTS_TILT: the voice source's quality, over whatever
	// a pack set -- for listening, and for tools/speech's source fit.
	if (getenv("ZTTS_OPEN")) phon_prosody_set(PHON_PRO_OPEN, atoi(getenv("ZTTS_OPEN")));
	if (getenv("ZTTS_TILT")) phon_prosody_set(PHON_PRO_TILT, atoi(getenv("ZTTS_TILT")));

	FILE *in = fopen(argv[1], "r");
	if (!in) { perror(argv[1]); return 1; }

	char line[1024];
	int count = 0;
	double secs = 0;
	while (fgets(line, sizeof(line), in)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		char *bar = strchr(line, '|');
		if (!bar) continue;
		*bar = 0;
		char *ph = bar + 1;
		char *bar2 = strchr(ph, '|');
		if (bar2) *bar2 = 0;
		// "t_" names are plain text; the rest are phonemes.
		char path[1200];
		if (marks) {
			snprintf(path, sizeof(path), "%s/%s.phones", argv[2], line);
			marks_out = fopen(path, "w");
			mark_tok = -2;
			mark_chunk = -1;
			chunk_no = 0;
		}
		uint32_t n = render(ph, strncmp(line, "t_", 2) == 0, rate);
		if (marks_out) {
			mark_close(n);
			fclose(marks_out);
			marks_out = 0;
		}
		snprintf(path, sizeof(path), "%s/%s.wav", argv[2], line);
		write_wav(path, n);
		secs += (double)n / SYNTH_FS;
		count++;
	}
	fclose(in);

	printf("tts_wav: %d files, %.1f s of speech at %u wpm\n", count, secs, rate);
	printf("tts_wav: peak inside a resonator %ld (limit 2147483647), peak output %ld, %lu samples clipped\n",
		(long)synth_peak_internal, (long)synth_peak_out, (unsigned long)synth_clipped);

	if (synth_peak_internal > 0x3fffffff) { printf("tts_wav: FAIL -- less than 2x headroom\n"); return 1; }
	if (synth_clipped) { printf("tts_wav: FAIL -- output clipped\n"); return 1; }
	return 0;

}
