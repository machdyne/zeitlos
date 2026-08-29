/*
 * modplay_test -- host-side tests for the MOD engine
 *
 *   cd sw/apps/mod && make test
 *
 * Builds with the HOST compiler, not the RISC-V one, and links nothing
 * from Zeitlos. That is the point: a mixer bug found here is printed
 * as a number on a terminal, and the same bug found on hardware is
 * "it sounds a bit wrong", which is a far more expensive thing to
 * chase. Same reasoning as sw/apps/calc/calc_test.c.
 *
 * Modules are SYNTHESISED here rather than shipped, because a real
 * .mod is a copyrighted artifact and a fixture whose correct output
 * nobody can state is not a test. Every module below is built by
 * make_mod() with known samples and known notes, so the expected
 * result is arithmetic rather than opinion.
 *
 * Pass -w to dump each test's output as a WAV for listening; the
 * tests themselves do not need it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "modplay.h"

static int failures = 0;
static int write_wav = 0;

static void ok(const char *what, int cond) {
	printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
	if (!cond) failures++;
}

/* ------------------------------------------------------------------
 * module builder
 * ------------------------------------------------------------------ */

#define PAT_ROWS 64

typedef struct {
	uint8_t *buf;
	size_t len;
	size_t cap;
	int channels;
	int patterns;
} modbuild_t;

static void mb_init(modbuild_t *mb, int channels, int patterns) {
	mb->channels = channels;
	mb->patterns = patterns;
	mb->cap = 1084 + (size_t)patterns * PAT_ROWS * channels * 4 + 512 * 1024;
	mb->buf = calloc(1, mb->cap);
	mb->len = 1084 + (size_t)patterns * PAT_ROWS * channels * 4;

	memcpy(mb->buf, "testmod             ", 20);

	if (channels == 4) memcpy(mb->buf + 1080, "M.K.", 4);
	else { mb->buf[1080] = (uint8_t)('0' + channels);
	       memcpy(mb->buf + 1081, "CHN", 3); }

	mb->buf[950] = (uint8_t)patterns;   /* song length */
	mb->buf[951] = 127;
	/* order table: pattern n at position n */
	for (int i = 0; i < patterns; i++) mb->buf[952 + i] = (uint8_t)i;
}

static void mb_sample(modbuild_t *mb, int idx, const int8_t *data,
	int nbytes, int volume, int loop_start, int loop_len)
{
	uint8_t *h = mb->buf + 20 + (idx - 1) * 30;
	int words = nbytes / 2;
	h[22] = (uint8_t)(words >> 8);  h[23] = (uint8_t)(words & 0xFF);
	h[24] = 0;                       /* finetune 0 */
	h[25] = (uint8_t)volume;
	h[26] = (uint8_t)((loop_start / 2) >> 8);
	h[27] = (uint8_t)((loop_start / 2) & 0xFF);
	h[28] = (uint8_t)((loop_len / 2) >> 8);
	h[29] = (uint8_t)((loop_len / 2) & 0xFF);

	memcpy(mb->buf + mb->len, data, nbytes);
	mb->len += nbytes;
}

static void mb_note(modbuild_t *mb, int pattern, int row, int chan,
	int sample, int period, int effect, int param)
{
	uint8_t *n = mb->buf + 1084
		+ (size_t)pattern * PAT_ROWS * mb->channels * 4
		+ (size_t)row * mb->channels * 4
		+ (size_t)chan * 4;
	n[0] = (uint8_t)((sample & 0xF0) | ((period >> 8) & 0x0F));
	n[1] = (uint8_t)(period & 0xFF);
	n[2] = (uint8_t)(((sample & 0x0F) << 4) | (effect & 0x0F));
	n[3] = (uint8_t)param;
}

static void mb_free(modbuild_t *mb) { free(mb->buf); }

/* ------------------------------------------------------------------ */

