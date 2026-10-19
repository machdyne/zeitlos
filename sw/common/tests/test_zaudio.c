/*
 * Host test for sw/common/zaudio.h: exact sample rates (FSDIV), the
 * old dividers, and what z_audio_start*() writes.
 *
 *   make -C sw/common/tests -f Makefile.zaudio
 *
 * The audio block's registers are an anonymous page mapped at
 * 0x7000_0000, so zaudio.h is compiled exactly as an app sees it.
 * Plain memory has none of the hardware's side effects (RATE does not
 * rewrite FSDIV here), so the checks are on what software WRITES and
 * on how it decodes what it reads; the hardware half is
 * rtl/tb/tb_audio.v's section 11.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "zaudio.h"

#define MAP_BASE ((void *)0x70000000)
#define MAP_LEN  4096

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static volatile uint32_t *R(uint32_t off) {
	return (volatile uint32_t *)(uintptr_t)(0x70000000u + off);
}

/* a board: audio block present, CONFIG with or without FSDIV */
static void board(int exact, int spdif) {
	memset(MAP_BASE, 0, MAP_LEN);
	*R(0x000) = Z_CSR_MAGIC;
	*R(0x008) = Z_FEATURE_AUDIO;
	*R(0x500) = Z_AUDIO_MAGIC;
	*R(0x518) = (Z_AUDIO_CONFIG_SIG << 16)
		| (exact ? Z_AUDIO_CONFIG_HAS_EXACT : 0)
		| Z_AUDIO_CONFIG_HAS_VOLUME
		| (spdif ? Z_AUDIO_FORMAT_SPDIF : Z_AUDIO_FORMAT_SD)
		| 10;
	*R(0x51c) = 48000000u;
}

/* fs from an FSDIV word, in exact rational arithmetic */
static double fs_of(uint32_t fsdiv) {
	return 48.0e6 * (fsdiv & 0xFFFF) / (128.0 * (fsdiv >> 16));
}

