/*
 * track -- ProTracker MOD player
 *
 *   > run wm
 *   > run track
 *
 * PHASE 2 of the audio subsystem, and the reference workload the whole
 * thing was designed around: a MOD exercises multi-channel playback,
 * per-channel pitch, volume and looping samples all at once, which is
 * why it is the test app rather than a sine wave.
 *
 * Runs windowed under the wm when there is one and falls back to the
 * console when there is not (see main()). Both paths share the same
 * feed loop, so the fallback is a real fallback rather than a second
 * implementation that can rot.
 *
 * -- WHY THE MODULE LIVES IN .bss AND NOT IN malloc() --
 *
 * Read this before "fixing" the buffer below into an allocation.
 *
 * A process's stack tier (Z_PROC_STACK_SIZE_DEFAULT, 16KB for any app
 * not named in z_proc_stack_size_for(), sw/os/kernel.h) is the ONLY
 * room its C stack and its malloc() heap ever get, shared, for its
 * entire life. It is not a stack allowance with a heap somewhere else.
 *
 * So the first version of this app, which did
 *
 *     data = fs_mallocfile(path);
 *
 * could never have worked for any real module. An 87KB MOD wants 87KB
 * of heap out of a 16KB allowance. Worse, the failure is a NULL return
 * that is indistinguishable from "file unreadable", which is exactly
 * what it unhelpfully reported on hardware.
 *
 * Static footprint is a different budget entirely: code, .rodata and
 * .bss are part of the BINARY, and k_proc_create() sizes the process
 * block as image + stack tier. A .bss array is therefore memory the
 * kernel reserves up front, at process creation -- and it either fits
 * or the process never starts, which is a much better failure than
 * starting and then dying on the first allocation.
 *
 * This is not a workaround, it is what the tree already does. `repl`
 * carries its whole 96KB Scheme cell heap as a .bss array inside ms.o
 * for precisely this reason, and kernel.h's tier comment calls that
 * out.
 *
 * A happy consequence: mod needs no entry in z_proc_stack_size_for().
 * Nothing here allocates, so the default 16KB tier is ample and the
 * kernel needs no change to run this.
 *
 * -- controls --
 *
 *   n         next module
 *   space     pause
 *   [ / ]     stereo separation
 *   q / ESC   quit (or the window's close icon)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"
#include "../../common/zfsapp.h"
#include "../../common/zkbd.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"

#include "modplay.h"

/*
 * THE MIXER IS A BUS MASTER AND ISSUES PHYSICAL ADDRESSES.
 *
 * This process sees its own memory through the MTU, which remaps
 * 0x8000_0000 to wherever the kernel actually put it (see
 * docs/app_runtime.md). So `modbuf` is a VIRTUAL address and handing
 * it to the mixer unchanged would point the sample fetches at whatever
 * physically lives at that offset -- which on this SOC is the BIOS and
 * the kernel. It would play, and it would play the wrong memory.
 *
 * reg_mtu_base (rtl/mtu.v, readable from an app because only
 * 0x8xxx_xxxx is translated, so a load from 0x9000_0000 reaches the
 * MTU itself) holds the translation base. Physical = virtual -
 * 0x8000_0000 + base.
 *
 * It reads as 0 where no translation is active, in which case virtual
 * and physical are already the same and the arithmetic below is a
 * no-op -- so this is correct in both contexts without a special case.
 *
 * The same class of mistake as the k_proc_create() bug documented in
 * docs/app_runtime.md: a 0x8000_0000-relative address used somewhere
 * that does not go through the MTU.
 */
#define Z_APP_VIRT_BASE 0x80000000u

static uint32_t phys_of(const void *p) {
	return reg_mtu_base + ((uint32_t)p - Z_APP_VIRT_BASE);
}

/*
 * Largest module this build can play, in bytes.
 *
 * Sized for a 1MB board, which is the binding case: Obst is `MEM 1`,
 * and with wm and a shell resident there is roughly 450KB free. This
 * buffer plus the app's own code and libc leaves comfortable room in
 * that, and most ProTracker modules are well under it.
 *
 * Raise it on a 32MB board; it is a plain define and nothing else
 * depends on the value:
 *
 *     make -C sw/apps/mod MOD_MAX_FILE=$((1024*1024))
 *
 * A module bigger than this is REFUSED, with both numbers, rather than
 * truncated. Truncation is not a quiet degradation here: pattern data
 * comes first in the file and sample data last, so a truncated module
 * plays all the right notes with missing instruments, which sounds
 * exactly like a mixer bug.
 */
#ifndef MOD_MAX_FILE
#define MOD_MAX_FILE (192 * 1024)
#endif

static uint8_t modbuf[MOD_MAX_FILE];

/*
 * Frames rendered per call to the engine.
 *
 * Sized against the hardware FIFO rather than picked at random. The
 * FIFO holds 128 frames, so a block of 64 is half of it and the push
 * loop can always drain a whole block into space it has just found.
 * A full FIFO's worth would leave the second half of every block with
 * nowhere to go; one frame at a time pays the call overhead 22050
 * times a second for nothing.
 */
#define BLOCK 64

static int16_t block[BLOCK * 2];
static int block_used = BLOCK;      /* == BLOCK means "exhausted" */

static mod_player_t player;

#define MAX_FILES 32
static char *files[MAX_FILES];
static int nfiles;
static int cur_file;

static z_win_t win;
static int windowed;

/*
 * Output volume, 0..100.
 *
 * NOT cosmetic. The 1-bit sigma-delta output drives headphones
 * directly with no amplifier and no attenuator between it and the
 * jack, so a full-scale module is very loud into a pair of 250-ohm
 * cans -- loud enough that the first thing anyone says about this
 * player is "how do I turn it down".
 *
 * The default is deliberately well below full scale for that reason.
 * Raise it with '=' if your board has an attenuator or you are driving
 * a line input.
 *
 * Applied in the hardware mixer's MIXVOL register, or in the software
 * engine's master scale -- one number, both back ends, so the two
 * sound the same at a given setting.
 */