static void dump_wav(const char *name, const int16_t *pcm, int frames,
	int rate)
{
	if (!write_wav) return;
	char path[256];
	snprintf(path, sizeof(path), "%s.wav", name);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint32_t datalen = (uint32_t)frames * 4;
	uint32_t riff = 36 + datalen;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	uint32_t fmtlen = 16; uint16_t pcm16 = 1, chans = 2, bits = 16;
	uint32_t srate = (uint32_t)rate, brate = (uint32_t)rate * 4;
	uint16_t align = 4;
	fwrite(&fmtlen,4,1,f); fwrite(&pcm16,2,1,f); fwrite(&chans,2,1,f);
	fwrite(&srate,4,1,f);  fwrite(&brate,4,1,f); fwrite(&align,2,1,f);
	fwrite(&bits,2,1,f);
	fwrite("data",1,4,f);  fwrite(&datalen,4,1,f);
	fwrite(pcm, 1, datalen, f);
	fclose(f);
	printf("  (wrote %s)\n", path);
}

static int peak_of(const int16_t *p, int n) {
	int pk = 0;
	for (int i = 0; i < n; i++) {
		int v = p[i] < 0 ? -p[i] : p[i];
		if (v > pk) pk = v;
	}
	return pk;
}

/* Count zero crossings on the left channel and turn that into a
 * frequency. A square wave sample played at a known period should come
 * back at a known pitch, and this is how that gets checked without a
 * Fourier transform. */
static double freq_of(const int16_t *pcm, int frames, int rate) {
	int crossings = 0;
	int prev = pcm[0] >= 0;
	for (int i = 1; i < frames; i++) {
		int cur = pcm[i * 2] >= 0;
		if (cur != prev) crossings++;
		prev = cur;
	}
	return (double)crossings * rate / (2.0 * frames);
}

/* ------------------------------------------------------------------
 * tests
 * ------------------------------------------------------------------ */

/* One cycle of a square wave, 32 bytes. Played at period p the
 * resulting tone is (3546895 / p) / 32 Hz. */
static int8_t square32[32];

static void make_square(void) {
	for (int i = 0; i < 32; i++) square32[i] = (i < 16) ? 100 : -100;
}

