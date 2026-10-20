/*
 * Benchmark: the print service's work on the RISC-V CPU, run under
 * sim/zsim-headless, which counts retired instructions (docs/print.md,
 * "Speed"). The tour deck as a job: each sheet rendered at 300 dpi and
 * coded as PWG raster, and the whole job as PDF and as PostScript.
 *
 *   make -C sw/apps/slide job          (the job: /tmp/slide/deck.zpp)
 *   make -C sw/apps/print/bench
 *   sim/zsim-headless -m 33554432 -n 0 sw/apps/print/bench/bench_print.bin
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "../../../common/zeitlos.h"
#include "../../../common/zpaths.h"
#include "../../../common/zpool.h"
#include "../run.h"
#include "../res.h"
#include "../raster.h"

typedef struct { const char *path; const uint8_t *data; uint32_t len; } bench_file_t;
extern const bench_file_t bench_files[];

static uint8_t mem[20 << 20];
static uint8_t scratch[64 * 1024], cache[192 * 1024];

static inline uint32_t instret(void) {
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

static uint8_t *load(const char *path, uint32_t *len) {
	int i;
	for (i = 0; bench_files[i].path; i++) {
		if (!strcmp(bench_files[i].path, path)) {
			uint8_t *b = pool_alloc(bench_files[i].len);
			if (!b) return NULL;
			memcpy(b, bench_files[i].data, bench_files[i].len);
			*len = bench_files[i].len;
			return b;
		}
	}
	return NULL;
}

// a printer that takes everything at once
static bool sink(out_t *o, const void *d, uint32_t n) { (void)o; (void)d; (void)n; return true; }

static uint32_t sheet_start, last;
static int last_sheet = -1;
static bool tick(void *ctx, int sheet, int sheets, int y, int h) {
	(void)ctx; (void)y;
	if (y >= h) {
		uint32_t now = instret();
		printf("  sheet %d of %d: %lu insns\n", sheet + 1, sheets, (unsigned long)(now - sheet_start));
		sheet_start = now;
	}
	last_sheet = sheet;
	return true;
}

static bool nothing(void *ctx, const uint8_t *row, int y) { (void)ctx; (void)row; (void)y; return true; }

int main(void) {
	static uint32_t start[JOB_MAX_PAGES], bytes[JOB_MAX_PAGES];
	uint32_t len, a, b;
	uint8_t *data;
	job_t j;
	run_t r;
	out_t o = { sink, NULL, 0, false };

	pool_init(mem, sizeof(mem));
	zc_scratch(scratch, sizeof(scratch));
	zc_text_cache(cache, sizeof(cache));
	res_init(load, 4u << 20);
	data = load(Z_DIR_TMP "/job.zpp", &len);
	if (!data || !job_open(&j, data, len, start, bytes)) { printf("no job\n"); return 1; }
	printf("job: %d pages, %lu bytes\n", j.npages, (unsigned long)len);

	// rendering alone: the bands, no coding
	{
		static zc_pt_t pts[8192], wpts[16384];
		static uint16_t subs[1024], wsubs[2048];
		zc_path_t path, work;
		zp_play_t pl;
		raster_t ra;
		const zpage_size_t *paper;
		int wmm, hmm, s;
		zc_path_init(&path, pts, 8192, subs, 1024);
		zc_path_init(&work, wpts, 16384, wsubs, 2048);
		memset(&pl, 0, sizeof(pl));
		pl.font = res_font;
		pl.image = res_image;
		pl.fallback = res_fallback();
		pl.path = &path;
		pl.work = &work;
		memset(&ra, 0, sizeof(ra));
		ra.job = &j;
		ra.play = &pl;
		job_sheet_at(&j, 0, 300, NULL, NULL, &ra.sheet, &paper, &wmm, &hmm);
		ra.band = pool_alloc((size_t)((ra.sheet.w + 3) & ~3) * 64);
		ra.rows = 64;
		printf("rendering, Letter at 300 dpi, 64-line bands:\n");
		for (s = 0; s < job_sheets(&j); s++) {
			a = instret();
			raster_sheet(&ra, s, nothing, NULL);
			printf("  sheet %d: %lu insns\n", s + 1, (unsigned long)(instret() - a));
		}
		pool_free(ra.band);
	}

	memset(&r, 0, sizeof(r));
	r.tick = tick;
	printf("rendering and PWG raster coding (sgray_8):\n");
	a = sheet_start = instret();
	run_raster(&r, &j, &o, 300, PWG_SGRAY8, NULL);
	b = instret();
	printf("  all: %lu insns, %lu bytes\n", (unsigned long)(b - a), (unsigned long)r.bytes);

	o.total = 0;
	memset(&r, 0, sizeof(r));
	a = instret();
	run_vec(&r, &j, &o, VEC_PDF);
	printf("PDF: %lu insns, %lu bytes\n", (unsigned long)(instret() - a), (unsigned long)o.total);
	o.total = 0;
	memset(&r, 0, sizeof(r));
	a = instret();
	run_vec(&r, &j, &o, VEC_PS);
	printf("PostScript: %lu insns, %lu bytes\n", (unsigned long)(instret() - a), (unsigned long)o.total);
	return 0;
}