#ifndef MOD_VOLUME_DEFAULT
#define MOD_VOLUME_DEFAULT 35
#endif

static int volume = MOD_VOLUME_DEFAULT;

static int separation = 65;
/* Set from the hardware at startup, NOT to a constant.
 *
 * boards.vh picks a power-on rate its outputs can actually carry --
 * Sergei sets 16 (46875Hz) because that is the only rate whose S/PDIF
 * half-cell is a whole number of sys_clk. This app used to overwrite
 * that with 22kHz at startup, which is below the 32kHz floor every
 * IEC 60958 receiver is specified to, so the digital output produced
 * garbage on a board that was configured correctly. */
static uint32_t rate_div;

/* Hardware mixing (rtl/audio_mixer.v) when the bitstream has it.
 *
 * This is the whole point of phase 3: with it set, the CPU's entire
 * job is writing channel registers on a tracker tick -- about fifty
 * times a second -- and audio no longer depends on how much CPU this
 * process gets. Software mixing remains as a fallback and is what
 * runs on a bitstream without the mixer. */
static int hw_mix;

/*
 * Tracker tick pacing for the hardware path, in 1/256 kernel ticks.
 *
 * The kernel tick is 732Hz and a tracker tick at 125 BPM and 22kHz is
 * 14.6347 of them. The first version of this rounded that to 14 with
 * an integer divide, which played every module 4.5% fast and made the
 * rate readout jitter between 90 and 117 -- the mean was right, which
 * is what gave it away.
 *
 * So the interval is carried in 8 fractional bits and the error never
 * accumulates. 732 * samples_per_tick * 256 is at most about 83
 * million at any rate this SOC produces, so this stays in 32 bits with
 * room to spare.
 */
static uint32_t hw_next_256;
static uint32_t hw_dt_256;

/* Largest number of tracker ticks feed_hw() has had to make up in one
 * call, since the last status update.
 *
 * A diagnostic, and the one that distinguishes the two remaining
 * explanations for a passage that sounds rushed. 1 means the pacing is
 * keeping up and whatever you are hearing is the MODULE -- most likely
 * a speed change or a pattern break, which the `spd` field will show.
 * Anything above 1 means the main loop stalled and the burst made the
 * time back, which is the player's fault and mine to fix. */
static int hw_burst;
static int hw_burst_max;

/*
 * Worst-case wall time spent in each phase of the main loop, in kernel
 * ticks (1.37ms each), reset every second alongside the rate readout.
 *
 * brst reached 7-8, which is 140-160ms of lateness and far more than
 * anything in this loop should cost -- a full-width blitter fill is
 * tens of microseconds. Rather than guess a fourth time at which phase
 * is responsible, these measure it: whichever of the four spikes when
 * the grid is showing blank rows is the one to fix.
 *
 * Shown as "fd/ms/pt/st" on the bottom line, replacing the key help.
 */
static uint32_t t_feed_max, t_msg_max, t_pat_max, t_sta_max;
static int paused;
static int want_next;
static int want_quit;

/* Wide enough for a row number plus four channel columns of
 * "C-2 01 A08" at 5x8, tall enough for the header, the status line and
 * PATTERN_ROWS of pattern. */
#define WIN_W 300
#define WIN_H 145

/* Rows of pattern shown, current row in the middle. Odd, so there is
 * an exact middle.
 *
 * SEVEN, NOT ELEVEN, AND THE LIMIT IS TIME.
 *
 * Rows are drawn one per main-loop iteration so that no single redraw
 * blocks the audio (see pat_pending). Measured on hardware a row costs
 * about 16ms of wall time, and a tracker row at the default speed
 * lasts 120ms -- so eleven rows need 180ms and can never finish before
 * the next row change restarts them. The redraw always got through the
 * first few slots and never reached the last ones, so the bottom three
 * or four lines simply stopped updating.
 *
 * Seven rows is 115ms, inside the budget with a little to spare. Raise
 * this only if you also make the rows cheaper -- and note that a
 * module running at a faster speed shortens the row period, which is
 * why the draw order below is centre-out rather than top-down. */
#define PATTERN_ROWS 7

/* Characters in a pattern line: "NN" plus four channels of
 * " C-2 01 A08". Padded to this width so the playing row's inverse bar
 * covers the full grid. */
#define PATTERN_COLS (2 + 4 * 11)

static int show_pattern = 1;
static int last_drawn_row = -1;
static int last_drawn_order = -1;

/* What each grid slot currently shows, so a slot that is already blank
 * is not filled again.
 *
 * Blank slots appear from row 59 through row 4 of the next pattern --
 * up to five of them, each a full-width blitter fill with its own
 * acquire and wait-idle. That is exactly the row range where the
 * playback speeds up, so they are the first thing to stop repeating.
 * Content rows still redraw every time: the grid scrolls, so every one
 * of them genuinely changed.
 *
 * -2 means "unknown", forcing a redraw. draw_all() resets to that. */
static int slot_row[PATTERN_ROWS];

/*
 * Incremental grid redraw: ONE ROW PER MAIN-LOOP ITERATION.
 *
 * draw_pattern() used to draw all eleven rows in one call. Measured on
 * hardware that is 131ms of wall time -- about 44ms of work, stretched
 * by sharing the CPU three ways -- and a tracker tick is 20ms. So
 * every grid redraw blocked feed_hw for six or seven ticks, which is
 * precisely the brst 7-8 that showed up as a speed-up at the end of
 * every pattern.
 *
 * Making the rows cheaper would only move the threshold. The rule has
 * to be structural: NO SINGLE DRAW MAY BLOCK THE AUDIO LOOP. One row
 * is about 12ms of wall time, comfortably inside a tick, and feed_hw
 * runs between every one of them.
 *
 * pat_row/pat_order snapshot the position the redraw is for, so a row
 * change mid-redraw simply restarts it against the new position rather
 * than producing a grid half from each.
 */