int main(void) {
	void *m = mmap(MAP_BASE, MAP_LEN, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (m == MAP_FAILED) {
		printf("test_zaudio: skipped (cannot map 0x70000000)\n");
		return 77;
	}

	/* -- 1. the standard rates reduce to small exact FSDIVs -- */
	board(1, 0);
	{
		static const struct { uint32_t hz, step, mod; } t[] = {
			{ 48000, 16, 125 }, { 44100, 147, 1250 },
			{ 32000, 32, 375 }, { 22050, 147, 2500 },
			{ 11025, 147, 5000 }, { 96000, 32, 125 },
			{ 88200, 147, 625 }, { 24000, 8, 125 },
			{ 16000, 16, 375 }, { 8000, 8, 375 },
		};
		unsigned i;
		for (i = 0; i < sizeof t / sizeof t[0]; i++) {
			uint32_t v = z_audio_fsdiv_for(t[i].hz);
			CHECK((v & 0xFFFF) == t[i].step && (v >> 16) == t[i].mod,
				"%u Hz -> step %u mod %u, want %u/%u", t[i].hz,
				v & 0xFFFF, v >> 16, t[i].step, t[i].mod);
		}
	}

	/* -- 2. every rate the stage can make: within 3e-5, and exact
	 *    (and decoded back exactly) whenever MOD fit unscaled -- */
	{
		uint32_t hz, exact = 0, worst_hz = 0;
		double worst = 0;
		for (hz = 1000; hz <= 240000; hz++) {
			uint32_t v = z_audio_fsdiv_for(hz);
			double fs = fs_of(v), err = (fs - hz) / hz;
			if (err < 0) err = -err;
			if (err > worst) { worst = err; worst_hz = hz; }
			CHECK(v && (v >> 16) <= 0xFFFF && (v & 0xFFFF) < (v >> 16),
				"%u Hz: FSDIV %08x out of range", hz, v);
			if (fs == (double)hz) {
				exact++;
				*R(0x53c) = v;
				CHECK(z_audio_rate_hz() == hz, "%u Hz decodes as %u",
					hz, z_audio_rate_hz());
			}
		}
		CHECK(worst < 1e-5, "worst error %.2e at %u Hz", worst, worst_hz);
		printf("  rates 1000..240000: %u exact, worst error %.1e (%u Hz)\n",
			exact, worst, worst_hz);
		CHECK(z_audio_fsdiv_for(0) == 0 && z_audio_fsdiv_for(240001) == 0,
			"out-of-range rates refused");
	}

	/* -- 3. the old dividers -- */
	CHECK(z_audio_div_for(44100) == 17, "div 44100 = %u", z_audio_div_for(44100));
	CHECK(z_audio_div_for(48000) == 16, "div 48000 = %u", z_audio_div_for(48000));
	CHECK(z_audio_div_for(22050) == 34, "div 22050 = %u", z_audio_div_for(22050));
	CHECK(z_audio_div_for(11025) == 68, "div 11025 = %u", z_audio_div_for(11025));
	CHECK(z_audio_div_for(1) == 255 && z_audio_div_for(400000) == 2, "div clamps");

	/* -- 4. z_audio_start_hz() / z_audio_start() with exact rates -- */
	board(1, 1);
	z_audio_start_hz(48000);
	CHECK(*R(0x510) == 16, "RATE shadow for 48000 = %u", *R(0x510));
	CHECK(*R(0x53c) == ((125u << 16) | 16), "FSDIV for 48000 = %08x", *R(0x53c));
	CHECK(*R(0x504) == (Z_AUDIO_CTRL_EN | Z_AUDIO_CTRL_CLRUR), "CTRL = %x", *R(0x504));
	CHECK(z_audio_rate_hz() == 48000, "rate_hz = %u", z_audio_rate_hz());
	CHECK(((*R(0x520) >> 8) & 0xF) == 2, "S/PDIF code for 48 kHz = %u",
		(*R(0x520) >> 8) & 0xF);
	z_audio_start(Z_AUDIO_RATE_44K);
	CHECK(*R(0x53c) == ((1250u << 16) | 147), "RATE_44K -> FSDIV %08x", *R(0x53c));
	CHECK(z_audio_rate_hz() == 44100, "RATE_44K -> %u Hz", z_audio_rate_hz());
	CHECK(((*R(0x520) >> 8) & 0xF) == 0, "S/PDIF code for 44.1 kHz");
	z_audio_start(Z_AUDIO_RATE_46K);
	CHECK(z_audio_rate_hz() == 48000, "RATE_46K -> %u Hz", z_audio_rate_hz());
	z_audio_start(Z_AUDIO_RATE_11K);
	CHECK(z_audio_rate_hz() == 11025, "RATE_11K -> %u Hz", z_audio_rate_hz());
	*R(0x520) = 0x80;			/* MIXVOL must survive the code store */
	z_audio_set_rate_hz(32000);
	CHECK((*R(0x520) & 0xFF) == 0x80 && ((*R(0x520) >> 8) & 0xF) == 3,
		"S/PDIF code 32 kHz without touching MIXVOL: %08x", *R(0x520));

	/* -- 5. ... and without them: the nearest divider, honestly reported -- */
	board(0, 1);
	z_audio_start_hz(48000);
	CHECK(*R(0x510) == 16 && *R(0x53c) == 0, "old bitstream: RATE 16, FSDIV untouched");
	CHECK(z_audio_rate_hz() == 46875, "old bitstream 48000 -> %u", z_audio_rate_hz());
	z_audio_start_hz(44100);
	CHECK(z_audio_rate_hz() == 44118, "old bitstream 44100 -> %u", z_audio_rate_hz());

	/* -- 6. what each board's outputs can carry -- */
	board(1, 1);
	CHECK(z_audio_rate_ok_hz(44100) && z_audio_rate_ok_hz(48000), "S/PDIF exact: 44.1/48 ok");
	CHECK(!z_audio_rate_ok_hz(22050) && !z_audio_rate_ok_hz(11025), "S/PDIF: below 32k refused");
	board(0, 1);
	CHECK(!z_audio_rate_ok_hz(44100), "old S/PDIF bitstream: odd divider 17 refused");
	CHECK(z_audio_rate_ok_hz(48000), "old S/PDIF bitstream: 16 ok");
	board(1, 0);
	CHECK(z_audio_rate_ok_hz(11025) && z_audio_rate_ok_hz(96000), "analogue: anything");
	CHECK(!z_audio_rate_ok_hz(500) && !z_audio_rate_ok_hz(240001), "analogue: stage limits");

	/* -- 7. preferred: the current rate if it fits, else the board's -- */
	board(1, 0);
	*R(0x53c) = z_audio_fsdiv_for(22050);
	CHECK(z_audio_preferred_hz() == 22050, "analogue keeps 22050");
	board(1, 1);
	*R(0x53c) = z_audio_fsdiv_for(22050);
	CHECK(z_audio_preferred_hz() == 48000, "S/PDIF at 22050 prefers 48000");
	*R(0x53c) = z_audio_fsdiv_for(44100);
	CHECK(z_audio_preferred_hz() == 44100, "S/PDIF keeps 44100");
	board(0, 1);
	*R(0x510) = 34;
	CHECK(z_audio_preferred_hz() == 48000, "old S/PDIF at 22k prefers 48000 (div 16)");

	/* -- 8. DDMI audio's clock regeneration -- */
	board(1, 0);
	{
		static const struct { uint32_t step, mod, k, n, cts; } t[] = {
			{ 16, 125, 1, 6144, 25200 },	/* 48 kHz */
			{ 147, 1250, 1, 6272, 28000 },	/* 44.1 kHz */
			{ 32, 375, 1, 4096, 25200 },	/* 32 kHz */
			{ 147, 2500, 2, 6272, 28000 },	/* 22.05 kHz x2 */
			{ 147, 5000, 4, 6272, 28000 },	/* 11.025 kHz x4 */
			{ 32, 125, 1, 12288, 25200 },	/* 96 kHz */
			{ 128, 1088, 1, 5680, 25347 },	/* old divider 17 */
			{ 128, 1024, 1, 6000, 25200 },	/* old divider 16 */
		};
		unsigned i;
		for (i = 0; i < sizeof t / sizeof t[0]; i++) {
			uint32_t n, cts;
			z_audio_ddmi_acr(t[i].step, t[i].mod, t[i].k, &n, &cts);
			CHECK(n == t[i].n && cts == t[i].cts, "ACR %u/%u x%u = %u/%u, want %u/%u",
				t[i].step, t[i].mod, t[i].k, n, cts, t[i].n, t[i].cts);
		}
	}
	/* every rate: the ratio is exact (N MOD 21 == CTS 40 k STEP, the
	 * definition), N is in the window sinks take, CTS fits 20 bits */
	{
		uint32_t hz, worst = 0, exact = 0;
		for (hz = 8000; hz <= 192000; hz += 7) {
			uint32_t v = z_audio_fsdiv_for(hz), step = v & 0xFFFF, mod = v >> 16;
			uint32_t k = 1, n, cts;
			uint64_t fs;
			while (k < 4 && (uint64_t)hz * k < 32000) k <<= 1;
			z_audio_ddmi_acr(step, mod, k, &n, &cts);
			fs = (uint64_t)375000 * step * k / mod;
			{
				/* exact where the reduced pair fits; otherwise within 5 ppm */
				uint64_t lhs = (uint64_t)n * mod * 21, rhs = (uint64_t)cts * 40 * k * step;
				double e = ((double)lhs - (double)rhs) / (double)rhs;
				if (e < 0) e = -e;
				if (lhs == rhs) exact++;
				CHECK(e < 5e-6, "%u Hz: N/CTS %u/%u off by %.2e", hz, n, cts, e);
			}
			CHECK(n >= fs * 128 / 1500 && n <= fs * 128 / 300 + 1 && cts < (1u << 20),
				"%u Hz: N %u / CTS %u out of range (fs %lu)", hz, n, cts, (unsigned long)fs);
			if (n > worst) worst = n;
		}
		printf("  DDMI ACR 8..192 kHz: %u exact of %u, largest N %u\n", exact, (192000 - 8000) / 7 + 1, worst);
	}
	/* z_audio_set_rate_hz() keeps socctl's DDMI registers in step */
	board(1, 0);
	*R(0x518) |= Z_AUDIO_FORMAT_DDMI;
	*R(0x204) = Z_SOCCTL_MAGIC;
	*R(0x208) = Z_VIDEO_SIG << 16;
	*R(0x228) = (Z_DDMI_SIG << 16) | Z_DDMI_AVAIL | Z_DDMI_SOUND_AVAIL | Z_DDMI_SOUND | Z_DDMI_EN;
	*R(0x53c) = z_audio_fsdiv_for(22050);   /* plain memory: set what FSDIV would read */
	z_audio_set_rate_hz(22050);
	*R(0x53c) = z_audio_fsdiv_for(22050);
	z_audio__ddmi_sync();
	CHECK(*R(0x22c) == 6272 && *R(0x230) == 28000, "22050: ACR %u/%u", *R(0x22c), *R(0x230));
	CHECK(((*R(0x228) >> 12) & 3) == 1 && ((*R(0x228) >> 4) & 15) == 0,
		"22050: REP 1, code 44.1k (%08x)", *R(0x228));
	CHECK((*R(0x228) & 3) == 3, "EN and SOUND kept (%08x)", *R(0x228));
	/* plain memory loses the read-only fields a write leaves alone in
	 * hardware: put them back */
	*R(0x228) |= (Z_DDMI_SIG << 16) | Z_DDMI_AVAIL | Z_DDMI_SOUND_AVAIL;
	*R(0x53c) = z_audio_fsdiv_for(48000);
	z_audio__ddmi_sync();
	*R(0x228) |= (Z_DDMI_SIG << 16) | Z_DDMI_AVAIL | Z_DDMI_SOUND_AVAIL;
	CHECK(*R(0x22c) == 6144 && *R(0x230) == 25200 && ((*R(0x228) >> 12) & 3) == 0
		&& ((*R(0x228) >> 4) & 15) == 2, "48000: ACR %u/%u (%08x)", *R(0x22c), *R(0x230), *R(0x228));
	z_ddmi_audio_set(false);
	CHECK(!(*R(0x228) & 1) && (*R(0x228) & 2) && ((*R(0x228) >> 4) & 15) == 2,
		"z_ddmi_audio_set(false) keeps the rest (%08x)", *R(0x228));

	/* -- 9. no audio block: nothing claims exact rates -- */
	memset(MAP_BASE, 0, MAP_LEN);
	CHECK(!z_audio_has_exact(), "no CSRs: no exact rates");

	printf("test_zaudio: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
