/*
 * zvbench -- how fast this machine decodes ZV (docs/zv.md, "zvbench").
 *
 *   > run zvbench
 *
 * Reads /zvbench.zv (a clip made with tools/zv/zvenc) and decodes it
 * into each of the board's views in turn -- the window's (half size,
 * mono), the desktop's full screen (mono) and game mode's 256 greys --
 * into a picture in main memory, as the player will. For each it
 * prints the cycles a frame spent reading the card and decoding,
 * the worst frame, and what that leaves of a frame at 24 and 30 fps.
 *
 * Nothing is drawn: drawing is the blitter's (the desktop) or a page
 * flip (chunky in main memory), and costs the CPU next to nothing.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zcycles.h"
#include "../../common/zfsapp.h"
#include "../../common/zv.h"
#include "../../common/zsoc.h"
#include "../../common/zgfx.h"

#define PATH      "/zvbench.zv"
#define CHUNK     16384
#define CPU_HZ    48000000u

static uint8_t pic[ZV_MAX_W * ZV_MAX_H];
static uint32_t fc[4096];       // each frame's decode cycles

static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}
static zv_dec_t dec;
static uint8_t dmap[ZV_MAX_BW * ZV_MAX_BH];

// game mode on a 45F board: the blocks a frame changed into chunky VRAM,
// as cinema does it (the picture at stride 320). VRAM row r of segment
// s is word (r + 240 (s & 1) + 480 (s >> 1)) * 20: a block column's
// segment and word are worked out once.
static uint16_t segoff[ZV_MAX_BW];

static void copy_changed(const uint8_t *pic) {
	int bx, by, y;
	volatile uint32_t *vram = (volatile uint32_t *)0x20000000u;
	if (!segoff[1]) for (bx = 0; bx < ZV_MAX_BW; bx++) {
		int x = bx * 4, seg = x / 80;
		segoff[bx] = (uint16_t)((((seg & 1) ? 240 : 0) + ((seg & 2) ? 480 : 0)) * 20 + (x % 80) / 4);
	}
	for (by = 0; by < dec.bh; by++) {
		const uint8_t *m = dmap + by * dec.bw;
		volatile uint32_t *row = vram + by * 4 * 20;
		const uint32_t *src = (const uint32_t *)(const void *)(pic + by * 4 * 320);
		for (bx = 0; bx < dec.bw; bx++) {
			volatile uint32_t *d;
			const uint32_t *s;
			if (!m[bx]) continue;
			((uint8_t *)m)[bx] = 0;
			d = row + segoff[bx];
			s = src + bx;
			for (y = 0; y < 4; y++) d[y * 20] = s[y * 80];
		}
	}
}

// the same by the blitter: a span of changed blocks a segment, a block
// row at a time (cinema's blit_changed())
static void blit_changed(const uint8_t *pic) {
	int by, s2, bx;
	for (by = 0; by < dec.bh; by++) {
		uint8_t *m = dmap + by * dec.bw;
		for (s2 = 0; s2 < 4; s2++) {
			int a = -1, z = -1, b0 = s2 * 20, b1 = b0 + 19;
			if (b1 >= dec.bw) b1 = dec.bw - 1;
			for (bx = b0; bx <= b1; bx++) if (m[bx]) { if (a < 0) a = bx; z = bx; m[bx] = 0; }
			if (a < 0) continue;
			z_fb_hw_blit_mem_async(pic, 320, a * 32, by * 4, (a - b0) * 32,
				by * 4 + ((s2 & 1) ? 240 : 0) + ((s2 & 2) ? 480 : 0), (z - a + 1) * 32, 4, Z_ROP_COPY);
		}
	}
}

typedef struct {
	int fh;
	uint8_t *buf;
	uint32_t cap, len, pos;
	uint32_t read_cycles, read_bytes;
} rd_t;

// at least `need` bytes in the buffer from pos, reading as the player
// will, in big chunks
static int rd_fill(rd_t *r, uint32_t need) {
	while (r->len - r->pos < need) {
		uint32_t t;
		int got;
		if (r->pos) {
			memmove(r->buf, r->buf + r->pos, r->len - r->pos);
			r->len -= r->pos;
			r->pos = 0;
		}
		if (r->cap - r->len < CHUNK) return -1;
		t = z_cycles();
		got = fs_read_chunk(r->fh, r->buf + r->len, CHUNK);
		r->read_cycles += z_cycles() - t;
		if (got <= 0) return -1;
		r->len += (uint32_t)got;
		r->read_bytes += (uint32_t)got;
	}
	return 0;
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *view_name(int v) {
	switch (v) {
	case ZV_VIEW_MONO_HALF: return "window (half, mono)";
	case ZV_VIEW_MONO_FULL: return "full screen (mono)";
	case ZV_VIEW_PAL_HALF: return "game mode (greys; with the copy into chunky VRAM)";
	default: return "?";
	}
}

static void pct(const char *what, uint32_t cyc, uint32_t fps) {
	uint32_t budget = CPU_HZ / fps;
	printf("    %-7s %3lu%% of a %lu fps frame\n", what, (unsigned long)((uint64_t)cyc * 100 / budget), (unsigned long)fps);
}

int main(void) {
	static const int views[] = { ZV_VIEW_MONO_HALF, ZV_VIEW_MONO_FULL, ZV_VIEW_PAL_HALF, ZV_VIEW_PAL_HALF };
	zv_header_t h;
	rd_t r;
	uint8_t hb[ZV_HDR_SIZE];
	int vi, frames_max = 240;

	memset(&r, 0, sizeof(r));
	r.fh = fs_open_read(PATH);
	if (r.fh < 0) {
		printf("zvbench: no %s (make one with tools/zv/zvenc and copy it to the card)\n", PATH);
		return 1;
	}
	if (fs_read_chunk(r.fh, hb, ZV_HDR_SIZE) != ZV_HDR_SIZE || zv_parse_header(hb, ZV_HDR_SIZE, &h)) {
		printf("zvbench: %s is not a ZV file\n", PATH);
		return 1;
	}
	r.cap = h.max_rec + 2 * CHUNK;
	r.buf = malloc(r.cap);
	if (!r.buf) { printf("zvbench: no memory\n"); return 1; }
	printf("zvbench: %s: %ux%u, %lu/%lu fps, %lu frames\n", PATH, h.w, h.h,
		(unsigned long)h.fps_num, (unsigned long)h.fps_den, (unsigned long)h.frames);

	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	z_fb_set_rows(960);
	for (vi = 0; vi < 4; vi++) {
		int w, hh, st, f;
		uint32_t dec_total = 0, dec_max = 0, n = 0, worst_f = 0, max_later = 0, later_f = 0;
		zv_dec_init(&dec, &h, views[vi], ZV_MASK_VAC8, 0);
		zv_view_size(&dec, &w, &hh, &st);
		if (views[vi] == ZV_VIEW_PAL_HALF) { st = 320; dec.dirty = dmap; memset(dmap, 0, sizeof(dmap)); }
		memset(pic, 0, sizeof(pic));
		fs_seek(r.fh, ZV_HDR_SIZE);
		r.len = r.pos = 0;
		r.read_cycles = r.read_bytes = 0;
		for (f = 0; f < (int)h.frames && f < frames_max; f++) {
			zv_rec_t rec;
			uint32_t size, t;
			if (rd_fill(&r, ZV_REC_HDR)) break;
			size = rd32(r.buf + r.pos);
			if (rd_fill(&r, size)) break;
			if (zv_split_record(&h, r.buf + r.pos, size, &rec)) break;
			t = z_cycles();
			if (zv_dec_video(&dec, rec.video, rec.video_len, rec.flags & ZV_RF_KEY, pic, st) < 0) {
				printf("zvbench: bad frame %d\n", f);
				break;
			}
			if (dec.dirty) { if (vi == 3 && z_fb_hw_blit_mem_available()) blit_changed(pic); else copy_changed(pic); }
			t = z_cycles() - t;
			if (n < 4096) fc[n] = t;
			dec_total += t;
			if (t > dec_max) { dec_max = t; worst_f = (uint32_t)f; }
			if (f > 0 && t > max_later) { max_later = t; later_f = (uint32_t)f; }
			r.pos += size;
			n++;
		}
		if (!n) continue;
		{
			uint32_t avg = dec_total / n, rdf = r.read_cycles / n, m = n < 4096 ? n : 4096;
			qsort(fc, m, sizeof(fc[0]), cmp_u32);
			printf("  %s%s, %dx%d: %lu frames\n", view_name(views[vi]), vi == 3 ? " (by the blitter)" : "", w, hh, (unsigned long)n);
			printf("    decode  %lu cycles a frame: median %lu, 90%% %lu, worst %lu (frame %lu)\n",
				(unsigned long)avg, (unsigned long)fc[m / 2], (unsigned long)fc[m * 9 / 10],
				(unsigned long)dec_max, (unsigned long)worst_f);
			printf("    after the first frame: worst %lu (frame %lu)\n", (unsigned long)max_later, (unsigned long)later_f);
			printf("    read    %lu cycles a frame (%lu a byte, %lu bytes a frame)\n", (unsigned long)rdf,
				(unsigned long)(r.read_bytes ? r.read_cycles / r.read_bytes : 0), (unsigned long)(r.read_bytes / n));
			pct("both", avg + rdf, 24);
			pct("both", avg + rdf, 30);
		}
	}
	fs_close_handle(r.fh);
	return 0;
}