/*
 * Header redraw queue.
 *
 * draw_status() drew two lines of text plus the channel bars in one
 * call -- measured on hardware at 34ms (`st25`), longer than a 20ms
 * tracker tick, which is what was still producing bursts after the
 * pattern grid was made incremental. Fixing the grid and leaving this
 * alone just moved the stall.
 *
 * So the header is a dirty-bit queue drawn ONE ITEM PER ITERATION,
 * same rule as the grid: no single draw may block the audio loop.
 *
 * It is also far less work than it was, because most of it does not
 * change most of the time. The title never changes during a module;
 * the rate/volume/separation line changes only on a keypress or when
 * the once-a-second counters update. Only the position line and the
 * channel bars actually move.
 */
#define DIRTY_TITLE   (1u << 0)
#define DIRTY_POS     (1u << 1)
#define DIRTY_INFO    (1u << 2)
#define DIRTY_CHANS   (1u << 3)

static unsigned hdr_dirty;
static int last_pos_row = -1;

/* 's' toggles a once-a-second channel dump to the serial console. Off
 * by default: printf over the UART is slow enough to disturb the very
 * timing it would be reporting on. */
static int dump_stats;

static int pat_pending = PATTERN_ROWS;
static int pat_row;
static int pat_order;

/* Render-rate measurement -- see the note on draw_status(). */
static uint32_t rr_frames;
static uint32_t rr_start_tick;
static int rr_percent = -1;

/* ------------------------------------------------------------------
 * module discovery
 * ------------------------------------------------------------------ */

static int has_suffix(const char *s, const char *suf) {
	int ls = (int)strlen(s), lf = (int)strlen(suf), i;
	if (ls < lf) return 0;
	for (i = 0; i < lf; i++) {
		char a = s[ls - lf + i];
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (a != suf[i]) return 0;
	}
	return 1;
}

static void find_modules(void) {
	uint32_t count = 0;
	char **list;
	uint32_t i;

	nfiles = 0;
	list = fs_list("/", 0, &count);
	if (!list) return;

	for (i = 0; i < count; i++) {
		if (list[i] && nfiles < MAX_FILES && has_suffix(list[i], ".mod")) {
			files[nfiles++] = list[i];
			list[i] = 0;            /* ownership taken */
		}
	}
	for (i = 0; i < count; i++)
		if (list[i]) free(list[i]);
	free(list);
}

/*
 * Load a module into modbuf.
 *
 * Chunked through fs_read_chunk() rather than fs_mallocfile(): the
 * destination already exists and is already the right size, so there
 * is nothing to allocate and nothing that can fail for want of heap.
 * Returns bytes read, or 0.
 */
static uint32_t load_module(const char *path) {
	int size = fs_size((char *)path);
	int fh;
	uint32_t got = 0;

	if (size <= 0) {
		printf("track: %s: cannot stat\n", path);
		return 0;
	}

	if ((uint32_t)size > MOD_MAX_FILE) {
		printf("track: %s is %d bytes; this build caps modules at %lu.\n",
			path, size, (unsigned long)MOD_MAX_FILE);
		printf("     Rebuild with a larger MOD_MAX_FILE (see mod.c).\n");
		return 0;
	}

	fh = fs_open_read(path);
	if (fh < 0) {
		printf("track: %s: cannot open\n", path);
		return 0;
	}

	for (;;) {
		int n = fs_read_chunk(fh, modbuf + got, (int)(MOD_MAX_FILE - got));
		if (n <= 0) break;
		got += (uint32_t)n;
		if (got >= MOD_MAX_FILE) break;
	}

	fs_close_handle(fh);
	return got;
}

/* ------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------ */

#define ROW_H (z_font_5x8.h + 1)

static void draw_row_text(int y, const char *s, int inverse);

/* Column layout, in pixels from the content-area left edge. */
#define COL_ROWNUM 2
#define COL_CH0    22
#define COL_W      66

/* y of the first pattern row.
 *
 * FOUR rows of header, not three: filename, status, status2, channel
 * bars. It was three, which put draw_channels() directly on top of the
 * second row of the pattern grid -- two things drawing the same pixels
 * on different schedules, which is half of why the middle of the
 * window flickered. */
#define PATTERN_Y (3 + 4 * ROW_H)

static void draw_line(int row, const char *s) {
	draw_row_text(3 + row * ROW_H, s, 0);
}

/*
 * Draw one full-width line of text, background included, in one call.
 *
 * z_fb_draw_text2() paints the background of every glyph cell as it
 * goes, which is how sw/apps/read draws inline code spans in inverse
 * video. It matters here for two separate reasons.
 *
 * TEARING. The obvious way to mark the playing row -- fill a white
 * rectangle, then draw black text over it -- is two operations, and
 * the display scans out between them. What you see is a solid white
 * bar for one frame, eight times a second: a flashing, partially
 * inverted block in the middle of the window. One call that paints
 * both cannot tear against itself.
 *
 * COST. It also collapses a row from five snprintf calls and five
 * draw calls into one of each, which matters because this runs on
 * every row change and anything slow here shows up as a lurch in the
 * music (see feed_hw's catch-up clamp).
 *
 * There is no z_win_ wrapper for the two-colour form, so this does
 * what z_win_draw_text() does internally: fetch the content rect and
 * offset by it.
 */
static void draw_row_text(int y, const char *s, int inverse) {
	z_clip_t clip;
	int n = 0;
	int used;

	while (s[n]) n++;
	used = 2 + n * z_font_5x8.w;

	/* Clear only the tail, with ONE fill, rather than padding the
	 * string out to the full width with blank glyphs.
	 *
	 * Blank glyphs are not free -- each is a separate blitter
	 * operation with its own acquire/wait -- and padding every line to
	 * 46 characters roughly doubled the cost of a status redraw. That
	 * pushed the main loop past a 20ms tracker tick, which is what
	 * made feed_hw's pacing bug visible as "rate 50%". Text is drawn
	 * with its own background, so only what lies BEYOND it needs
	 * clearing, and the tail is empty either way so a fill there
	 * cannot tear against anything.
	 */
	z_win_fill_rect(&win, used, y, z_win_content_w(&win) - used, ROW_H,
		inverse ? 1 : 0);

	z_win_content_rect(&win, &clip);
	z_fb_draw_text2(clip.x0 + 2, clip.y0 + y, s,
		inverse ? 0 : 1, inverse ? 1 : 0, &z_font_5x8, &clip);
}