static void test_parse(void) {
	modbuild_t mb;
	mod_player_t m;
	int rc;

	printf("\n=== parse ===\n");

	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	rc = modplay_init(&m, mb.buf, mb.len, 22050);
	ok("M.K. accepted", rc == MOD_OK);
	ok("4 channels", m.channels == 4);
	ok("song name read", strncmp(m.name, "testmod", 7) == 0);
	ok("sample 1 length", m.samples[1].length == 32);
	ok("sample 1 volume", m.samples[1].volume == 64);
	ok("sample 1 loops", m.samples[1].loop_len == 32);
	mb_free(&mb);

	/* not a module */
	{
		uint8_t junk[2048];
		memset(junk, 0xAB, sizeof(junk));
		rc = modplay_init(&m, junk, sizeof(junk), 22050);
		ok("garbage rejected", rc == MOD_ERR_NOT_MOD);
	}

	/* too small */
	{
		uint8_t tiny[100];
		memset(tiny, 0, sizeof(tiny));
		rc = modplay_init(&m, tiny, sizeof(tiny), 22050);
		ok("short file rejected", rc == MOD_ERR_TOO_SMALL);
	}

	/* 8 channels */
	mb_init(&mb, 8, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	rc = modplay_init(&m, mb.buf, mb.len, 22050);
	ok("8CHN accepted", rc == MOD_OK && m.channels == 8);
	ok("8ch master scaled down", m.master == 96);
	mb_free(&mb);

	/* more channels than we support */
	mb_init(&mb, 4, 1);
	mb.buf[1080] = '9'; memcpy(mb.buf + 1081, "CHN", 3);
	rc = modplay_init(&m, mb.buf, mb.len, 22050);
	ok("9CHN rejected", rc == MOD_ERR_CHANNELS);
	mb_free(&mb);

	/* truncated: header claims patterns that are not there */
	mb_init(&mb, 4, 4);
	rc = modplay_init(&m, mb.buf, 1200, 22050);
	ok("truncated file rejected", rc == MOD_ERR_TRUNCATED);
	mb_free(&mb);
}

static void test_pitch(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 44100;
	int frames = rate / 2;
	int16_t *pcm = malloc((size_t)frames * 4);

	printf("\n=== pitch ===\n");
	make_square();

	/* C-2 is period 428 -> 3546895/428 = 8287 Hz playback rate,
	 * / 32 samples per cycle = 258.9 Hz. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 0);
	modplay_render(&m, pcm, frames);

	{
		double f = freq_of(pcm, frames, rate);
		double want = (3546895.0 / 428.0) / 32.0;
		printf("  period 428 -> %.1f Hz (want %.1f)\n", f, want);
		ok("C-2 within 1%", f > want * 0.99 && f < want * 1.01);
		dump_wav("test_pitch_c2", pcm, frames, rate);
	}
	mb_free(&mb);

	/* An octave up is period 214, and should be exactly double. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 214, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, frames);
	{
		double f = freq_of(pcm, frames, rate);
		double want = (3546895.0 / 214.0) / 32.0;
		printf("  period 214 -> %.1f Hz (want %.1f)\n", f, want);
		ok("C-3 is an octave up", f > want * 0.99 && f < want * 1.01);
	}
	mb_free(&mb);

	free(pcm);
}

static void test_volume_and_pan(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 22050;
	int frames = 4096;
	int16_t *pcm = malloc((size_t)frames * 4);

	printf("\n=== volume and panning ===\n");

	/* Full volume on one channel of a 4-channel module. Sample peaks
	 * at 100, volume 64, master 192, pan 256 (near side is never
	 * attenuated -- see modplay_set_separation):
	 *   100 * 64 = 6400
	 *   6400 * 256 >> 8 = 6400
	 *   6400 * 192 >> 8 = 4800
	 */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 0);
	modplay_render(&m, pcm, frames);
	{
		int pk = peak_of(pcm, frames * 2);
		printf("  peak %d (want 4800)\n", pk);
		ok("mix scaling is exact", pk == 4800);
	}
	mb_free(&mb);

	/* Cxx at half volume should halve it. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xC, 32);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 0);
	modplay_render(&m, pcm, frames);
	{
		int pk = peak_of(pcm, frames * 2);
		printf("  peak at Cxx=32: %d (want 2400)\n", pk);
		ok("Cxx set volume", pk == 2400);
	}
	mb_free(&mb);

	/* Hard panning: channel 0 is a LEFT channel in Amiga LRRL order,
	 * so the right output must be silent. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 100);
	modplay_render(&m, pcm, frames);
	{
		int lpk = 0, rpk = 0;
		for (int i = 0; i < frames; i++) {
			int l = pcm[i*2] < 0 ? -pcm[i*2] : pcm[i*2];
			int r = pcm[i*2+1] < 0 ? -pcm[i*2+1] : pcm[i*2+1];
			if (l > lpk) lpk = l;
			if (r > rpk) rpk = r;
		}
		printf("  hard pan L=%d R=%d\n", lpk, rpk);
		ok("channel 0 is hard left", lpk > 0 && rpk == 0);
	}

	/* Channel 1 is a RIGHT channel. */
	memset(pcm, 0, (size_t)frames * 4);
	mb_note(&mb, 0, 0, 0, 0, 0, 0, 0);
	mb_note(&mb, 0, 0, 1, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 100);
	modplay_render(&m, pcm, frames);
	{
		int lpk = 0, rpk = 0;
		for (int i = 0; i < frames; i++) {
			int l = pcm[i*2] < 0 ? -pcm[i*2] : pcm[i*2];
			int r = pcm[i*2+1] < 0 ? -pcm[i*2+1] : pcm[i*2+1];
			if (l > lpk) lpk = l;
			if (r > rpk) rpk = r;
		}
		printf("  hard pan L=%d R=%d\n", lpk, rpk);
		ok("channel 1 is hard right", rpk > 0 && lpk == 0);
	}
	mb_free(&mb);

	free(pcm);
}

