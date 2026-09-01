/*
 * info -- system monitor
 *
 * What the machine is and what it is doing: the Zeitlos wordmark, the
 * OS version, what the gateware has in it, and live memory, load,
 * uptime and process figures.
 *
 *   > run wm
 *   > run info
 *
 * -- this app is a showcase, and it is also always open --
 *
 * Those two goals pull against each other. Something people leave
 * running must not cost anything to leave running, so:
 *
 *   - It samples once every SAMPLE_MS and redraws only what changed.
 *     A full repaint happens on Z_WM_REDRAW and nothing else.
 *   - Every figure it shows comes from syscalls that already existed
 *     and copy a fixed, small amount (z_mem_stats(), z_proc_list(),
 *     z_uptime_ticks()). Nothing is polled in a loop, nothing is
 *     computed by burning CPU to see how slow it feels.
 *   - Bars and the chart go through the blitter and line rasterizer.
 *     There is no per-pixel drawing anywhere in this file.
 *   - The logo is a 96x18 bitmap in .rodata, blitted in ONE hardware
 *     operation (z_fb_hw_blit_mem). The alternative -- reading the
 *     38400-byte boot splash off flash and downscaling it -- would
 *     cost more memory and more startup time for the same result.
 *
 * -- CPU --
 *
 * Real per-process accounting, not a proxy. The kernel charges each
 * KTIMER tick to whichever process it interrupted (z_proc.cpu_ticks,
 * sw/os/kernel.h), and z_proc_list() reports the running total. A
 * percentage is the DIFFERENCE between two samples over the elapsed
 * ticks between them.
 *
 * This app keeps the previous sample per pid and shows both a total
 * (everything except the idle-by-blocking remainder) and a per-process
 * figure in the list.
 *
 * Sampled at Z_TICK_HZ (~732Hz), so a process that starts and finishes
 * work between two ticks is invisible. Over the one-second interval
 * used here that is not a practical concern; over a much shorter one
 * it would be.
 *
 * This replaced counting runnable processes (Z_PROC_FLAG_BLOCKED),
 * which said how many things WANTED the CPU but nothing about who was
 * getting it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zproc.h"
#include "logo_small.h"

// -- window --
//
// Deliberately narrow. This is a status panel meant to sit alongside
// real work, not a document window, and everything in it is either a
// short label/value pair or a bar that looks fine at this width.
// Resizable upward for a longer process list.
#define WIN_W   180
#define WIN_H   340

static z_win_t win;

// -- sampling --
//
// SAMPLE_MS is how often the load counter is read; CHART_MS how often
// a column is committed and the text refreshed. The first is faster
// than the second on purpose: a single reading of "how many processes
// are runnable right now" is extremely noisy, and averaging four of
// them into each column is the difference between a chart and a
// picket fence.
#define SAMPLE_MS   250
#define CHART_MS   1000

// NO SD CARD FIGURE. It was here and it was removed, and the reason
// is not performance -- it is that the number could never be right.
//
// There is no card-detect line on this hardware (SPI only), and no
// mechanism anywhere to remount a volume: sdmm.c sets its status once
// in disk_initialize() and nothing ever sets it back, so FatFs never
// re-mounts either. A card is therefore mounted exactly once, at
// boot, forever. Any capacity figure this app showed would be a
// boot-time snapshot that could not be refreshed, could not notice a
// card being swapped, and would read "no card" permanently on a
// machine booted without one.
//
// A monitor showing a number it cannot keep true is worse than a
// monitor not showing it. Bring this back when there IS a remount
// path -- fs_df() itself is fine, and the shell's `df` still uses it.
//
// (fs_df() is also the only call this app ever made that could block
// on the card: f_getfree() walks the whole FAT the first time it is
// asked. FatFs caches the result afterwards and the volume stays
// mounted, so it was a one-off startup cost rather than a recurring
// one -- worth stating plainly, because it was briefly and wrongly
// blamed for a recurring stall.)

#define MS_TO_TICKS(ms)  (((uint32_t)(ms) * Z_TICK_HZ) / 1000u)

// Chart history, one entry per column, as a CPU percentage 0..100.
// A byte each: the value has one meaningful digit of precision at
// this sample rate, so anything wider would be storing noise.
#define CHART_MAX   200
static uint8_t chart[CHART_MAX];
static int chart_len;

// -- layout, recomputed on resize --

#define MARGIN      4
#define LINE_H      (z_font_5x8.h + 2)
#define BAR_H       7
#define CHART_H     34

static int y_logo, y_ver, y_feat, y_up, y_mem, y_chart, y_proc;
static int feat_lines;
static int proc_rows;
static int chart_w;

// -- last sampled state, so a redraw can tell what actually changed --

static z_mem_stats_args_t mem;
static uint32_t uptime_s;

#define PROC_MAX    16
static z_proc_info_t procs[PROC_MAX];
static uint32_t proc_count;

// Previous cpu_ticks per pid, for the difference that makes a
// percentage. Indexed by pid directly -- pids are small and bounded
// (Z_PROCS_MAX is 16), so a flat array is smaller and faster than any
// lookup, and a pid that goes away simply stops being read.
#define PID_MAX  16
static uint32_t prev_cpu[PID_MAX];

// Which pids we had a baseline for last time round. A process seen
// for the first time has no previous sample to subtract, and its
// lifetime total is whatever it has used since it started -- which,
// clamped to the interval, comes out as a confident 100%. Every
// freshly launched app would flash full-CPU for a second. Reporting
// nothing until there are two samples to compare is both honest and
// what it looks like anyway one interval later.
static uint32_t prev_seen;
static uint32_t prev_tick;

// Busy percentage over the last interval, 0..100, and the per-process
// share in the same units.
static int cpu_busy;
static uint8_t cpu_pct[PROC_MAX];

// ---------------------------------------------------------------
// small helpers -- no printf anywhere in the draw path
// ---------------------------------------------------------------
//
// printf() in an app that redraws once a second is not free: newlib's
// formatter is large and slow, and this app exists partly to
// demonstrate that a live display doesn't have to be expensive. These
// three cover everything it needs.

// Appends `v` in decimal. Returns the new length.
static int put_u32(char *buf, int n, int cap, uint32_t v) {

	char tmp[12];
	int t = 0;

	if (!v) tmp[t++] = '0';
	while (v && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + v % 10); v /= 10; }

	while (t && n < cap - 1) buf[n++] = tmp[--t];

	buf[n] = 0;
	return n;

}

static int put_str(char *buf, int n, int cap, const char *s) {

	for (; *s && n < cap - 1; s++) buf[n++] = *s;

	buf[n] = 0;
	return n;

}

// Appends `v` as a size with a unit -- bytes below 1K, then K, then M,
// with one decimal place above 1K so a bar and its label agree at a
// glance ("511.9K" rather than "511K" for two different values).
static int put_size(char *buf, int n, int cap, uint32_t bytes) {

	if (bytes < 1024) {
		n = put_u32(buf, n, cap, bytes);
		return put_str(buf, n, cap, "B");
	}

	if (bytes < 1024u * 1024u) {
		n = put_u32(buf, n, cap, bytes / 1024);
		n = put_str(buf, n, cap, ".");
		n = put_u32(buf, n, cap, (bytes % 1024) * 10 / 1024);
		return put_str(buf, n, cap, "K");
	}

	n = put_u32(buf, n, cap, bytes / (1024u * 1024u));
	n = put_str(buf, n, cap, ".");
	n = put_u32(buf, n, cap, (bytes % (1024u * 1024u)) * 10 / (1024u * 1024u));
	return put_str(buf, n, cap, "M");

}

// ---------------------------------------------------------------
// drawing primitives
// ---------------------------------------------------------------

// Content-relative fill, clamped to the content area.
// z_fb_hw_fill_rect() clamps to the SCREEN, not to this window, so a
// content-relative rect handed to it directly would paint over other
// windows the moment anything ran past our own edge.
static void fill(int cx, int cy, int w, int h, int color) {

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x0 = c.x0 + cx, y0 = c.y0 + cy;
	int x1 = x0 + w - 1, y1 = y0 + h - 1;

	if (x0 < c.x0) x0 = c.x0;
	if (y0 < c.y0) y0 = c.y0;
	if (x1 > c.x1) x1 = c.x1;
	if (y1 > c.y1) y1 = c.y1;
	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

static void text_at(int cx, int cy, const char *s) {
	z_win_draw_text(&win, cx, cy, s, 1, &z_font_5x8);
}

// Draws one line of live text, OPAQUELY, padded with spaces to a fixed
// column count.
//
// The obvious version -- clear the line, then draw the text -- is what
// this replaced, and it was the source of a visible flash every single
// second. Between the fill and the glyphs there is a moment where the
// line is blank, and at a 1Hz refresh the eye catches every one of
// them.
//
// z_fb_draw_text2() writes a SOLID cell per glyph: foreground where
// the glyph has a bit, background everywhere else. So each character
// overwrites its own patch in one operation and the line is never
// blank at any instant. Padding with spaces extends that to the tail,
// which is what the clear used to be for -- a space is a fully
// background-coloured cell, so it erases whatever was there.
//
// Costs nothing extra: an opaque glyph blit is the same single
// blitter operation a transparent one is.
static void line_at(int cx, int cy, const char *s) {

	int cw = z_win_content_w(&win);
	int cols = (cw - cx - MARGIN) / z_font_5x8.w;

	if (cols < 1) return;

	char padded[64];
	int n = 0;

	for (; s[n] && n < cols && n < (int)sizeof(padded) - 1; n++)
		padded[n] = s[n];
	for (; n < cols && n < (int)sizeof(padded) - 1; n++)
		padded[n] = ' ';

	padded[n] = 0;

	z_clip_t c;
	z_win_content_rect(&win, &c);

	z_fb_draw_text2(c.x0 + cx, c.y0 + cy, padded, 1, 0, &z_font_5x8, &c);

}

// A labelled proportion bar: frame, then a filled portion.
//
// Everything here is one engine (the blitter), frame included. The
// line rasterizer would be the natural way to draw the frame, but the
// two engines have no ordering between them and both read-modify-write
// whole VRAM words -- see field_draw() in sw/common/zdialog.c for the
// same reasoning at more length.
static void bar(int cx, int cy, int w, uint32_t used, uint32_t total) {

	if (w < 4) return;

	// frame, as four one-pixel fills
	fill(cx, cy, w, 1, 1);
	fill(cx, cy + BAR_H - 1, w, 1, 1);
	fill(cx, cy, 1, BAR_H, 1);
	fill(cx + w - 1, cy, 1, BAR_H, 1);

	int inner = w - 2;

	// 64-bit intermediate: total is a byte count that can be several
	// megabytes, and w is up to a few hundred, so the product
	// overflows 32 bits well before it becomes implausible.
	int fillw = total ? (int)(((uint64_t)used * inner) / total) : 0;
	if (fillw < 0) fillw = 0;
	if (fillw > inner) fillw = inner;

	// Filled part and empty part, each written once. NOT a clear
	// followed by a fill: that leaves the bar momentarily empty every
	// refresh, which is exactly the flash line_at() above also had to
	// get rid of.
	if (fillw) fill(cx + 1, cy + 1, fillw, BAR_H - 2, 1);
	if (fillw < inner)
		fill(cx + 1 + fillw, cy + 1, inner - fillw, BAR_H - 2, 0);

}

// ---------------------------------------------------------------
// layout
// ---------------------------------------------------------------

// Feature bits worth showing, in the order they read best. Only the
// ones actually present are drawn, so this is a menu rather than a
// list -- a board without ethernet simply doesn't show it.
typedef struct {
	uint32_t	bit;
	const char	*name;
} feat_t;

static const feat_t features[] = {
	{ Z_FEATURE_MEM_SDRAM,  "SDRAM"  },
	{ Z_FEATURE_MEM_SRAM,   "SRAM"   },
	{ Z_FEATURE_MEM_QQSPI,  "QQSPI"  },
	{ Z_FEATURE_MEM_VRAM,   "VRAM"   },
	{ Z_FEATURE_MEM_GLYPH,  "GLYPH"  },
	{ Z_FEATURE_GPU_RASTER, "RASTER" },
	{ Z_FEATURE_GPU_BLIT,   "BLIT"   },
	{ Z_FEATURE_GPU_CURSOR, "CURSOR" },
	{ Z_FEATURE_GPU_VGA,    "VGA"    },
	{ Z_FEATURE_GPU_DDMI,   "DDMI"   },
	{ Z_FEATURE_USB_HID,    "USBHID" },
	{ Z_FEATURE_SPI_SDCARD, "SDCARD" },
	{ Z_FEATURE_SPI_FLASH,  "FLASH"  },
	{ Z_FEATURE_ETH_RMII,   "ETH"    },
	{ Z_FEATURE_SPI_ETH,    "ETHSPI" },
	{ Z_FEATURE_UART0,      "UART"   },
};
#define FEAT_COUNT (int)(sizeof(features) / sizeof(features[0]))

// How many lines the feature tags wrap to at this width. Computed
// rather than assumed, since which features exist varies per board.
static int feature_line_count(int cw) {

	int avail = cw - 2 * MARGIN;
	int per = avail / z_font_5x8.w;
	if (per < 1) return 1;

	uint32_t f = reg_csr_features;
	int col = 0, lines = 1;

	for (int i = 0; i < FEAT_COUNT; i++) {
		if (!(f & features[i].bit)) continue;
		int need = (int)strlen(features[i].name) + 1;
		if (col && col + need > per) { lines++; col = 0; }
		col += need;
	}

	return lines;

}

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	feat_lines = feature_line_count(cw);

	y_logo  = MARGIN;
	y_ver   = y_logo + Z_LOGO_SMALL_H + 3;
	y_feat  = y_ver + LINE_H + 2;
	y_up    = y_feat + feat_lines * LINE_H + 2;
	y_mem   = y_up + LINE_H;
	y_chart = y_mem + LINE_H + BAR_H + 3;
	y_proc  = y_chart + CHART_H + 4;

	chart_w = cw - 2 * MARGIN;
	if (chart_w < 0) chart_w = 0;
	if (chart_w > CHART_MAX) chart_w = CHART_MAX;

	// Whatever is left goes to the process list, minus the grip's
	// corner so the window stays resizable (Z_WIN_GRIP_INSET, zwm.h).
	int avail = ch - Z_WIN_GRIP_INSET - y_proc - LINE_H;
	proc_rows = avail / LINE_H;
	if (proc_rows < 0) proc_rows = 0;
	if (proc_rows > PROC_MAX) proc_rows = PROC_MAX;

}

// ---------------------------------------------------------------
// sampling
// ---------------------------------------------------------------

// Refreshes the process list and computes each process's CPU share
// since the previous call, plus the total.
//
// Called once per chart interval, not four times a second: a
// percentage is meaningless without a matching elapsed-time baseline,
// and taking one over 250ms would be four times noisier for no more
// information.
static void sample_cpu(void) {

	uint32_t truncated = 0;
	proc_count = z_proc_list(procs, PROC_MAX, &truncated);

	uint32_t now = z_uptime_ticks();
	uint32_t elapsed = now - prev_tick;
	prev_tick = now;

	// First call, or a suspiciously long gap -- record the baseline
	// and report nothing rather than a number derived from an
	// interval we don't trust.
	if (!elapsed) {
		for (uint32_t i = 0; i < proc_count; i++)
			if (procs[i].pid < PID_MAX) {
				prev_cpu[procs[i].pid] = procs[i].cpu_ticks;
				prev_seen |= (1u << procs[i].pid);
			}
		return;
	}

	uint32_t busy = 0;
	uint32_t seen = 0;

	for (uint32_t i = 0; i < proc_count; i++) {

		uint32_t pid = procs[i].pid;
		if (pid >= PID_MAX) { cpu_pct[i] = 0; continue; }

		// Unsigned subtraction, so this stays right across the 32-bit
		// wrap of cpu_ticks (~68 days of solid CPU at 732Hz).
		uint32_t d = procs[i].cpu_ticks - prev_cpu[pid];
		prev_cpu[pid] = procs[i].cpu_ticks;

		seen |= (1u << pid);

		// No baseline last time -- see prev_seen. Record one and
		// report nothing for this interval.
		if (!(prev_seen & (1u << pid))) { cpu_pct[i] = 0; continue; }

		// Still clamp: a pid can be REUSED by a different process
		// between two samples, in which case the difference is
		// against a stranger's counter and can be anything.
		if (d > elapsed) d = elapsed;

		busy += d;

		cpu_pct[i] = (uint8_t)((d * 100) / elapsed);

	}

	prev_seen = seen;

	if (busy > elapsed) busy = elapsed;

	cpu_busy = (int)((busy * 100) / elapsed);

}

// The per-second figures. All cheap: z_mem_stats() copies a fixed
// struct and z_uptime_ticks() reads a counter. Nothing in this app's
// periodic path touches the filesystem at all -- see the note at the
// top on why the SD figure is gone.
static void sample_slow(void) {

	z_mem_stats(&mem);

	uptime_s = z_uptime_ticks() / Z_TICK_HZ;

}

// ---------------------------------------------------------------
// painting
// ---------------------------------------------------------------

static void draw_logo(void) {

	int cw = z_win_content_w(&win);

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int x = c.x0 + (cw - Z_LOGO_SMALL_W) / 2;
	int y = c.y0 + y_logo;

	// One hardware operation for the whole wordmark. The bitmap is
	// already in framebuffer bit order (see logo_small.h), which is
	// what makes that possible -- there is no conversion step.
	//
	// Falls back to nothing on a bitstream whose blitter predates
	// memory-source blits: a missing logo is a cosmetic loss, and the
	// software alternative is a 1728-pixel loop this app has no
	// business running.
	if (!z_fb_hw_blit_mem(z_logo_small, Z_LOGO_SMALL_STRIDE,
		0, 0, x, y, Z_LOGO_SMALL_W, Z_LOGO_SMALL_H))
		fill(0, y_logo, cw, Z_LOGO_SMALL_H, 0);

}

static void draw_static(void) {

	int cw = z_win_content_w(&win);
	char buf[64];
	int n;

	draw_logo();

	// version, centred under the wordmark
	n = put_str(buf, 0, sizeof(buf), "Zeitlos ");
	n = put_str(buf, n, sizeof(buf), Z_OS_VERSION);

	int tw = n * z_font_5x8.w;
	fill(0, y_ver, cw, z_font_5x8.h, 0);
	text_at((cw - tw) / 2, y_ver, buf);

	// feature tags, wrapped
	fill(0, y_feat, cw, feat_lines * LINE_H, 0);

	uint32_t f = reg_csr_features;
	int per = (cw - 2 * MARGIN) / z_font_5x8.w;
	int col = 0, line = 0;

	buf[0] = 0;
	n = 0;

	for (int i = 0; i < FEAT_COUNT; i++) {

		if (!(f & features[i].bit)) continue;

		int need = (int)strlen(features[i].name) + 1;

		if (col && col + need > per) {
			text_at(MARGIN, y_feat + line * LINE_H, buf);
			line++;
			col = 0;
			n = 0;
			buf[0] = 0;
		}

		if (n) n = put_str(buf, n, sizeof(buf), " ");
		n = put_str(buf, n, sizeof(buf), features[i].name);
		col += need;

	}

	if (n) text_at(MARGIN, y_feat + line * LINE_H, buf);

}

static void draw_uptime(void) {

	char buf[48];
	int n;

	uint32_t s = uptime_s;
	uint32_t d = s / 86400; s %= 86400;
	uint32_t h = s / 3600;  s %= 3600;
	uint32_t m = s / 60;    s %= 60;

	n = put_str(buf, 0, sizeof(buf), "up ");

	if (d) { n = put_u32(buf, n, sizeof(buf), d); n = put_str(buf, n, sizeof(buf), "d "); }

	n = put_u32(buf, n, sizeof(buf), h);
	n = put_str(buf, n, sizeof(buf), h < 10 ? "h0" : "h");
	n = put_u32(buf, n, sizeof(buf), m);
	n = put_str(buf, n, sizeof(buf), m < 10 ? "m0" : "m");
	n = put_u32(buf, n, sizeof(buf), s);
	n = put_str(buf, n, sizeof(buf), "s");

	line_at(MARGIN, y_up, buf);

}

static void draw_mem(void) {

	int cw = z_win_content_w(&win);
	char buf[64];
	int n;

	n = put_str(buf, 0, sizeof(buf), "mem ");
	n = put_size(buf, n, sizeof(buf), mem.used);
	n = put_str(buf, n, sizeof(buf), " / ");
	n = put_size(buf, n, sizeof(buf), mem.total);

	line_at(MARGIN, y_mem, buf);
	bar(MARGIN, y_mem + LINE_H, cw - 2 * MARGIN, mem.used, mem.total);

}

// The load chart: a framed strip with one vertical bar per column.
//
// Bars rather than a polyline. A line joining adjacent samples implies
// the value moved smoothly between them, which for a count of runnable
// processes it did not -- it is a sequence of discrete observations,
// and drawing it as one is both more honest and cheaper.
static void draw_chart_full(void) {

	int cw = z_win_content_w(&win);
	int w = cw - 2 * MARGIN;

	// frame, blitter only -- same single-engine reasoning as bar()
	fill(MARGIN, y_chart, w, 1, 1);
	fill(MARGIN, y_chart + CHART_H - 1, w, 1, 1);
	fill(MARGIN, y_chart, 1, CHART_H, 1);
	fill(MARGIN + w - 1, y_chart, 1, CHART_H, 1);

	int inner_h = CHART_H - 2;
	if (inner_h < 1) return;

	// Fixed full scale of 100%.
	//
	// An autoscaling chart was here first and it was actively
	// misleading: an idle machine's noise got stretched to full
	// height, so the display looked alarming when nothing was
	// happening, and two screenshots taken a minute apart could not
	// be compared. A percentage has a natural ceiling; use it.
	const int peak = 100;

	int first = chart_len > chart_w ? chart_len - chart_w : 0;

	for (int i = first; i < chart_len; i++) {

		int col = MARGIN + 1 + (i - first);
		if (col >= MARGIN + w - 1) break;

		int hgt = (chart[i] * inner_h) / peak;
		if (hgt > inner_h) hgt = inner_h;
		if (!hgt && chart[i]) hgt = 1;	// never lose a nonzero sample

		if (hgt < inner_h)
			fill(col, y_chart + 1, 1, inner_h - hgt, 0);
		if (hgt)
			fill(col, y_chart + CHART_H - 1 - hgt, 1, hgt, 1);

	}

}

// Adds the newest column without redrawing the whole strip.
//
// The interior is scrolled one pixel left with a VRAM-to-VRAM blit --
// one hardware operation for the entire chart -- and only the new
// rightmost column is drawn. Redrawing every column each second was
// the third source of the once-a-second flash, and by far the largest
// area of it.
//
// A left shift is safe with this blitter even though it overlaps:
// z_fb_hw_blit_vram() copies top-to-bottom, left-to-right, and the
// destination trails the source by one pixel in that order, so every
// word is read before it is overwritten. A right shift would not be.
//
// Falls back to a full redraw when the peak changes, because then
// every existing column's height is wrong and scrolling would carry
// the old scale along with it.
static void draw_chart_step(void) {

	int cw = z_win_content_w(&win);
	int w = cw - 2 * MARGIN;
	int inner_h = CHART_H - 2;

	if (w < 4 || inner_h < 1 || chart_len < 1) return;

	// Fixed scale (see draw_chart_full()), so the only reason to redraw
	// the whole strip instead of scrolling is not having filled it yet.
	const int peak = 100;

	if (chart_len <= chart_w) {
		draw_chart_full();
		return;
	}

	z_clip_t c;
	z_win_content_rect(&win, &c);

	int ix = c.x0 + MARGIN + 1;			// interior, absolute
	int iy = c.y0 + y_chart + 1;
	int iw = w - 2;

	if (iw < 2) return;

	z_fb_hw_blit_vram(ix + 1, iy, ix, iy, iw - 1, inner_h);

	// the newest sample, in the column just vacated at the right
	int hgt = (chart[chart_len - 1] * inner_h) / peak;
	if (hgt > inner_h) hgt = inner_h;
	if (!hgt && chart[chart_len - 1]) hgt = 1;

	int col = MARGIN + 1 + iw - 1;

	if (hgt < inner_h)
		fill(col, y_chart + 1, 1, inner_h - hgt, 0);
	if (hgt)
		fill(col, y_chart + CHART_H - 1 - hgt, 1, hgt, 1);

}

static void draw_procs(void) {

	int cw = z_win_content_w(&win);
	char buf[64];
	int n;

	n = put_str(buf, 0, sizeof(buf), "cpu ");
	n = put_u32(buf, n, sizeof(buf), (uint32_t)cpu_busy);
	n = put_str(buf, n, sizeof(buf), "%  procs ");
	n = put_u32(buf, n, sizeof(buf), proc_count);

	line_at(MARGIN, y_proc, buf);

	for (int r = 0; r < proc_rows; r++) {

		int y = y_proc + LINE_H + r * LINE_H;

		fill(MARGIN, y, cw - 2 * MARGIN, z_font_5x8.h, 0);

		if (r >= (int)proc_count) continue;

		z_proc_info_t *p = &procs[r];

		n = put_u32(buf, 0, sizeof(buf), p->pid);
		while (n < 3) n = put_str(buf, n, sizeof(buf), " ");
		n = put_str(buf, n, sizeof(buf), " ");

		// A process that never registered a name shows its pid only
		// -- see z_proc_info_t in zproc.h. "-" rather than blank, so
		// the column still reads as a column.
		n = put_str(buf, n, sizeof(buf), p->name[0] ? p->name : "-");

		// pad to a fixed column so the numbers line up
		while (n < 13) n = put_str(buf, n, sizeof(buf), " ");

		// CPU share over the last interval, right-aligned in 3.
		{
			char pct[8];
			int pn = put_u32(pct, 0, sizeof(pct), cpu_pct[r]);
			for (int k = pn; k < 3; k++) n = put_str(buf, n, sizeof(buf), " ");
			n = put_str(buf, n, sizeof(buf), pct);
			n = put_str(buf, n, sizeof(buf), "% ");
		}

		n = put_size(buf, n, sizeof(buf), p->size);

		text_at(MARGIN, y, buf);

	}

}

static void repaint(void) {

	// wm clears before most redraws but NOT after a move
	// (repair_drag() excludes the window's own final footprint), so
	// anything not actively rewritten keeps its pre-move contents.
	z_win_clear(&win);

	draw_static();
	draw_uptime();
	draw_mem();
	draw_chart_full();
	draw_procs();

}

// ---------------------------------------------------------------

static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		// The part of this window not covered by the windows in front
		// of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:
			z_win_apply_resized(&win, &msg->obj);
			layout();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("info: starting\n");

	// CLOSE_ICON without CLOSE_KILLS_OWNER would need a Z_WM_CLOSE
	// handler; this app owns exactly one window for its whole
	// lifetime and has nothing to save, so the killing form is
	// correct and simpler. See that flag's comment in zwm.h.
	if (z_win_create_flags(&win, "info", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER |
		Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_MIN_IS_CREATE) != Z_OK) {
		printf("info: failed to create window -- is wm running?\n");
		return 1;
	}

	// Claimed and discarded: this app takes no argument, but leaving
	// one pending would hand it to whatever the user opens next
	// (Z_WM_SET_ARG, zwm.h).
	{
		char ignored[8];
		z_launch_arg_take(ignored, sizeof(ignored));
	}

	layout();

	sample_slow();
	prev_tick = z_uptime_ticks();
	sample_cpu();			// baseline; reports nothing yet
	repaint();

	uint32_t last_chart = z_uptime_ticks();

	for (;;) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) forward_msg(&msg, NULL);

		uint32_t now = z_uptime_ticks();

		if (now - last_chart >= MS_TO_TICKS(CHART_MS)) {

			last_chart = now;

			// Percentages are a difference over an interval, so the
			// sample and the chart column are the same event now --
			// there is nothing useful to average four times a second.
			sample_cpu();

			int tenths = cpu_busy;
			if (tenths > 255) tenths = 255;

			if (chart_len < CHART_MAX) {
				chart[chart_len++] = (uint8_t)tenths;
			} else {
				// Scroll by one. memmove of at most 200 bytes once a
				// second is not worth a ring buffer's extra index
				// arithmetic in every reader.
				memmove(chart, chart + 1, CHART_MAX - 1);
				chart[CHART_MAX - 1] = (uint8_t)tenths;
			}

			sample_slow();

			// Only the live parts. The logo, version and feature list
			// never change, so they are drawn once per repaint() and
			// left alone -- which is most of the pixels in this
			// window.
			draw_uptime();
			draw_mem();
			draw_chart_step();
			draw_procs();

		}

		// Blocks until a message arrives or the next sample is due,
		// instead of spinning. That matters more here than anywhere
		// else in this tree: a status display that busy-waits would
		// distort the very load figure it is reporting, and would
		// show up in its own chart.
		z_proc_wait(MS_TO_TICKS(SAMPLE_MS) / 2);

	}

	return 0;

}