/*
 * One pattern row, as a single padded string:
 *
 *   "32 C-2 01 A08 --- .. ... C#3 04 000 --- .. ..."
 *
 * Padded to a fixed width so the inverse-video bar on the playing row
 * spans the whole grid rather than stopping after the last character.
 */
static void draw_pattern_row(int slot, int order_pos, int row, int highlight) {
	int y = PATTERN_Y + slot * ROW_H;

	/* Already blank and still blank: nothing to do. Only valid for
	 * blank slots -- a content row's text changes as the grid
	 * scrolls even when its slot index does not. */
	if (row < 0 && slot_row[slot] == -1) return;
	slot_row[slot] = row;
	char line[64];
	char note[4];
	int n;
	int c;

	if (row < 0) {
		/* Past the end of the pattern: blank, but still painted, so
		 * the previous contents cannot survive underneath. One fill,
		 * no glyphs. */
		z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
		return;
	}

	n = snprintf(line, sizeof(line), "%02d", row);

	for (c = 0; c < player.channels && c < 4; c++) {
		mod_cell_t cell;
		modplay_get_cell(&player, order_pos, row, c, &cell);
		modplay_note_name(cell.note, note);

		/* Empty fields print as dots rather than blanks: a tracker
		 * display is read by scanning columns, and a blank column is
		 * much harder to scan past than a dotted one. */
		if (cell.sample)
			n += snprintf(line + n, sizeof(line) - n, " %s %02X %X%02X",
				note, cell.sample, cell.effect, cell.param);
		else if (cell.effect || cell.param)
			n += snprintf(line + n, sizeof(line) - n, " %s .. %X%02X",
				note, cell.effect, cell.param);
		else
			n += snprintf(line + n, sizeof(line) - n, " %s .. ...", note);
	}

	line[n] = 0;

	draw_row_text(y, line, highlight);
}

/*
 * The pattern grid, redrawn only when the playing row changes.
 *
 * That condition is not a nicety. A row at the default speed lasts
 * 120ms, so this runs about eight times a second; redrawing it every
 * pass of the main loop instead would spend more CPU on glyphs than on
 * mixing, and on a board where the mixer is already the bottleneck
 * that turns a display into the cause of the underruns it displays.
 * 'v' turns the grid off entirely for exactly that reason.
 */
static void draw_pattern(void) {
	int k;

	if (!show_pattern) return;

	/* Snapshot and arm; the rows themselves are drawn one per
	 * iteration by pattern_step(). */
	pat_row = player.row;
	pat_order = player.order_pos;
	pat_pending = 0;

	(void)k;

	last_drawn_row = player.row;
	last_drawn_order = player.order_pos;
}

/* Draw at most one pending grid row. Called every main-loop iteration;
 * a no-op once the grid is up to date. */
static void pattern_step(void) {
	int half = PATTERN_ROWS / 2;
	int row;
	int i;

	if (!show_pattern || pat_pending >= PATTERN_ROWS) return;

	/* CENTRE-OUT draw order: the playing row first, then its
	 * neighbours working outwards.
	 *
	 * The order only matters when a redraw runs out of time, which it
	 * will on a module with a fast speed setting. Top-down means the
	 * rows nearest the cursor -- the ones actually being read -- are
	 * the last to update and the first to be dropped. Centre-out
	 * degrades the other way round: the outer rows lag, which nobody
	 * notices, and the playing row is always current.
	 *
	 *   step 0 1 2 3 4 5 6   ->   slot 3 2 4 1 5 0 6
	 */
	i = pat_pending++;
	if (i == 0) i = half;
	else if (i & 1) i = half - ((i + 1) >> 1);
	else i = half + (i >> 1);

	row = pat_row - half + i;

	draw_pattern_row(i, pat_order, (row >= 0 && row < 64) ? row : -1,
		i == half);
}

/*
 * Status line, including the number that actually matters when
 * something sounds wrong: `mix`, the percentage of real time the
 * mixer is achieving.
 *
 * The FIFO throttles rendering, so a healthy player sits at 100 --
 * it renders exactly as fast as the hardware consumes and no faster.
 * Anything below 100 means the CPU could not keep up, and the music
 * plays slow by exactly that factor. That is the difference between
 * "the buffer is too small" (occasional UNDERRUN, mix stays at 100)
 * and "the CPU is too slow" (mix sits below 100), which sound similar
 * and want completely different fixes.
 */
static void draw_pos_line(void) {
	char buf[64];
	snprintf(buf, sizeof(buf), "pos %02d/%02d  pat %02d  row %02d  spd %d/%d",
		player.order_pos, player.song_len,
		player.order[player.order_pos], player.row,
		player.speed, player.bpm);
	draw_line(1, buf);
}

static void draw_info_line(void) {
	char buf[64];
	uint32_t fmt = z_audio_formats();

	snprintf(buf, sizeof(buf), "%lu %dch vol%d sep%d %s%d%% brst%d %s",
		(unsigned long)z_audio_rate_hz(), player.channels,
		volume, separation, hw_mix ? "rate" : "mix", rr_percent,
		hw_burst_max,
		paused ? "PAUSED" : (z_audio_underrun() ? "UNDER" : ""));
	(void)fmt;
	draw_line(2, buf);
}

/* Draw at most one pending header item. A no-op once they are clean. */
/*
 * Per-channel activity bars.
 *
 * Drawn from the ENGINE's channel state, not by measuring the mixed
 * output. That is the honest thing to show: these bars are what the
 * player believes each channel is doing, so when they disagree with
 * what comes out of the jack they are evidence about where the fault
 * is rather than decoration.
 *
 * Cheap -- one fill per channel, no glyphs -- which is why it shares a
 * queue slot with the text lines without unbalancing it.
 */