static void test_looping(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 22050;
	/* two seconds -- far longer than the 32-byte sample lasts */
	int frames = rate * 2;
	int16_t *pcm = malloc((size_t)frames * 4);

	printf("\n=== looping ===\n");

	/* Looped: must still be making sound at the end. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, frames);
	ok("looped sample still sounding after 2s",
		peak_of(pcm + (frames - 1000) * 2, 2000) > 0);
	mb_free(&mb);

	/* One-shot: must have gone silent. */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 0);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, frames);
	ok("one-shot sample stops", peak_of(pcm + (frames - 1000) * 2, 2000) == 0);
	ok("...but did play at the start", peak_of(pcm, 200) > 0);
	mb_free(&mb);

	free(pcm);
}

static void test_timing(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 22050;

	printf("\n=== timing ===\n");

	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);

	/* default 125 BPM -> a tick is 20ms -> 441 frames at 22050 */
	printf("  samples_per_tick %u (want 441)\n", m.samples_per_tick);
	ok("default tempo 125 BPM", m.samples_per_tick == 441);

	/* A row at speed 6 is 6 ticks. 64 rows is 384 ticks. Render
	 * exactly one pattern's worth and check we land on row 0 of the
	 * next order position. */
	{
		int frames = 441 * 6 * 64;
		int16_t *pcm = malloc((size_t)frames * 4);
		modplay_render(&m, pcm, frames);
		printf("  after one pattern: order %d row %d\n", m.order_pos, m.row);
		ok("one pattern is 64 rows", m.row == 0);
		free(pcm);
	}
	mb_free(&mb);

	/* Fxx tempo change */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xF, 250);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	{
		int16_t pcm[64];
		modplay_render(&m, pcm, 32);
		printf("  after F250: bpm %d, spt %u\n", m.bpm, m.samples_per_tick);
		ok("Fxx >= 32 sets BPM", m.bpm == 250);
		ok("BPM 250 halves the tick", m.samples_per_tick == 220);
	}
	mb_free(&mb);

	/* Fxx speed change */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xF, 3);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	{
		int16_t pcm[64];
		modplay_render(&m, pcm, 32);
		ok("Fxx < 32 sets speed", m.speed == 3);
	}
	mb_free(&mb);
}

static void test_effects(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 22050;
	int16_t *pcm = malloc((size_t)rate * 4 * 2);

	printf("\n=== effects ===\n");

	/* Dxx pattern break: row 0 of pattern 0 breaks to row 32 of the
	 * next pattern. */
	mb_init(&mb, 4, 2);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xD, 0x32);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 441 * 6 + 16);
	printf("  after D32: order %d row %d\n", m.order_pos, m.row);
	ok("Dxx breaks to the right row", m.order_pos == 1 && m.row == 32);
	mb_free(&mb);

	/* Bxx position jump */
	mb_init(&mb, 4, 3);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xB, 2);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 441 * 6 + 16);
	printf("  after B02: order %d row %d\n", m.order_pos, m.row);
	ok("Bxx jumps position", m.order_pos == 2 && m.row == 0);
	mb_free(&mb);

	/* Axx volume slide down: volume 64, slide 4 per tick for 5 ticks
	 * after tick 0 -> 64 - 20 = 44 */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xA, 0x04);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 441 * 6 - 1);
	printf("  after A04 for one row: volume %d (want 44)\n", m.ch[0].volume);
	ok("Axy volume slide down", m.ch[0].volume == 44);
	mb_free(&mb);

	/* 1xx portamento up lowers the period */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0x1, 4);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 441 * 6 - 1);
	printf("  after 104: period %d (want 408)\n", m.ch[0].period);
	ok("1xx portamento up", m.ch[0].period == 408);
	mb_free(&mb);

	/* 3xx tone portamento walks toward the target and stops there */
	mb_init(&mb, 4, 2);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0, 0);
	mb_note(&mb, 0, 1, 0, 0, 214, 0x3, 0xFF);   /* huge speed: snaps */
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 441 * 6 * 2 + 16);
	printf("  after 3FF: period %d (want 214)\n", m.ch[0].period);
	ok("3xx reaches the target and clamps", m.ch[0].period == 214);
	mb_free(&mb);

	/* 9xx sample offset past the end silences the channel */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0x9, 0x10);   /* offset 4096 >> 32 */
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 2048);
	ok("9xx past the end plays silence", peak_of(pcm, 4096) == 0);
	mb_free(&mb);

	/* EEx pattern delay stretches the row without retriggering */
	mb_init(&mb, 4, 1);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	mb_note(&mb, 0, 0, 0, 1, 428, 0xE, 0xE1);   /* delay 1 -> row is 2x */
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_render(&m, pcm, 16);
	printf("  EE1: row_ticks %d (want 12)\n", m.row_ticks);
	ok("EEx doubles the row length", m.row_ticks == 12);
	mb_free(&mb);

	free(pcm);
}