static void draw_channels(int row) {
	int y = 3 + row * ROW_H;
	int c;
	int nch = player.channels ? player.channels : 1;
	int bw = (z_win_content_w(&win) - 8) / nch;

	for (c = 0; c < player.channels; c++) {
		int x = 4 + c * bw;
		int lvl = player.ch[c].active ? player.ch[c].volume : 0;
		int w = (lvl * (bw - 4)) / 64;
		z_win_fill_rect(&win, x, y, bw - 3, ROW_H, 0);
		if (w > 0) z_win_fill_rect(&win, x, y + 1, w, ROW_H - 2, 1);
	}
}

static void header_step(void) {
	if (hdr_dirty & DIRTY_POS) {
		hdr_dirty &= ~DIRTY_POS;
		draw_pos_line();
	} else if (hdr_dirty & DIRTY_CHANS) {
		hdr_dirty &= ~DIRTY_CHANS;
		draw_channels(3);
	} else if (hdr_dirty & DIRTY_INFO) {
		hdr_dirty &= ~DIRTY_INFO;
		draw_info_line();
	} else if (hdr_dirty & DIRTY_TITLE) {
		char buf[64];
		hdr_dirty &= ~DIRTY_TITLE;
		snprintf(buf, sizeof(buf), "%s  \"%s\"",
			files[cur_file] ? files[cur_file] : "", player.name);
		draw_line(0, buf);
	}
}

/*
 * Per-phase worst-case timings, in kernel ticks, on the bottom line.
 *
 * fd = feed_hw, ms = drain_messages, pt = draw_pattern, st =
 * draw_status. One tick is 1.37ms, and a tracker tick is about 15 of
 * them -- so any of these reaching double digits is the stall.
 */
static void draw_diag(void) {
	char buf[64];
	snprintf(buf, sizeof(buf), "fd%lu ms%lu pt%lu st%lu  n spc v r s -=",
		(unsigned long)t_feed_max, (unsigned long)t_msg_max,
		(unsigned long)t_pat_max, (unsigned long)t_sta_max);
	draw_row_text(z_win_content_h(&win) - ROW_H - 1, buf, 0);
}

static void draw_all(void) {
	int k;

	z_win_clear(&win);

	/* Everything is repainted, but ONE ITEM PER ITERATION by
	 * header_step() and pattern_step() -- a full repaint in one call
	 * is what used to stall the audio. */
	hdr_dirty = DIRTY_TITLE | DIRTY_POS | DIRTY_INFO | DIRTY_CHANS;

	for (k = 0; k < PATTERN_ROWS; k++) slot_row[k] = -2;
	last_drawn_row = -1;
	draw_pattern();

	if (!show_pattern) draw_line(4, "pattern view off (v)");

	draw_diag();
}

/* ------------------------------------------------------------------
 * input, shared by both modes
 * ------------------------------------------------------------------ */

static void apply_volume(void);

static void handle_key(uint32_t key) {
	switch (key) {
	case 'q':
	case 0x1b:                      /* Escape -- see zkbd.c */
		want_quit = 1;
		break;
	case 'n':
		want_next = 1;
		break;
	case ' ':
		paused = !paused;
		/* EN low mutes but keeps the DAC clocked (rtl/audio_out.v).
		 * Flushing as well means unpausing resumes from live audio
		 * rather than from a buffer that has been sitting there. */
		if (paused) {
			z_audio_stop();
		} else {
			/* z_audio_start() writes CTRL from scratch, which clears
			 * MIXEN -- so unpausing used to hand the DAC back to the
			 * (empty) FIFO and play silence. Re-enable it, and resync
			 * the tick clock so the pause does not count as lateness. */
			z_audio_start(rate_div);
			if (hw_mix) z_audio_mixer_enable(true);
			hw_next_256 = z_uptime_ticks() << 8;
		}
		break;
	case 's':
		dump_stats = !dump_stats;
		printf("\ntrack: stats %s\n", dump_stats ? "on" : "off");
		break;
	case 'v':
		show_pattern = !show_pattern;
		draw_all();
		break;
	case 'r': {
		uint32_t next;
		/* Cycle the sample rate live.
		 *
		 * Halving the rate halves the mixer's work, so this is the
		 * fastest way to find where this board stops keeping up:
		 * step down until `mix` reaches 100. The engine is told the
		 * new rate immediately, so pitch and tempo are unaffected --
		 * only the resampling quality changes.
		 */
		/* Skip anything this board's outputs cannot carry -- on a
		 * board with a transmitter that rules out 22k and 11k. */
		next = rate_div;
		do {
			if (next == Z_AUDIO_RATE_44K) next = Z_AUDIO_RATE_22K;
			else if (next == Z_AUDIO_RATE_22K) next = Z_AUDIO_RATE_11K;
			else next = Z_AUDIO_RATE_44K;
		} while (!z_audio_rate_ok(next) && next != rate_div);
		rate_div = next;
		reg_audio_rate = rate_div;
		player.rate = z_audio_rate_hz();
		modplay_retempo(&player);
		hw_next_256 = z_uptime_ticks() << 8;
		rr_frames = 0;
		rr_start_tick = z_uptime_ticks();
		if (windowed) draw_all();
		else printf("\ntrack: %lu Hz\n", (unsigned long)z_audio_rate_hz());
		break;
	}
	case '-':
	case '_':
		volume -= 5;
		apply_volume();
		if (windowed) hdr_dirty |= DIRTY_INFO;
		else printf("\ntrack: volume %d%%\n", volume);
		break;
	case '=':
	case '+':
		volume += 5;
		apply_volume();
		if (windowed) hdr_dirty |= DIRTY_INFO;
		else printf("\ntrack: volume %d%%\n", volume);
		break;
	case '[':
	case ']':
		separation += (key == ']') ? 10 : -10;
		if (separation < 0) separation = 0;
		if (separation > 100) separation = 100;
		modplay_set_separation(&player, separation);
		break;
	default:
		break;
	}
}

static void drain_messages(void) {
	z_msg_t msg;
	int redraw = 0;

	while (z_msg_read(&msg) == Z_OK) {
		if (msg.subject == Z_WM_REDRAW) {
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			redraw = 1;
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);
		} else if (msg.subject == Z_WM_KEY) {
			/* Z_WM_KEY is a PACKED Z_UINT32 (Z_WM_PACK_KEY in
			 * zwm.h), not a bare keysym -- it carries modifiers and
			 * the press/release flag alongside it. Passing the packed
			 * word straight to handle_key() compares it against 'n',
			 * ' ' and so on, which never matches, so no key did
			 * anything at all.
			 *
			 * Releases are dropped too. Acting on both edges makes
			 * every toggle fire twice: space would pause and instantly
			 * unpause, and nothing would appear to happen there
			 * either. */
			if (Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32))
				handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32));
		}
	}

	if (redraw) {
		draw_all();
		z_win_redraw_done(&win);
	}
}

/*
 * Top the FIFO up, WITH A BOUND ON HOW MUCH WORK IT DOES.
 *
 * The bound is the whole point, and leaving it out was a real bug.
 *
 * This used to loop until the FIFO was full. With a 128-frame FIFO
 * that was at most 5.8ms of audio per pass and nobody noticed. When
 * the FIFO went to 1024 frames the same loop started rendering up to
 * 46ms of audio -- more wall time than that, since the mixer runs
 * below real time -- before returning to drain_messages() even once.
 *
 * wm's repair_region() sends Z_WM_REDRAW and BLOCKS waiting for the
 * ack (see docs/window_manager.md). So a player that disappears into
 * its mixer for 50ms at a time stalls the window manager for 50ms at
 * a time: the whole desktop freezes, the busy cursor stays up, and
 * the mod window itself stops updating. Making the buffer bigger made
 * the freeze proportionally worse, which is the opposite of what a
 * bigger buffer is supposed to do.
 *
 * FEED_BLOCKS is therefore a latency budget, not a throughput knob.
 * Four blocks is 256 frames, 11.6ms of audio at 22kHz -- enough that
 * the per-pass overhead is amortised, short enough that the wm never
 * waits longer than that for an ack.
 *
 * The FIFO is what stores audio ahead of time. This loop's job is
 * only to keep it topped up in small bites.
 */
#define FEED_BLOCKS 4

/*
 * Hardware-mixer service.
 *
 * There is no audio work here at all. The tracker is advanced one tick
 * at a time and the resulting channel state is written to the mixer;
 * the mixer does every per-sample thing itself, off frame_req, on a
 * clock that cannot be preempted.
 *
 * Ticks are paced off the HARDWARE's own frame counter, not off
 * z_uptime_ticks(). The kernel tick is 732Hz and a tracker tick at
 * 125 BPM is 50Hz -- close enough that pacing off the wrong clock
 * would drift audibly over a few minutes, and would also make tempo
 * depend on scheduling jitter, which is the disease being cured here.
 * The mixer consumes exactly samples_per_tick frames per tracker tick
 * by definition, so counting frames is exact.
 *
 * Being late here is harmless in a way it never was in software mode:
 * the mixer keeps playing the current note. A late tick is a slightly
 * long row, not a gap. That is the difference the hardware buys.
 */
static void write_channels(void);

static void feed_hw(void) {
	int budget;

	if (paused) return;

	/*
	 * Advance the tracker to WHERE THE CLOCK SAYS IT SHOULD BE, up to
	 * a budget -- not one tick per call.
	 *
	 * This is the bug that produced "rate 50%". feed_hw() used to
	 * advance exactly one tick per call, which silently made the
	 * tempo depend on how fast the main loop happened to be running:
	 * a tick could never happen more often than an iteration. A
	 * tracker tick is 20ms and a full pattern redraw costs most of
	 * that, so the loop period crept past 20ms and every single call
	 * arrived late. The catch-up clamp then did exactly what it was
	 * asked and threw the backlog away, and the module played at half
	 * speed, evenly, forever.
	 *
	 * Before the clamp the same condition produced catch-up bursts
	 * instead -- correct average tempo, audible lurching. Two very
	 * different symptoms, one cause: TEMPO WAS TIED TO LOOP RATE.
	 * Nothing that draws should be able to change the tempo.
	 *
	 * So the loop below advances until the clock is satisfied. The
	 * budget bounds how much can be made up in one go: 8 ticks is
	 * 160ms of music, enough to absorb any redraw or descheduling
	 * this app can suffer, short enough that a genuinely long stall
	 * (a debugger, a stopped process) does not fast-forward through
	 * half a pattern when it resumes.
	 *
	 * Being a tick or two late is inaudible now in a way it never was
	 * under software mixing: the hardware mixer keeps playing the
	 * current notes while we are away, so a late tick is a slightly
	 * long row, not a gap.
	 */
	hw_burst = 0;

	for (budget = 8; budget > 0; budget--) {

		uint32_t now256 = z_uptime_ticks() << 8;
		int32_t late = (int32_t)(now256 - hw_next_256);

		if (late < 0) return;

		/* Further behind than the budget can make up: the lost time
		 * is gone, so resync rather than fast-forward through it. */
		if (hw_dt_256 && late > (int32_t)(hw_dt_256 * 8))
			hw_next_256 = now256;

		modplay_advance(&player);

		{
			uint32_t rate = z_audio_rate_hz();
			hw_dt_256 = rate
				? (732u * player.samples_per_tick * 256u) / rate
				: 3746u;
			if (hw_dt_256 == 0) hw_dt_256 = 1;
			hw_next_256 += hw_dt_256;
		}

		rr_frames += player.samples_per_tick;
		hw_burst++;
		if (hw_burst > hw_burst_max) hw_burst_max = hw_burst;

		write_channels();
	}
}