static void test_no_overflow(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 44100;
	int frames = rate;
	int16_t *pcm = malloc((size_t)frames * 4);
	int c;

	printf("\n=== headroom ===\n");

	/* Every channel at full volume, full-scale sample. This is the
	 * worst case the mix scaling is designed for; it must not clip. */
	mb_init(&mb, 4, 1);
	{
		int8_t loud[32];
		for (int i = 0; i < 32; i++) loud[i] = (i < 16) ? 127 : -128;
		mb_sample(&mb, 1, loud, 32, 64, 0, 32);
	}
	for (c = 0; c < 4; c++) mb_note(&mb, 0, 0, c, 1, 428, 0, 0);
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);
	modplay_set_separation(&m, 0);
	modplay_render(&m, pcm, frames);
	{
		int pk = peak_of(pcm, frames * 2);
		printf("  4ch full scale peak %d (of 32767)\n", pk);
		ok("does not clip", pk < 32767);
		ok("uses most of the range", pk > 20000);
		dump_wav("test_headroom", pcm, frames, rate);
	}
	mb_free(&mb);

	free(pcm);
}

static void test_long_run(void) {
	modbuild_t mb;
	mod_player_t m;
	int rate = 22050;
	int chunk = 64;
	int16_t pcm[128];
	long i;

	printf("\n=== long run ===\n");

	/* Sixty seconds through a two-pattern module in 64-frame chunks,
	 * which is exactly how the app feeds the FIFO. Catches anything
	 * that only breaks at a chunk boundary or after a song wrap --
	 * neither of which the short tests above would ever reach. */
	mb_init(&mb, 4, 2);
	mb_sample(&mb, 1, square32, 32, 64, 0, 32);
	for (int r = 0; r < 64; r += 4) {
		mb_note(&mb, 0, r, 0, 1, 428 - r, 0, 0);
		mb_note(&mb, 0, r, 1, 1, 214 + r, 0xA, 0x02);
		mb_note(&mb, 1, r, 2, 1, 300 + r, 0x4, 0x47);
		mb_note(&mb, 1, r, 3, 1, 500 - r, 0x1, 0x03);
	}
	modplay_init(&m, mb.buf, mb.len, (uint32_t)rate);

	for (i = 0; i < (long)rate * 60 / chunk; i++)
		modplay_render(&m, pcm, chunk);

	printf("  60s rendered, %u song loops, order %d row %d\n",
		m.loops, m.order_pos, m.row);
	ok("survived 60s of chunked rendering", m.loops > 0);
	ok("order position still in range",
		m.order_pos >= 0 && m.order_pos < m.song_len);
	ok("row still in range", m.row >= 0 && m.row < 64);
	mb_free(&mb);
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "-w") == 0) write_wav = 1;

	make_square();

	printf("\nmodplay host tests\n");

	test_parse();
	test_pitch();
	test_volume_and_pan();
	test_looping();
	test_timing();
	test_effects();
	test_no_overflow();
	test_long_run();

	printf("\n");
	if (failures == 0) printf("=== modplay: PASS ===\n\n");
	else printf("=== modplay: %d FAILURE(S) ===\n\n", failures);

	return failures ? 1 : 0;
}