/*
 * Dump what the app is actually programming into the mixer.
 *
 * Printed once per second to the serial console. The point is to make
 * the app's view checkable against what comes out of the jack: if
 * these values are sane and the sound is not, the fault is below this
 * layer -- the mixer, the bus, or main memory -- and if they are wrong
 * it is the engine or the address arithmetic.
 *
 * `base` is the PHYSICAL address the mixer fetches from. Compare it
 * against the process base `ps` reports: it should land inside the
 * process, well past its code.
 */
static void dump_channels(void)
{
	int c;

	printf("track: buf phys %08lx  mixvol %02lx  mixstat %02lx\n",
		(unsigned long)phys_of(modbuf),
		(unsigned long)(reg_audio_mixvol & 0xFF),
		(unsigned long)(reg_audio_mixstat & 0xFF));

	for (c = 0; c < player.channels; c++) {
		mod_channel_t *ch = &player.ch[c];
		uint32_t smp_hz = ch->period
			? (3546895u / (uint32_t)ch->period) : 0;
		printf("  ch%d smp%-2d per%4d %5luHz step%08lx vol%2d %s\n",
			c, ch->sample, ch->period, (unsigned long)smp_hz,
			(unsigned long)z_audio_step(smp_hz, z_audio_rate_hz()),
			ch->volume, ch->active ? "on" : "--");
	}
}

static void write_channels(void) {
	int c;

	for (c = 0; c < player.channels; c++) {
		mod_hw_channel_t h;
		modplay_hw_channel(&player, c, &h);

		if (h.trigger) {
			/* Full setup only on a real note start. Everything below
			 * is static for the life of the note, so writing it on
			 * every tick would be six stores per channel per tick for
			 * no reason. */
			Z_AUDIO_CH_BASE(c) = phys_of(modbuf) + h.base;
			Z_AUDIO_CH_LEN(c) = h.length;
			Z_AUDIO_CH_LOOPST(c) = h.loop_start;
			Z_AUDIO_CH_LOOPLEN(c) = h.loop_len;
		}

		Z_AUDIO_CH_STEP(c) = z_audio_step(h.sample_hz, z_audio_rate_hz());
		Z_AUDIO_CH_CTRL(c) = z_audio_ch_ctrl(h.gain_l, h.gain_r,
			h.enable, h.trigger, h.offset);
	}

}

static void feed_audio(void) {
	int passes;
	uint32_t space;

	if (paused) return;

	/* One status read for the whole burst rather than one per frame:
	 * z_audio_push() would re-read STATUS on every single push, which
	 * at 22kHz is 22,000 extra bus cycles a second spent asking a
	 * question we already know the answer to. */
	space = z_audio_space();

	for (passes = 0; passes < FEED_BLOCKS && space >= BLOCK; passes++) {
		int i;

		if (block_used >= BLOCK) {
			modplay_render(&player, block, BLOCK);
			block_used = 0;
			rr_frames += BLOCK;
		}

		for (i = block_used; i < BLOCK; i++)
			z_audio_push_unchecked(block[i * 2], block[i * 2 + 1]);

		space -= (uint32_t)(BLOCK - block_used);
		block_used = BLOCK;
	}
}

/* ------------------------------------------------------------------
 * playback
 * ------------------------------------------------------------------ */

/*
 * Push the current volume setting to whichever back end is running.
 *
 * The two scales differ -- the mixer's MIXVOL is 0..255 against a
 * >>10, the software engine's master is 768/channels against a >>8 --
 * so the percentage is converted separately for each rather than
 * shared. What matters is that a given percentage sounds the same
 * either way, not that the register values match.
 */
static void apply_volume(void) {
	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;

	if (hw_mix) {
		/* Full scale for the channel count, scaled by the setting.
		 * 255 suits four channels; eight need half that or they clip
		 * against each other. See rtl/audio_mixer.v. */
		uint32_t full = (player.channels <= 4) ? 255u : 128u;
		reg_audio_mixvol = (full * (uint32_t)volume) / 100u;
	} else {
		player.master = ((768 / (player.channels ? player.channels : 4))
			* volume) / 100;
	}
}

/* Returns 0 to stop entirely, 1 to advance to the next module. */
static int play(int index) {
	uint32_t size;
	int rc;
	uint32_t last_draw = 0;

	cur_file = index;

	size = load_module(files[index]);
	if (size == 0) return 1;

	rc = modplay_init(&player, modbuf, size, z_audio_rate_hz());
	if (rc != MOD_OK) {
		printf("track: %s: %s\n", files[index], modplay_strerror(rc));
		return 1;
	}
	modplay_set_separation(&player, separation);

	printf("track: \"%s\"  %d ch  %d positions  %lu bytes\n",
		player.name, player.channels, player.song_len,
		(unsigned long)size);

	{
		int k;
		for (k = 0; k < PATTERN_ROWS; k++) slot_row[k] = -2;
	}

	block_used = BLOCK;
	want_next = 0;
	paused = 0;
	rr_frames = 0;
	rr_start_tick = z_uptime_ticks();
	rr_percent = -1;
	last_drawn_row = -1;
	last_drawn_order = -1;

	z_audio_start(rate_div);
	z_audio_clear_underrun();

	if (hw_mix) {
		int c;
		for (c = 0; c < MOD_MAX_CHANNELS; c++)
			Z_AUDIO_CH_CTRL(c) = 0;
		z_audio_mixer_enable(true);
		hw_next_256 = z_uptime_ticks() << 8;
		/* a sane starting interval, so the catch-up clamp above has
		 * something to compare against on the very first tick */
		hw_dt_256 = 3746;
	}
	apply_volume();

	if (windowed) draw_all();

	while (!want_next && !want_quit) {

		{
			uint32_t t0 = z_uptime_ticks();
			uint32_t dt;
			if (hw_mix) feed_hw(); else feed_audio();
			dt = z_uptime_ticks() - t0;
			if (dt > t_feed_max) t_feed_max = dt;
		}

		/* Mix-rate measurement, once a second. z_uptime_ticks() is
		 * the 732Hz kernel tick; expected frames over that window is
		 * just rate * ticks / 732. Recomputed rather than accumulated
		 * so a long pause cannot skew it permanently. */
		{
			uint32_t now = z_uptime_ticks();
			uint32_t el = now - rr_start_tick;
			if (el >= 732) {
				uint32_t expect = (z_audio_rate_hz() / 732) * el;
				rr_percent = expect ? (int)((rr_frames * 100) / expect) : -1;
				rr_frames = 0;
				rr_start_tick = now;
				/* The diagnostics line only changes when these
				 * do, so it is redrawn here rather than on every
				 * status refresh -- one fewer line of glyphs in
				 * the path that runs eight times a second. */
				if (windowed) draw_diag();
				if (dump_stats) dump_channels();
				hw_burst_max = 0;
				t_feed_max = 0; t_msg_max = 0;
				t_pat_max = 0; t_sta_max = 0;
			}
		}

		if (windowed) {
			uint32_t t0 = z_uptime_ticks();
			uint32_t dt;

			drain_messages();

			dt = z_uptime_ticks() - t0;
			if (dt > t_msg_max) t_msg_max = dt;

			/* The pattern grid follows the MUSIC, not the clock: it is
			 * redrawn when the playing row changes and at no other
			 * time. At the default speed that is about eight times a
			 * second, and it means the display cannot get busier than
			 * the module it is showing. */
			if (show_pattern &&
				(player.row != last_drawn_row ||
				 player.order_pos != last_drawn_order)) {
				draw_pattern();
			}

			/* The position line and the channel bars follow the
			 * MUSIC too -- they only change when the row does, so
			 * marking them here rather than on a timer removes
			 * most of the header's work outright. */
			if (player.row != last_pos_row) {
				last_pos_row = player.row;
				hdr_dirty |= DIRTY_POS | DIRTY_CHANS;
			}

			/* One header item and one grid row per pass, so a
			 * redraw is spread across iterations instead of
			 * blocking feed_hw(). */
			t0 = z_uptime_ticks();
			header_step();
			dt = z_uptime_ticks() - t0;
			if (dt > t_sta_max) t_sta_max = dt;

			t0 = z_uptime_ticks();
			pattern_step();
			dt = z_uptime_ticks() - t0;
			if (dt > t_pat_max) t_pat_max = dt;

		} else {
			int32_t ev = hid_read_key();
			if (ev >= 0)
				handle_key(z_kbd_usage_to_keysym((uint8_t)(ev & 0xFF),
					(uint8_t)((ev >> 8) & 0xFF)));
			if (z_uptime_ticks() - last_draw > 366) {
				last_draw = z_uptime_ticks();
				printf("\r  pos %02d/%02d row %02d  mix %3d%%  %s   ",
					player.order_pos, player.song_len, player.row,
					rr_percent,
					z_audio_underrun() ? "UNDERRUN" : "        ");
				fflush(stdout);
			}
		}
	
		/* Yield. This loop used to spin, so a music player running in
		 * the background took a full scheduler share from whatever
		 * was in the foreground -- see docs/app_runtime.md.
		 *
		 * One tick (~1.37ms) rather than a longer sleep, because
		 * unlike a message-driven app this one has real periodic
		 * work: feeding the mixer, and the tracker tick itself.
		 * Waking 732 times a second is far more often than either
		 * needs and still returns essentially the whole timeslice.
		 *
		 * Safe against underrun by a wide margin: the audio FIFO
		 * holds several milliseconds and the hardware mixer is fed on
		 * tracker ticks at around 50Hz. */
		z_proc_wait(1);
	}

	if (!windowed) printf("\n");
	z_audio_stop();

	return want_quit ? 0 : 1;
}

int main(void) {
	int i;

	printf("\nmod -- ProTracker player\n");

	/* Feature bit FIRST, then MAGIC. On a bitstream built before
	 * rtl/audio.v existed, 0x7000_05xx is decoded by nothing, and an
	 * undecoded address on this bus never acks -- the probe would hang
	 * the CPU. See sw/common/zaudio.h. */
	if (!z_audio_present()) {
		printf("This bitstream has no audio block.\n");
		printf("Rebuild with `AUDIO in rtl/boards.vh for this board.\n");
		return 1;
	}

	/* 22kHz rather than 44.1kHz, and this is the phase-2 trade in one
	 * line: software mixing costs roughly 20%% of the CPU for four
	 * channels here, and about double at 44.1kHz. A player on its own
	 * can afford either; a game running alongside can afford neither,
	 * which is what phase 3's hardware mixer is for. */
	/* Start from the rate the board came up with. */
	rate_div = reg_audio_rate & 0xFF;
	if (!z_audio_rate_ok(rate_div)) rate_div = Z_AUDIO_RATE_44K;

	z_audio_start(rate_div);

	hw_mix = z_audio_mixer_present();
	printf("track: mixing in %s\n", hw_mix ? "HARDWARE" : "software");

	find_modules();

	if (nfiles == 0) {
		printf("No .mod files in the root directory.\n");
		z_audio_stop();
		return 1;
	}

	printf("found %d module%s\n", nfiles, nfiles == 1 ? "" : "s");

	/* Windowed if there is a wm to talk to, console if not.
	 *
	 * z_win_create_flags() fails rather than hanging when nothing is
	 * listening, so this doubles as the probe. The console path is not
	 * a lesser mode kept for nostalgia -- it is the one to use when
	 * something is wrong, because it depends on nothing but the audio
	 * block and stdout. */
	windowed = (z_win_create_flags(&win, "track", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) == Z_OK);

	if (!windowed)
		printf("no wm; running on the console (q quits)\n");

	/* Plays through the list and wraps, rather than exiting after the
	 * last module: this is a player left running while other things
	 * happen, which is the whole point of the exercise. n skips, the
	 * close icon or q stops. */
	for (i = 0; !want_quit; i++) {
		if (i >= nfiles) i = 0;
		if (!play(i)) break;
	}

	z_audio_stop();
	for (i = 0; i < nfiles; i++) free(files[i]);

	printf("track: done.\n");
	return 0;
}
