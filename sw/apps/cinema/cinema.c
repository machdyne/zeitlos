/*
 * cinema -- the film player (docs/cinema_app.md, docs/zv.md).
 *
 *   > run cinema                 then the open icon, or
 *   double-click a .zv file in files
 *
 * A desktop app: the film plays in a window at half size, in black and
 * white. Made bigger than the film -- maximized, or F11 (wm's full
 * screen) -- the window shows it at full size.
 *
 * -- how it keeps time --
 *
 * The sound is the clock. A ZV record carries a frame and its share of
 * the sound; the sound goes into the hardware mixer's ring as soon as a
 * record is read, about half a second ahead, and the mixer's position
 * says how much has been played. Frame n is due at the 60 Hz refresh
 * zv_frame_vsync() gives it; without sound (a file with none, or a
 * bitstream without the mixer) the refresh counter itself is the clock.
 *
 * -- how it draws --
 *
 * Two pictures in main memory, one bit a pixel in the framebuffer's
 * order. The one shown is the front; the next frame is decoded into the
 * back (a copy of the front first: ZV skips unchanged blocks), and at
 * the first refresh at or after its time the back goes to the window
 * with the blitter and the two change places. A frame that falls behind
 * is decoded without being shown, so the picture catches up with the
 * sound rather than the sound waiting for it.
 *
 * -- seeking --
 *
 * A ZV seek point sends the codebooks whole; the picture is then
 * refreshed over the next `refresh` frames (the header says how many).
 * So a seek starts at the seek point before (target - refresh) and
 * decodes quietly up to the target.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/zfsc.h"
#include "../../common/zaudio.h"
#include "../../common/zv.h"
#include "../../common/zcycles.h"
#include "../../common/zaccent.h"
#include "../../common/zport.h"
#include "netsrc.h"

#define VRAM        ((volatile uint32_t *)0x20000000)
#define VRAM_WPL    (Z_SCREEN_W / 32)

#define NO_FILE_W   320
#define NO_FILE_H   180

/* the pictures: mono, a row of words and one more the blitter may
 * read; or chunky's 320x240, a byte a pixel (the larger) */
#define PIC_WPL     (ZV_MAX_W / 32 + 1)
#define PIC_WORDS   (Z_CHUNKY_W * Z_CHUNKY_H / 4 + 16)

/* records waiting: their sound written or not, their picture decoded
 * or not. Half a second of a 200 KB/s film fits several times. */
#define RQ_BYTES    (256 * 1024)
#define RQ_N        128

/* the mixer's ring of signed 8-bit samples */
#define MIXRING     (64 * 1024)
#define MIXMASK     (MIXRING - 1)

#define SEEK_POINTS 8192

static z_win_t win;
static z_dialog_ctx_t dlg_ctx;

/* -- the file -- */

static int fh = -1;
/* the film is on a web server (netsrc.c), not on the card */
static uint32_t sb_p, sb_f;
static bool net_src;
static bool net_wait;           /* a read may wait for the network (a seek; not while playing) */
static bool net_stall;          /* the sound was stopped for want of film: buffering */
static bool net_failed;         /* said so */
static uint32_t file_pos;
static zv_header_t hd;
static uint32_t keys[SEEK_POINTS * 2];          /* frame, offset */
static uint32_t nkeys;
static char filename[200];
static char last_dir[80] = Z_DIR_MEDIA_VIDEO;
static char title_buf[100];
static bool have_file;

/* -- records -- */

typedef struct {
	uint32_t off, size, frame;
	bool sound;                                 /* its sound is in the ring (or there is none) */
} rq_t;

static uint8_t rqbuf[RQ_BYTES] __attribute__((aligned(4)));
static rq_t rq[RQ_N];
static int rq_head, rq_n;                       /* oldest, count */
static uint32_t next_read;                      /* frame of the next record in the file */
static bool at_end;

/* -- pictures -- */

static uint32_t pic[2][PIC_WORDS] __attribute__((aligned(16)));
static int front;                               /* index into pic */

/* where the pictures go */
enum {
	OUT_WINDOW,         /* the window (mono, half or full): blitted */
	OUT_GAME_MONO,      /* game mode, mono, doubled: blitted to the viewport */
	OUT_CHUNKY_VRAM,    /* game mode, chunky in VRAM (45F): changed blocks copied in */
	OUT_CHUNKY_MEM      /* game mode, chunky in main memory (VRAM_SDRAM): page flips */
};
static int out_kind = OUT_WINDOW;
static int pic_ox, pic_oy;                      /* chunky: where the film sits in 320x240 */
static int pic_bs = 4;                          /* chunky: a block's size there (4 half, 8 full) */
/* chunky: the blocks changed in the back since the last change of
 * places (acc), and those the new back still lacks (sync) */
static uint8_t dmap_acc[ZV_MAX_BW * ZV_MAX_BH], dmap_sync[ZV_MAX_BW * ZV_MAX_BH];
static bool sync_due, flip_wait;
static uint16_t flip_fc;
static uint16_t greys[ZV_PAL_MAX];
static int ngreys;
static int32_t front_frame = -1, back_frame = -1;
static zv_dec_t dec;
static int view = ZV_VIEW_MONO_HALF;
static int pw, ph, pstride;                     /* the view's picture: pixels, bytes a row */
static bool use_hw_blit;

/* -- sound -- */

static bool sound;                              /* this file's sound is being played */
static bool mix_avail;
static uint32_t out_hz;
static uint8_t mixring[MIXRING + 4] __attribute__((aligned(4)));
static uint32_t mix_head;                       /* bytes written, ever (since the start) */
static uint32_t mix_laps, mix_lastpos;
static uint32_t clock_frame;                    /* the frame playing started at */
static uint16_t fc_start;                       /* without sound: the refresh then */
static uint32_t vs_start;

/* -- state -- */

static bool paused, looping;
static uint16_t last_fc;
static bool quit_now;

/* ------------------------------------------------------------------ */

/* through the kernel's UART ring (Z_SYS_UART_PUTC): a byte stored
 * straight into UART0's data register is lost when its FIFO is full,
 * which a 150-byte status line is -- and the ring keeps it in one piece
 * beside wm's and the shell's output */
void uart_putc(char c);
static void dbg(const char *s) {
	/* a bare LF leaves a serial terminal's cursor mid-line, so the
	 * next thing typed lands after the status line: CR LF, as
	 * printf's _write() sends */
	while (*s) {
		if (*s == '\n') uart_putc('\r');
		uart_putc(*s++);
	}
}

/* apps run at 0x8000_0000 through the MTU; the mixer reads physical
 * memory (as sw/apps/play does it) */
#define Z_APP_VIRT_BASE 0x80000000u

static void dbg_dec(uint32_t v) {
	char b[12];
	int i = 11;
	b[i] = 0;
	do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
	dbg(b + i);
}

/* -- how playing went, told on UART0 at the end and on quit -- */
static uint32_t st_shown, st_dropped, st_late, st_dec_cycles, st_dec_n, st_dec_max;
static uint32_t st_read_cycles, st_read_bytes, st_show_cycles;
static bool st_lines;           /* the five-second lines: off until `i` */
static uint32_t st_last_fc, st_last_n, st_dec_worst;    /* worst: since the last line */
static uint32_t st_read_calls, st_call_cyc;     /* since the last report_fs() */
static void rd_report(void);
static uint32_t st_t0_sum, st_t1_sum, st_rq_calls;      /* the film's reads: the counter before and after, summed */
static z_fs_stats_t st_fs0;             /* fs's counters when the film opened */
static bool st_fs_ok;

/* where the reading went, in fs (Z_FSOP_STATS): per call to it, how
 * much was fs, and of that how much the card -- the rest is the trip
 * there and back, and FatFs */
static void report_fs(void) {
	z_fs_stats_t s;
	uint32_t n, c;
	if (net_src) {
		/* a film on a server: what came, in how many requests, and how
		 * far ahead of the player the network is */
		ns_stats_t ns;
		ns_stats(&ns, true);
		dbg("cinema: net: "); dbg_dec(ns.bytes); dbg(" bytes in "); dbg_dec(ns.requests);
		dbg(" requests ("); dbg_dec(ns.connects); dbg(" connections, "); dbg_dec(ns.retries);
		dbg(" tried again); "); dbg_dec(ns_stream_buffered() + (sb_f - sb_p)); dbg(" bytes and ");
		dbg_dec((uint32_t)rq_n); dbg(" frames ahead; waited "); dbg_dec(ns.wait_ticks * 1000u / Z_TICK_HZ);
		dbg(" ms");
		if (ns_error()[0]) { dbg("; "); dbg(ns_error()); }
		dbg("\n");
		return;
	}
	if (!st_fs_ok || zfs_stats(&s)) return;
	n = s.reads - st_fs0.reads;
	if (!n) return;
	c = s.read_cyc - st_fs0.read_cyc;
	dbg("cinema: fs: "); dbg_dec(n); dbg(" reads, "); dbg_dec(s.read_bytes - st_fs0.read_bytes);
	dbg(" bytes; a read "); dbg_dec(st_read_calls ? st_call_cyc / st_read_calls : 0);
	dbg(" cycles here, in fs "); dbg_dec(c / n); dbg(", the card ");
	dbg_dec((s.disk_read_cyc - st_fs0.disk_read_cyc) / n);
	dbg(" of them ("); dbg_dec(s.sectors_read - st_fs0.sectors_read); dbg(" sectors in ");
	dbg_dec(s.disk_reads - st_fs0.disk_reads); dbg(" card reads, ");
	dbg_dec(s.dma_bytes - st_fs0.dma_bytes); dbg(" bytes by DMA)\n");
	/* the trip each way, when every read fs served was one of the
	 * film's (a colour film's layer reads beside them: not then) */
	if (st_rq_calls == n) {
		dbg("cinema: fs: a request takes "); dbg_dec((s.read_t0_sum - st_fs0.read_t0_sum - st_t0_sum) / n);
		dbg(" cycles to reach fs, its answer "); dbg_dec((st_t1_sum - (s.read_t1_sum - st_fs0.read_t1_sum)) / n);
		dbg(" to come back\n");
	}
	rd_report();
	dbg("cinema: fs woke with no request "); dbg_dec(s.idle_passes - st_fs0.idle_passes);
	dbg(" times (once a second is its own timer)\n");
	st_t0_sum = st_t1_sum = st_rq_calls = 0;
	st_fs0 = s;
	st_call_cyc = 0;
}

/* where the whole machine's time went since the last call (the kernel's
 * accounting, z_cpu_stat(); docs/kernel.md): cinema, fs, wm, net, the
 * rest, interrupts and idle, in percent -- whether frames are missed
 * for want of time, and whose */
static z_cpu_stat_args_t cpu0;
static bool cpu0_ok;

static uint64_t cpu_c(const z_cpu_count_t *c) { return ((uint64_t)c->hi << 32) | c->lo; }

static void cpu_part(const char *name, uint64_t v, uint64_t all) {
	dbg(name); dbg(" "); dbg_dec(all ? (uint32_t)(v * 100 / all) : 0); dbg("%");
}

static void report_cpu(void) {
	static z_cpu_stat_args_t c;
	uint32_t me = z_getpid(), fs = 0xffffffffu, wm = 0xffffffffu, net = 0xffffffffu, i;
	uint64_t all, d, mine = 0, f = 0, w = 0, n = 0, rest = 0, irq = 0;
	if (z_cpu_stat(&c) != Z_OK) return;
	if (!cpu0_ok) { cpu0 = c; cpu0_ok = true; return; }
	(void)z_pid_lookup(Z_FS_SERVICE, &fs);
	(void)z_pid_lookup("wm0", &wm);
	(void)z_pid_lookup("net0", &net);
	all = (uint32_t)(c.now - cpu0.now);
	for (i = 0; i < c.n && i < Z_CPU_STAT_PROCS; i++) {
		d = cpu_c(&c.proc[i]) - cpu_c(&cpu0.proc[i]);
		if (i == me) mine += d;
		else if (i == fs) f += d;
		else if (i == wm) w += d;
		else if (i == net) n += d;
		else rest += d;
	}
	for (i = 0; i < 4; i++) irq += cpu_c(&c.irq[i]) - cpu_c(&cpu0.irq[i]);
	dbg("cinema: cpu: ");
	cpu_part("cinema", mine, all); dbg(", ");
	cpu_part("fs", f, all); dbg(", ");
	cpu_part("wm", w, all); dbg(", ");
	cpu_part("net", n, all); dbg(", ");
	cpu_part("others", rest, all); dbg(", ");
	cpu_part("interrupts", irq, all); dbg(", ");
	cpu_part("idle", cpu_c(&c.idle) - cpu_c(&cpu0.idle), all);
	dbg("\n");
	cpu0 = c;
}

static void report(void) {
	if (!st_shown && !st_dropped) return;
	if (st_lines) report_fs();
	dbg("cinema: "); dbg_dec(st_shown); dbg(" frames shown, ");
	dbg_dec(st_late); dbg(" of them a refresh or more late, ");
	dbg_dec(st_dropped); dbg(" decoded but not shown; decoding ");
	dbg_dec(st_dec_n ? st_dec_cycles / st_dec_n : 0); dbg(" cycles a frame, at most ");
	dbg_dec(st_dec_max);
	dbg("; reading "); dbg_dec(st_dec_n ? st_read_cycles / st_dec_n : 0);
	dbg(", showing "); dbg_dec(st_shown ? st_show_cycles / st_shown : 0); dbg(" cycles a frame\n");
	st_shown = st_dropped = st_late = st_dec_cycles = st_dec_n = st_dec_max = 0;
	st_last_n = 0;
	st_read_cycles = st_read_bytes = st_show_cycles = 0;
}

static uint32_t phys_of(const void *p) {
	return reg_mtu_base + ((uint32_t)p - Z_APP_VIRT_BASE);
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* (The film itself is read by rq_fill(), in pieces straight into the
 * queue's buffer; this is for the header and the index.) */

static int read_full(void *buf, uint32_t n) {
	uint8_t *b = buf;
	uint32_t got = 0;
	if (net_src) {
		int r = ns_read_at(file_pos, buf, n);
		if (r != (int)n) return -1;
		file_pos += n;
		return 0;
	}
	while (got < n) {
		uint32_t t = z_cycles();
		int r = fs_read_chunk(fh, b + got, (int)(n - got > 16384 ? 16384 : n - got));
		st_call_cyc += z_cycles() - t;
		st_read_calls++;
		if (r <= 0) return -1;
		got += (uint32_t)r;
	}
	return 0;
}

static void file_seek(uint32_t off) {
	if (net_src) file_pos = off;
	else fs_seek(fh, off);
}

/* -- the sound ----------------------------------------------------- */

static void mixer_off(void) {
	int c;
	if (!mix_avail) return;
	z_audio_mixer_enable(false);
	for (c = 0; c < 2; c++) Z_AUDIO_CH_CTRL(c) = 0;
}

/* sound bytes the mixer has consumed since it started. From the write
 * head and the mixer's place in the ring: what is still to play is
 * (head - pos) mod the ring, so nothing depends on seeing every lap --
 * a slow pass cannot make the clock jump. (If the mixer ever overtook
 * the head it would replay old sound; the clock then stops at the head
 * rather than running on.) */
static uint32_t mixer_consumed(void) {
	uint32_t pos = z_audio_ch_pos_bytes(0) & MIXMASK;
	uint32_t ahead = (mix_head - pos) & MIXMASK;
	uint32_t c = mix_head - ahead;
	(void)mix_laps;
	if ((int32_t)(c - mix_lastpos) < 0) c = mix_lastpos;   /* never backwards */
	mix_lastpos = c;
	return c;
}

static uint32_t mixer_used(void) {
	uint32_t c = mixer_consumed();
	return mix_head > c ? mix_head - c : 0;
}

static void mixer_start(void) {
	uint32_t base = phys_of(mixring), step;
	uint8_t g = 255;
	int c;
	if (!sound) return;
	if (hd.achan == 2) {
		step = ((2u * hd.arate) << Z_AUDIO_STEP_FRAC) / out_hz;
		for (c = 0; c < 2; c++) {
			Z_AUDIO_CH_BASE(c) = base + (uint32_t)c;
			Z_AUDIO_CH_LEN(c) = MIXRING;
			Z_AUDIO_CH_LOOPST(c) = 0;
			Z_AUDIO_CH_LOOPLEN(c) = MIXRING;
			Z_AUDIO_CH_STEP(c) = step;
		}
		Z_AUDIO_CH_CTRL(0) = z_audio_ch_ctrl(g, 0, true, true, 0);
		Z_AUDIO_CH_CTRL(1) = z_audio_ch_ctrl(0, g, true, true, 0);
	} else {
		step = (hd.arate << Z_AUDIO_STEP_FRAC) / out_hz;
		Z_AUDIO_CH_BASE(0) = base;
		Z_AUDIO_CH_LEN(0) = MIXRING;
		Z_AUDIO_CH_LOOPST(0) = 0;
		Z_AUDIO_CH_LOOPLEN(0) = MIXRING;
		Z_AUDIO_CH_STEP(0) = step;
		Z_AUDIO_CH_CTRL(0) = z_audio_ch_ctrl(g, g, true, true, 0);
	}
	mix_laps = 0;
	mix_lastpos = 0;
	z_audio_mixer_enable(true);
}

/* a record's sound into the ring, if there is room for all of it */
static bool sound_put(rq_t *r) {
	zv_rec_t rec;
	uint32_t n, i, at;
	const uint8_t *s;
	if (!sound) return true;
	if (zv_split_record(&hd, rqbuf + r->off, r->size, &rec)) return true;
	n = rec.n_audio * hd.achan;
	if (mix_head + n - mixer_consumed() > MIXRING - 64) return false;
	s = rec.audio;
	at = mix_head;
	for (i = 0; i < n; i++, at++) mixring[at & MIXMASK] = (uint8_t)(s[i] ^ 0x80);
	mix_head = at;
	return true;
}

/* the 60 Hz refresh it is now, counted from the film's start */
static uint32_t now_vsync(void) {
	if (sound) {
		uint64_t played = (uint64_t)zv_audio_before(&hd, clock_frame) + mixer_consumed() / hd.achan;
		return (uint32_t)(played * 60 / hd.arate);
	}
	return vs_start + (uint16_t)(z_game_frame() - fc_start);
}

/* -- records ---------------------------------------------------------- */

static bool end_pad;                /* the film's sound is all in the ring, and a silence after it */
static uint32_t end_used;
static uint16_t end_stall;      /* refreshes played with the mixer no nearer the end */
static void rq_flush(void) {
	rq_head = rq_n = 0;
	sb_p = sb_f = 0;
	at_end = false;
	end_pad = false;
}

/* Whose time a read took (the timing lines, `i`): the kernel's accounts
 * (z_cpu_stat()) before and after each read, summed by who was charged
 * -- this process (its own syscalls and switches in and out), fs, any
 * other process that ran meanwhile, interrupt handlers, and nobody. */
static z_cpu_stat_args_t rd_c0, rd_c1;
static uint32_t rd_who[5], rd_wall, rd_n;       /* me, fs, others, interrupts, idle */

static uint32_t rd_d(const z_cpu_count_t *a, const z_cpu_count_t *b) { return b->lo - a->lo; }

static void rd_account(uint32_t t0) {
	uint32_t me = z_getpid(), fs = 0xffffffffu, i;
	uint32_t wall = z_cycles() - t0;
	if (z_cpu_stat(&rd_c1) != Z_OK) return;
	(void)z_pid_lookup(Z_FS_SERVICE, &fs);
	for (i = 0; i < rd_c1.n && i < Z_CPU_STAT_PROCS; i++) {
		uint32_t d = rd_d(&rd_c0.proc[i], &rd_c1.proc[i]);
		rd_who[i == me ? 0 : i == fs ? 1 : 2] += d;
	}
	for (i = 0; i < 4; i++) rd_who[3] += rd_d(&rd_c0.irq[i], &rd_c1.irq[i]);
	rd_who[4] += rd_d(&rd_c0.idle, &rd_c1.idle);
	rd_wall += wall;
	rd_n++;
}

static void rd_report(void) {
	static const char *const nm[5] = { "cinema ", ", fs ", ", other processes ", ", interrupts ", ", idle " };
	int i;
	if (!rd_n) return;
	dbg("cinema: a read's "); dbg_dec(rd_wall / rd_n); dbg(" cycles, by whose they were: ");
	for (i = 0; i < 5; i++) { dbg(nm[i]); dbg_dec(rd_who[i] / rd_n); rd_who[i] = 0; }
	dbg("\n");
	rd_wall = rd_n = 0;
}

/* The file comes in pieces of RD_CHUNK, each ending on a sector's
 * boundary, straight into the queue's buffer, and the records are found
 * in place: a call to `fs` every other frame or so rather than one a
 * frame (each a trip through the kernel and back), and whole sectors
 * only, which FatFs and the card pass straight through -- no part
 * sectors by its buffer (docs/cinema_app.md, "Reading").
 *
 * [sb_p, sb_f) is read and not yet a record; the queue's records are
 * before it, or after it once the buffer has wrapped. */
#define RD_CHUNK 8192
static uint32_t rd_pos;                         /* the file's position */

static bool rq_fill(void) {
	uint32_t n = net_src ? RD_CHUNK : RD_CHUNK - (rd_pos & 511), t;
	int r;
	if (rq_n == 0 && sb_p == sb_f) sb_p = sb_f = 0;
	if (rq_n && rq[rq_head].off > sb_p) {
		/* wrapped: up to the oldest record */
		if (sb_f + n >= rq[rq_head].off) return false;
	} else if (sb_f + n > RQ_BYTES) {
		/* to the start, with what is not yet a record (word aligned
		 * where the next piece lands, for the card's DMA) */
		uint32_t tail = sb_f - sb_p, base = (4 - (tail & 3)) & 3;
		uint32_t limit = rq_n ? rq[rq_head].off : sb_p;
		if (base + tail + n >= limit) return false;
		memmove(rqbuf + base, rqbuf + sb_p, tail);
		sb_p = base;
		sb_f = base + tail;
	}
	if (st_lines) (void)z_cpu_stat(&rd_c0);
	t = z_cycles();
	if (net_src) {
		/* what has arrived of it (netsrc.c asks ahead); none yet is not
		 * the end -- that is the server's last byte, or its silence */
		r = (int)ns_stream_read(rqbuf + sb_f, n, net_wait);
		if (r == 0) {
			if (ns_stream_ended()) at_end = true;
			return false;
		}
		sb_f += (uint32_t)r;
		rd_pos += (uint32_t)r;
		st_read_bytes += (uint32_t)r;
		st_read_calls++;
		return true;
	}
	r = fs_read_chunk(fh, rqbuf + sb_f, (int)n);
	if (st_lines) rd_account(t);
	st_t0_sum += t;
	t = z_cycles() - t;
	st_t1_sum += z_cycles();
	st_rq_calls++;
	st_call_cyc += t;
	st_read_cycles += t;
	st_read_calls++;
	if (r <= 0) { at_end = true; return false; }
	sb_f += (uint32_t)r;
	rd_pos += (uint32_t)r;
	st_read_bytes += (uint32_t)r;
	return true;
}

/* the next record into the queue; false if there is no room or no more */
static bool rq_read(void) {
	uint32_t size;
	rq_t *r;
	if (at_end || next_read >= hd.frames) { at_end = true; return false; }
	if (rq_n >= RQ_N) return false;
	for (;;) {
		uint32_t have = sb_f - sb_p;
		if (have >= ZV_REC_HDR) {
			size = rd32(rqbuf + sb_p);
			if (size < ZV_REC_HDR || size > hd.max_rec) { at_end = true; return false; }
			if (have >= size) break;
		}
		if (!rq_fill()) return false;
	}
	r = &rq[(rq_head + rq_n) % RQ_N];
	r->off = sb_p;
	r->size = size;
	r->frame = next_read++;
	r->sound = !sound;
	sb_p += size;
	rq_n++;
	return true;
}

static void col_prefetch(void);

/* write what sound fits; read records while the sound is less than
 * about a second ahead */
static void feed(void) {
	int i, guard = 0;
	uint32_t lead = sound ? hd.arate * hd.achan : 0;     /* a second */
	col_prefetch();
	if (lead > MIXRING / 2) lead = MIXRING / 2;
	for (;;) {
		for (i = 0; i < rq_n; i++) {
			rq_t *r = &rq[(rq_head + i) % RQ_N];
			if (r->sound) continue;
			if (!sound_put(r)) return;
			r->sound = true;
		}
		if (sound ? mixer_used() >= lead : rq_n >= 4) return;
		if (!rq_read()) return;
		if (++guard > 64) return;
	}
}

static rq_t *rq_first_video(void) {
	return rq_n ? &rq[rq_head] : NULL;
}

static void rq_pop(void) {
	rq_head = (rq_head + 1) % RQ_N;
	rq_n--;
}

/* -- pictures ---------------------------------------------------------- */

static bool chunky_out(void) { return out_kind == OUT_CHUNKY_VRAM || out_kind == OUT_CHUNKY_MEM; }

/* The greys window (below): a black and white film through the colour
 * window, the decoder's four grey planes (ZV_VIEW_PL_*) in pic[0] */
#define GW_ST       48          /* a plane's row, bytes: 320 pixels and a word's alignment */
#define GW_PL       (GW_ST * 240)   /* a plane to the next */
static bool gw;                 /* the decoder is in a plane view */
static bool gw_all;             /* every row is to be shown, not the changed ones */

/* the decoder's view for `view`: its plane twin while the greys window is on */
static int dec_view(void) {
	if (!gw) return view;
	return view == ZV_VIEW_MONO_FULL ? ZV_VIEW_PL_FULL : ZV_VIEW_PL_HALF;
}

static void view_layout(void) {
	zv_view_size(&dec, &pw, &ph, &pstride);
	dec.plane_off = GW_PL;
	if (chunky_out()) {
		pstride = Z_CHUNKY_W;
		pic_bs = dec.view == ZV_VIEW_PAL_FULL ? 8 : 4;
		pic_ox = ((Z_CHUNKY_W - pw) / 2) & ~7;
		pic_oy = (Z_CHUNKY_H - ph) / 2;
		if (pic_ox < 0) pic_ox = 0;
		if (pic_oy < 0) pic_oy = 0;
		dec.dirty = dmap_acc;
	} else {
		pstride = gw ? GW_ST : PIC_WPL * 4;
		pic_ox = pic_oy = 0;
		dec.dirty = gw ? dmap_acc : NULL;      /* the rows to show */
	}
}

/* (the controls bar and the seeks, further down) */
static int bar_h(void);
static void bar_toggle(void);
static void bar_update(void);
static int32_t seek_want;
static void seek_ask(int32_t f, int dir);
static void set_view(int v);
static int view_for_window(void);

/* where the decoder writes in picture k */
static uint8_t *pic_out(int k) {
	/* Two pictures only where the screen shows one of them (chunky in
	 * main memory, a page flip a frame). Elsewhere the screen is VRAM
	 * and holds this frame while the next is decoded into the one
	 * picture: no copy a frame to keep a second in step. */
	return (uint8_t *)pic[out_kind == OUT_CHUNKY_MEM ? k : 0] + pic_oy * pstride + pic_ox;
}

/* chunky: the blocks in `map` from picture `from` to `to` (both in
 * main memory), or to VRAM's segments (to < 0). In VRAM, row r of
 * segment s is word (r + 240 (s & 1) + 480 (s >> 1)) * 20: each block
 * column's segment and word are worked out once (segoff). */
static uint16_t segoff[ZV_MAX_BW];
static int segoff_ox = -1;

static void copy_blocks(const uint8_t *map, int from, int to) {
	int bw = dec.bw, bh = dec.bh, bx, by, y;
	const uint32_t *src = (const uint32_t *)(const void *)pic_out(from);
	int bs = pic_bs, wpb = pic_bs / 4, k;
	if (to < 0 && segoff_ox != pic_ox * 16 + bs) {
		for (bx = 0; bx < ZV_MAX_BW; bx++) {
			int x = pic_ox + bx * bs, seg = x / Z_CHUNKY_SEG_PX;
			if (seg > 3) seg = 3;
			segoff[bx] = (uint16_t)((((seg & 1) ? 240 : 0) + ((seg & 2) ? 480 : 0)) * Z_CHUNKY_SEG_WORDS +
			                        (x % Z_CHUNKY_SEG_PX) / 4);
		}
		segoff_ox = pic_ox * 16 + bs;
	}
	for (by = 0; by < bh; by++) {
		const uint8_t *m = map + by * bw;
		const uint32_t *srow = src + by * bs * (Z_CHUNKY_W / 4);
		for (bx = 0; bx < bw; bx++) {
			const uint32_t *s;
			if (!m[bx]) continue;
			s = srow + bx * wpb;
			if (to >= 0) {
				uint32_t *d = (uint32_t *)(void *)pic_out(to) + by * bs * (Z_CHUNKY_W / 4) + bx * wpb;
				for (y = 0; y < bs; y++) for (k = 0; k < wpb; k++) d[y * (Z_CHUNKY_W / 4) + k] = s[y * (Z_CHUNKY_W / 4) + k];
			} else {
				/* an 8-wide block never straddles segments: 80 and the
				 * offset are multiples of 4, and of 8 when bs is 8 */
				volatile uint32_t *d = Z_CHUNKY_VRAM + (pic_oy + by * bs) * Z_CHUNKY_SEG_WORDS + segoff[bx];
				for (y = 0; y < bs; y++) for (k = 0; k < wpb; k++) d[y * Z_CHUNKY_SEG_WORDS + k] = s[y * (Z_CHUNKY_W / 4) + k];
			}
		}
	}
}

/* the colour window (below): the classes are wanted for it too */
static bool cw_track;
static void cw_blocks(const uint16_t *list, int n);
static void pic_place(int *x, int *y);

/* -- the colour layer (zv.h, docs/zv.md): chunky game mode ------------
 *
 * A class a block, 16 of them a seek point's group; a pixel's palette
 * index is class * 16 + its grey level, which the palette view already
 * draws. So after each decode the classes go into the blocks that
 * changed -- the decoder's or the classes' -- and those blocks go to the
 * screen as any changed block does. The group's 256 colours go into the
 * palette when its first picture is shown.
 *
 * The layer is after the index, read through a second handle: a group
 * at a time (a few KB, ~7 KB/s of film), the next one a little at a
 * time while this one plays (col_prefetch(), from feed()), so a seek
 * point costs no pause. */

#define COL_BUF     49152       /* a group's data, at most */
#define COL_GROUPS  SEEK_POINTS

static int fhc = -1;
static bool col_have;           /* the file has a usable layer */
static bool col_on;             /* and it is being shown (chunky game mode) */
static zv_col_header_t colh;
static uint32_t col_gidx[COL_GROUPS * 2];       /* first frame, offset (in the file) */
static uint32_t col_ng;
static uint8_t col_buf[2][COL_BUF] __attribute__((aligned(4)));
static int32_t col_bg[2] = { -1, -1 };          /* the group in each buffer */
static uint32_t col_len[2], col_want[2];        /* loaded, and all of it */
static uint32_t col_fpos = 0xffffffffu;         /* where fhc is */
static int col_cur;                             /* the playing group's buffer */
static int32_t col_g = -1;                      /* the playing group */
static bool col_g_ok;
static uint32_t col_at, col_next;               /* its next frame's runs, and that frame */
static uint8_t col_cls[ZV_MAX_BW * ZV_MAX_BH * 4];    /* cells: 8x8, or 4x4 */
static uint8_t col_cmap[ZV_MAX_BW * ZV_MAX_BH];      /* blocks whose class changed */
static uint32_t col_w[ZV_MAX_BW * ZV_MAX_BH * 4];    /* the classes as the decoder takes them */
static uint16_t col_list[ZV_MAX_BW * ZV_MAX_BH];    /* and the same, listed */
static int col_nch;                                  /* how many */
static uint16_t col_pal[2][256];                /* two groups' palettes */
static int32_t col_pal_g[2] = { -1, -1 };
static int32_t pal_shown = -2;                  /* the group in the hardware palette */
static int32_t pic_group[2] = { -1, -1 };       /* each picture's */
static int32_t col_frame_g = -1;                /* the group of the frame being decoded */

/* a group on its way from the server (ns_read_later()), into col_buf[col_nb] */
static int col_nb = -1;
static uint32_t col_ngot;
static bool col_ndone;

static void col_close(void) {
	if (fhc >= 0) { fs_close_handle(fhc); fhc = -1; }
	if (col_nb >= 0) { (void)ns_read_pending(true); col_nb = -1; }
	col_have = false;
	col_bg[0] = col_bg[1] = -1;
	col_pal_g[0] = col_pal_g[1] = -1;
	col_g = -1;
	pal_shown = -2;
}

static void col_open(const char *path) {
	uint8_t h16[ZV_COL_HDR];
	uint32_t i, n;
	col_close();
	if (!(hd.flags & ZV_F_COLOUR) || !hd.colour_off) return;
	if (!net_src) {
		fhc = fs_open_read(path);
		if (fhc < 0) return;
		fs_seek(fhc, hd.colour_off);
	}
	if ((net_src ? ns_read_at(hd.colour_off, h16, ZV_COL_HDR) : fs_read_chunk(fhc, h16, ZV_COL_HDR)) != ZV_COL_HDR ||
	    zv_col_parse_header(h16, ZV_COL_HDR, &colh) ||
	    colh.cw != hd.w / colh.cell || colh.ch != hd.h / colh.cell || !colh.groups || colh.groups > COL_GROUPS) {
		dbg("cinema: the colour layer is not one this player can show\n");
		col_close();
		return;
	}
	n = colh.groups;
	{
		uint8_t *b = (uint8_t *)col_gidx;
		uint32_t got = 0;
		if (net_src) {
			if (ns_read_at(hd.colour_off + ZV_COL_HDR, b, n * 8) != (int)(n * 8)) { col_close(); return; }
			got = n * 8;
		}
		while (got < n * 8) {
			int r = fs_read_chunk(fhc, b + got, (int)(n * 8 - got > 16384 ? 16384 : n * 8 - got));
			if (r <= 0) { col_close(); return; }
			got += (uint32_t)r;
		}
	}
	for (i = 0; i < n * 2; i++) {
		col_gidx[i] = rd32((const uint8_t *)&col_gidx[i]);
		if (i & 1) col_gidx[i] += hd.colour_off;
	}
	col_ng = n;
	col_fpos = 0xffffffffu;
	col_have = true;
}

static uint32_t col_group_of(uint32_t f) {
	uint32_t lo = 0, hi = col_ng;          /* the last group starting at or before f */
	while (hi - lo > 1) {
		uint32_t m = (lo + hi) / 2;
		if (col_gidx[m * 2] <= f) lo = m; else hi = m;
	}
	return lo;
}

/* col_load() for a film on a server: one request for the whole group,
 * asked for ahead and left to arrive between the film's own (the
 * prefetch, budget under COL_BUF), waited for only when the group is
 * wanted now (after a seek). A short answer is the file's end: the
 * last group, whose size the index does not give. */
static bool col_load_net(int b, uint32_t g, uint32_t budget) {
	uint32_t off = col_gidx[g * 2 + 1];
	bool now = budget >= COL_BUF;
	if (col_nb == b && col_bg[b] != (int32_t)g) {
		(void)ns_read_pending(true);        /* another group is arriving in this buffer: let it, unused */
		col_nb = -1;
	}
	if (col_nb >= 0 && now) (void)ns_read_pending(true);
	if (col_nb >= 0 && !ns_read_pending(false)) {
		col_len[col_nb] = col_ngot;
		if (col_ngot < col_want[col_nb]) col_want[col_nb] = col_ngot;
		col_nb = -1;
	}
	if (col_bg[b] != (int32_t)g) {
		uint32_t size = g + 1 < col_ng ? col_gidx[g * 2 + 3] - off : COL_BUF;
		col_bg[b] = (int32_t)g;
		col_len[b] = 0;
		col_want[b] = size <= COL_BUF ? size : 0;
	}
	if (!col_want[b] || col_len[b] >= col_want[b]) return true;
	if (col_nb == b) return false;              /* on its way */
	if (now) {
		int r = ns_read_at(off, col_buf[b], col_want[b]);
		col_len[b] = r > 0 ? (uint32_t)r : 0;
		col_want[b] = col_len[b];
		return true;
	}
	if (col_nb < 0 && ns_read_later(off, col_buf[b], col_want[b], &col_ngot, &col_ndone)) col_nb = b;
	return false;
}

/* up to `budget` more bytes of group g into buffer b; true when it is all there */
static bool col_load(int b, uint32_t g, uint32_t budget) {
	uint32_t off = col_gidx[g * 2 + 1];
	if (net_src) return col_load_net(b, g, budget);
	if (col_bg[b] != (int32_t)g) {
		uint32_t size = g + 1 < col_ng ? col_gidx[g * 2 + 3] - off : COL_BUF;
		col_bg[b] = (int32_t)g;
		col_len[b] = 0;
		col_want[b] = size <= COL_BUF ? size : 0;   /* 0: too big, shown grey */
	}
	if (!col_want[b]) return true;
	while (col_len[b] < col_want[b] && budget) {
		uint32_t n = col_want[b] - col_len[b];
		int r;
		if (n > budget) n = budget;
		if (n > 16384) n = 16384;
		if (col_fpos != off + col_len[b]) { fs_seek(fhc, off + col_len[b]); col_fpos = off + col_len[b]; }
		r = fs_read_chunk(fhc, col_buf[b] + col_len[b], (int)n);
		if (r <= 0) { col_want[b] = col_len[b]; break; }    /* the end of the file: the last group */
		col_len[b] += (uint32_t)r;
		col_fpos += (uint32_t)r;
		budget -= (uint32_t)r > budget ? budget : (uint32_t)r;
		if ((uint32_t)r < n) { col_want[b] = col_len[b]; break; }
	}
	return col_len[b] >= col_want[b];
}

/* the next group, a piece at a time (from feed()) */
static void col_prefetch(void) {
	if ((!col_on && !cw_track) || col_g < 0 || (uint32_t)col_g + 1 >= col_ng) return;
	(void)col_load(col_cur ^ 1, (uint32_t)col_g + 1, 2048);
}

/* group g, from its first frame */
static void col_enter(uint32_t g) {
	int b = col_bg[col_cur ^ 1] == (int32_t)g ? col_cur ^ 1 : col_cur;
	int slot;
	(void)col_load(b, g, COL_BUF);
	col_cur = b;
	col_g = (int32_t)g;
	col_g_ok = col_want[b] >= ZV_COL_TABLE && col_len[b] >= col_want[b];
	col_at = ZV_COL_TABLE;
	col_next = col_gidx[g * 2];
	/* its palette, beside the last group's (still on screen) */
	slot = col_pal_g[0] == (int32_t)g ? 0 : col_pal_g[1] == (int32_t)g ? 1 :
	       (pal_shown == col_pal_g[0] ? 1 : 0);
	if (col_g_ok) zv_col_palette((const int8_t *)col_buf[b], col_pal[slot]);
	else zv_col_palette((const int8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", col_pal[slot]);
	col_pal_g[slot] = (int32_t)g;
}

/* before frame f is decoded: its classes, for the decoder to draw with
 * (dec.col) -- the blocks whose class changed are noted, for the ones
 * the decode does not draw */
static void col_before_decode(uint32_t f) {
	uint32_t g, cells = (uint32_t)colh.cw * colh.ch, i;
	dec.colw = NULL;
	if (!col_on && !cw_track) return;
	g = col_group_of(f);
	/* (mid-group -- a seek point the index had no room for -- the loop
	 * below catches the classes up from the group's first frame) */
	if ((int32_t)g != col_g || f < col_next) col_enter(g);
	while (col_g_ok && col_next <= f) {
		int r = zv_col_frame_blocks(col_buf[col_cur] + col_at, col_len[col_cur] - col_at, col_cls,
		                            colh.cw, colh.ch, colh.cell, col_cmap, col_list, &col_nch);
		if (r < 0) { col_g_ok = false; break; }
		col_at += (uint32_t)r;
		col_next++;
	}
	if (!col_g_ok) {
		/* this group is shown grey: class 0 everywhere (every block
		 * listed; a broken or oversized group is rare) */
		int bx, by;
		for (i = 0; i < cells; i++) col_cls[i] = 0;
		col_nch = 0;
		for (by = 0; by < dec.bh; by++)
			for (bx = 0; bx < dec.bw; bx++) {
				col_cmap[by * dec.bw + bx] = 1;
				col_list[col_nch++] = (uint16_t)(by << 8 | bx);
			}
		col_next = f + 1;
	}
	if (!col_on) { col_frame_g = (int32_t)g; return; }     /* the window's: the planes, after */
	if (col_nch) zv_col_words_list(col_cls, colh.cw, colh.cell, col_list, col_nch, col_w);
	dec.colw = col_w;
	col_frame_g = (int32_t)g;
}

/* after it was decoded into picture k: the blocks whose class changed,
 * if the decode did not draw them, and all of them to be shown */
static void col_after_decode(int k) {
	int i;
	if (!col_on && !cw_track) return;
	if (!col_on) {
		/* the colour window: the classes go to its planes, not the picture */
		if (col_nch) {
			cw_blocks(col_list, col_nch);
			for (i = 0; i < col_nch; i++) col_cmap[(col_list[i] >> 8) * dec.bw + (col_list[i] & 0xff)] = 0;
			col_nch = 0;
		}
		pic_group[k] = col_frame_g;
		return;
	}
	if (col_nch) {
		zv_col_apply_list(col_w, dec.bw, col_list, col_nch, pic_bs, pic_out(k), pstride);
		for (i = 0; i < col_nch; i++) {
			int b = (col_list[i] >> 8) * dec.bw + (col_list[i] & 0xff);
			dmap_acc[b] = 1;
			col_cmap[b] = 0;
		}
		col_nch = 0;
	}
	pic_group[k] = col_frame_g;
}

/* the palette for what picture k shows, if it is not in already */
static void col_show(int k) {
	int slot, i;
	if (!col_on || pic_group[k] < 0 || pic_group[k] == pal_shown) return;
	slot = col_pal_g[0] == pic_group[k] ? 0 : col_pal_g[1] == pic_group[k] ? 1 : -1;
	if (slot < 0) return;
	for (i = 0; i < 256; i++) z_color_set_palette((uint32_t)i, col_pal[slot][i]);
	pal_shown = pic_group[k];
}

/* -- the colour window (docs/color.md, "The colour window"): 32 colours
 * in the window, on the boards that have it (the 45F ones) ----------
 *
 * The window's picture stays the black and white one, dithered; the
 * colour window adds four planes under it, which here hold each cell's
 * class -- so a pixel shows its class's dark or light colour: black or
 * the class's colour at the brightness of the desktop's ink. A
 * hand-tinted film, in 16 tints a seek point's group.
 *
 * wm's rules: the window says it wants the colour (Z_WM_COLOR_WANT,
 * only while it shows a colour film at 320x240 or less), wm says when
 * it may have it (Z_WM_COLOR), and it takes it only while it is wholly
 * uncovered. The planes are kept in main memory, a cell's class in
 * each of its pixels, and go to VRAM by the blitter: the rows that
 * changed when a picture is shown, all of them when the window moves
 * or gets the colour back. */

#define CW_W 320
#define CW_H 240
#define CW_WPL 16       /* a row (11 words used: from the word the left edge is in); a power of two, so no multiply */

static bool cw_avail;           /* the board has 32 colours in a window */
static bool cw_mode_ok(void) { return z_vmode_get() == Z_VMODE_640X480; }
static bool game_entering;
static int game_clean;          /* shown frames until the screen is wiped once more */
static void chunky_zero(void);
static bool gw_want;            /* a black and white film that could show in greys through it */
static void dec_view_apply(void);
static void gw_show(void);
static bool cw_wanted;          /* told wm we want it */
static bool cw_off;             /* F10 */
static bool cw_focus;           /* wm said we may */
static bool cw_on;              /* COLWIN is ours */
static int cw_x, cw_y, cw_w, cw_h;  /* where it was set: the picture on the screen */
static bool cw_blitting;        /* a plane blit may still be reading the planes */
static int cw_dy0 = CW_H, cw_dy1;               /* the rows changed since they were shown */
static int32_t cw_pal_shown = -2;
/* The planes in main memory, laid out as VRAM has them: picture column
 * x is bit (x + cw_a) of its row, cw_a being the window's left edge
 * within its VRAM word. A changed block's words can then be copied to
 * VRAM whole -- stores, no reads of uncached VRAM -- and a window moved
 * to another cw_a has its planes built again (cw_check()). */
static uint32_t cw_plane[4][CW_H * CW_WPL] __attribute__((aligned(4)));
/* The same a row of cells at a time: a cell's rows are all alike, so a
 * changed block is one read-modify-write a row of cells and plane here,
 * and plain stores to VRAM for its pixel rows (cw_vram_blocks()). The
 * pixel rows above are made from these only to be blitted whole
 * (cw_flush()). */
#define CW_CR (CW_H / 2)
static uint32_t cw_cplane[4][CW_CR * CW_WPL] __attribute__((aligned(4)));
static int cw_a, cw_w0;         /* cw_x & 31, and cw_x's word */

/* A block's row of cells, its bits in each plane, from the classes: for
 * classes a (left) and b (right), byte k of cw_tab[a * 16 + b] is plane
 * k's bs bits -- a's half, then b's. A block of one cell is a * 17. Made
 * for the block size in use (cw_tab_for()), so a block costs a load a
 * row of cells rather than a bit test a plane and cell. */
static uint32_t cw_tab[256];
static int cw_tab_bs;

static void cw_tab_for(int bs) {
	int a, b, k, hw = bs / 2;
	uint32_t mh = (1u << hw) - 1;
	if (cw_tab_bs == bs) return;
	for (a = 0; a < 16; a++)
		for (b = 0; b < 16; b++) {
			uint32_t v = 0;
			for (k = 0; k < 4; k++)
				v |= ((((uint32_t)a >> k) & 1 ? mh : 0) | ((((uint32_t)b >> k) & 1 ? mh : 0) << hw)) << (8 * k);
			cw_tab[a * 16 + b] = v;
		}
	cw_tab_bs = bs;
}

/* the window's block size: 8 at full size, 4 at half */
static int cw_bs(void) { return dec.view == ZV_VIEW_MONO_FULL ? 8 : 4; }

/* one block's classes into the rows of cells (cw_cplane); its first
 * word, and whether a row of it takes two */
static inline __attribute__((always_inline)) int cw_block_cells(int bx, int by, int bs, int c4, int cp,
	int cw, uint32_t m, int *wxo) {
	int k, x = bx * bs + cw_a, cr = c4 ? by << 1 : by, b = x & 31, wx = x >> 5, two = b + bs > 32;
	const uint8_t *c = col_cls + cr * cw + (c4 ? bx * 2 : bx);
	uint32_t v[2], keep = ~(m << b), keep2 = two ? ~(m >> (32 - b)) : 0;
	v[0] = c4 ? cw_tab[(c[0] << 4) + c[1]] : cw_tab[(c[0] << 4) + c[0]];
	v[1] = c4 ? cw_tab[(c[cw] << 4) + c[cw + 1]] : 0;
	for (k = 0; k < 4; k++) {
		uint32_t *p = cw_cplane[k] + cr * CW_WPL + wx;
		uint32_t b0 = (v[0] >> (8 * k)) & 0xff, b1 = (v[1] >> (8 * k)) & 0xff;
		p[0] = (p[0] & keep) | (b0 << b);
		if (two) p[1] = (p[1] & keep2) | (b0 >> (32 - b));
		if (c4) {
			p += CW_WPL;
			p[0] = (p[0] & keep) | (b1 << b);
			if (two) p[1] = (p[1] & keep2) | (b1 >> (32 - b));
		}
	}
	*wxo = wx;
	return two;
}

/* The blocks whose class changed since the planes were last shown:
 * put into the planes when the next picture is (cw_shown()), by when
 * the blitter has long finished reading them -- so a decode never
 * waits for it. */
static uint16_t cw_pend[ZV_MAX_BW * ZV_MAX_BH];
static uint8_t cw_pmark[ZV_MAX_BW * ZV_MAX_BH];
static int cw_npend;
static bool cw_all;             /* every block, rather */

static void cw_blocks(const uint16_t *list, int n) {
	int i;
	if (cw_all) return;
	for (i = 0; i < n; i++) {
		int b = (list[i] >> 8) * dec.bw + (list[i] & 0xff);
		if (cw_pmark[b]) continue;
		cw_pmark[b] = 1;
		cw_pend[cw_npend++] = list[i];
	}
}

/* all of them again (a seek, a change of view), when next shown */
static void cw_rebuild(void) {
	if (!cw_track) return;
	cw_all = true;
}

/* The listed blocks' classes into the planes, in memory and in VRAM at
 * once, by the CPU: a row of the block at a time, each plane's bits
 * into memory, and the row's one or two words of each plane stored to
 * their folded place in VRAM (z_color_window_plane_xy()) -- whole
 * words, as the memory rows are aligned as VRAM's are: no VRAM reads,
 * and the fold at column 640 falls between words. No multiply or
 * divide in the loop (picorv32's take 32 cycles and more): the folds
 * are compares, rows are steps. */
static void cw_vram_blocks(const uint16_t *list, int n) {
	int i, h, r, wx, two, bs = cw_bs(), rows1 = (int)z_color_rows() - 480, cw = colh.cw, wpl = VRAM_WPL;
	int c4 = colh.cell == 4, cp = c4 ? bs / 2 : bs, halves = c4 ? 2 : 1;
	uint32_t m = (1u << bs) - 1;
	volatile uint32_t *vbase = VRAM + 480 * wpl, *vend;
	if (rows1 <= 0) rows1 = 240;
	vend = vbase + rows1 * wpl;
	if (cw_blitting) { z_fb_hw_sync(); cw_blitting = false; }
	cw_tab_for(bs);
	for (i = 0; i < n; i++) {
		int bx = list[i] & 0xff, by = list[i] >> 8, y = by * bs;
		int ca, cb, ca2, cb2, Ya, Yb;
		const uint32_t *q;
		volatile uint32_t *da, *db;
		if (bx * bs + bs > pw || y + bs > ph) continue;
		two = cw_block_cells(bx, by, bs, c4, cp, cw, m, &wx);
		/* planes 0 and 2 at the window's column, 1 and 3 320 on */
		ca = cw_w0 + wx; if (ca >= 20) ca -= 20;
		cb = ca + 10; if (cb >= 20) cb -= 20;
		ca2 = ca + 1 >= 20 ? 0 : ca + 1;
		cb2 = cb + 1 >= 20 ? 0 : cb + 1;
		/* planes 0 and 1 at the window's row, 2 and 3 240 on */
		Ya = cw_y + y; while (Ya >= 480) Ya -= 480; while (Ya >= rows1) Ya -= rows1;
		Yb = cw_y + y + 240; while (Yb >= 480) Yb -= 480; while (Yb >= rows1) Yb -= rows1;
		da = vbase + Ya * wpl;          /* two multiplies a block */
		db = vbase + Yb * wpl;
		q = cw_cplane[0] + (c4 ? by << 1 : by) * CW_WPL + wx;
		for (h = 0; h < halves; h++, q += CW_WPL) {
			uint32_t w0 = q[0], w1 = q[CW_CR * CW_WPL], w2 = q[2 * CW_CR * CW_WPL], w3 = q[3 * CW_CR * CW_WPL];
			uint32_t x0 = 0, x1 = 0, x2 = 0, x3 = 0;
			if (two) { x0 = q[1]; x1 = q[CW_CR * CW_WPL + 1]; x2 = q[2 * CW_CR * CW_WPL + 1]; x3 = q[3 * CW_CR * CW_WPL + 1]; }
			for (r = 0; r < cp; r++) {
				da[ca] = w0; da[cb] = w1; db[ca] = w2; db[cb] = w3;
				if (two) { da[ca2] = x0; da[cb2] = x1; db[ca2] = x2; db[cb2] = x3; }
				da += wpl; if (da >= vend) da = vbase;
				db += wpl; if (db >= vend) db = vbase;
			}
		}
	}
}

/* every block's classes into the rows of cells, and those into the
 * pixel rows, for a blit of the whole picture */
static void cw_flush(void) {
	int i, k, r, cr, wx, bx, by, bs = cw_bs(), cw = colh.cw;
	int c4 = colh.cell == 4, cp = c4 ? bs / 2 : bs;
	uint32_t m = (1u << bs) - 1;
	if (cw_blitting) { z_fb_hw_sync(); cw_blitting = false; }
	cw_tab_for(bs);
	for (by = 0; by < dec.bh && (by + 1) * bs <= CW_H; by++)
		for (bx = 0; bx < dec.bw && (bx + 1) * bs <= CW_W; bx++)
			(void)cw_block_cells(bx, by, bs, c4, cp, cw, m, &wx);
	for (k = 0; k < 4; k++) {
		uint32_t *d = cw_plane[k];
		for (cr = 0; cr * cp < ph && cr < CW_CR; cr++) {
			const uint32_t *s2 = cw_cplane[k] + cr * CW_WPL;
			for (r = 0; r < cp; r++, d += CW_WPL)
				for (i = 0; i < CW_W / 32 + 1; i++) d[i] = s2[i];
		}
	}
	for (i = 0; i < cw_npend; i++) cw_pmark[(cw_pend[i] >> 8) * dec.bw + (cw_pend[i] & 0xff)] = 0;
	cw_npend = 0;
	cw_all = false;
	cw_dy0 = 0;
	cw_dy1 = ph;
}


/* rows y0..y1 of the planes to their folded places (z_color_window_plane_xy()) */
static int gw_x0, gw_x1;
static void cw_blit(int y0, int y1) {
	int k, rows1 = (int)z_color_rows() - 480;
	if (y1 > ph) y1 = ph;
	if (y0 >= y1) return;
	if (rows1 <= 0) rows1 = 240;
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	for (k = 0; k < 4; k++) {
		/* (the greys: only the columns that changed, gw_x0..gw_x1) */
		int xo = gw ? gw_x0 : 0;
		int X = (cw_x + xo + ((k & 1) ? 320 : 0)) % 640;
		int Y = ((cw_y + y0 + ((k & 2) ? 240 : 0)) % 480) % rows1;
		int w = gw ? gw_x1 - gw_x0 : pw, h = y1 - y0, w1 = w < 640 - X ? w : 640 - X, h1 = h < rows1 - Y ? h : rows1 - Y;
		/* the classes' planes, or the decoder's grey ones */
		const void *src = gw ? (const void *)((const uint8_t *)pic[0] + k * GW_PL) : (const void *)cw_plane[k];
		int ss = gw ? GW_ST : CW_WPL * 4, sx0 = gw ? xo : cw_a;
		z_fb_hw_blit_mem_async(src, ss, sx0, y0, X, 480 + Y, w1, h1, Z_ROP_COPY);
		if (w > w1) z_fb_hw_blit_mem_async(src, ss, sx0 + w1, y0, 0, 480 + Y, w - w1, h1, Z_ROP_COPY);
		if (h > h1) {
			z_fb_hw_blit_mem_async(src, ss, sx0, y0 + h1, X, 480, w1, h - h1, Z_ROP_COPY);
			if (w > w1) z_fb_hw_blit_mem_async(src, ss, sx0 + w1, y0 + h1, 0, 480, w - w1, h - h1, Z_ROP_COPY);
		}
	}
	cw_blitting = true;
}

/* the 32 colours for picture k's group: class c, desktop bit b, at
 * entry b | c << 4; class 0 the desktop's own black and ink */
static void cw_palette(int k) {
	uint16_t acc[4];
	int slot, c, lv;
	uint32_t ink;
	if (!cw_on || pic_group[k] == cw_pal_shown) return;
	slot = col_pal_g[0] == pic_group[k] ? 0 : col_pal_g[1] == pic_group[k] ? 1 : -1;
	z_accent_palette(z_video_get_mode(), z_video_get_brightness(), false, acc);
	ink = acc[Z_ACCENT_INK];
	/* the level of the ink's brightness, of the 16 the palette's classes have */
	lv = (int)((2126 * ((ink >> 8) & 15) + 7152 * ((ink >> 4) & 15) + 722 * (ink & 15) + 5000) / 10000);
	z_color_set_palette(0, acc[Z_ACCENT_BLACK]);
	z_color_set_palette(1, ink);
	for (c = 1; c < 16; c++) {
		z_color_set_palette((uint32_t)(c << 4), acc[Z_ACCENT_BLACK]);
		z_color_set_palette((uint32_t)(c << 4 | 1), slot >= 0 ? col_pal[slot][c * 16 + lv] : ink);
	}
	cw_pal_shown = pic_group[k];
}

/* The greys window's 32 entries: grey c for a pixel whose planes say c,
 * whatever the desktop's own bit under it -- in the desktop's phosphor
 * (amber, green), at full brightness: the film's white is the brightest
 * the display has. */
static void gw_palette(void) {
	uint16_t acc[4];
	uint32_t ink, r, g, b, mx;
	int c;
	z_accent_palette(z_video_get_mode(), z_video_get_brightness(), false, acc);
	ink = acc[Z_ACCENT_INK];
	r = (ink >> 8) & 15; g = (ink >> 4) & 15; b = ink & 15;
	mx = r > g ? r : g; if (b > mx) mx = b; if (!mx) { r = g = b = mx = 1; }
	for (c = 0; c < 16; c++) {
		uint32_t v = ((((uint32_t)c * r + mx / 2) / mx) << 8) | ((((uint32_t)c * g + mx / 2) / mx) << 4) |
		             (((uint32_t)c * b + mx / 2) / mx);
		z_color_set_palette((uint32_t)(c << 4), v);
		z_color_set_palette((uint32_t)(c << 4 | 1), v);
	}
}

/* the planes' changed rows to the screen (all of them after a seek, a
 * move, or when the window is taken) */
static void gw_show(void) {
	int bs = dec.view == ZV_VIEW_PL_FULL ? 8 : 4, by, bx, y0 = -1, y1 = 0;
	if (!cw_on) return;
	/* the box the changed blocks are in: the blitter's time is the
	 * words it moves, four planes of them (on Sergei ML2 a whole
	 * 320x240 plane is 90k cycles) */
	gw_x0 = 0; gw_x1 = pw;
	if (gw_all) { y0 = 0; y1 = ph; }
	else {
		int xa = dec.bw, xz = -1;
		for (by = 0; by < dec.bh; by++) {
			const uint8_t *m = dmap_acc + by * dec.bw;
			/* (only as far in from each side as the box does not reach yet) */
			for (bx = 0; bx < dec.bw && !m[bx]; bx++) { }
			if (bx == dec.bw) continue;
			if (bx < xa) xa = bx;
			for (bx = dec.bw - 1; bx > xz && !m[bx]; bx--) { }
			if (bx > xz) xz = bx;
			if (y0 < 0) y0 = by * bs;
			y1 = by * bs + bs;
		}
		if (xz >= 0) { gw_x0 = xa * bs; gw_x1 = (xz + 1) * bs; if (gw_x1 > pw) gw_x1 = pw; }
	}
	memset(dmap_acc, 0, sizeof(dmap_acc));
	gw_all = false;
	if (y0 >= 0) cw_blit(y0, y1);
}

static bool gw_fix;             /* the decoder's view is to be put right (cw_check()) */

/* Our planes out of the second sheet: whoever has it next (wm, for the
 * desktop's tints) would show what is left of them as colour where the
 * picture was. */
static void cw_clear(void) {
	int k, rows1 = (int)z_color_rows() - 480;
	if (!cw_mode_ok()) return;      /* those rows are the screen's now, and wm's to paint */
	if (rows1 <= 0) rows1 = 240;
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	for (k = 0; k < 4; k++) {
		int X = (cw_x + ((k & 1) ? 320 : 0)) % 640;
		int Y = ((cw_y + ((k & 2) ? 240 : 0)) % 480) % rows1;
		int w1 = cw_w < 640 - X ? cw_w : 640 - X, h1 = cw_h < rows1 - Y ? cw_h : rows1 - Y;
		z_fb_hw_fill_rect(X, 480 + Y, w1, h1, 0);
		if (cw_w > w1) z_fb_hw_fill_rect(0, 480 + Y, cw_w - w1, h1, 0);
		if (cw_h > h1) {
			z_fb_hw_fill_rect(X, 480, w1, cw_h - h1, 0);
			if (cw_w > w1) z_fb_hw_fill_rect(0, 480, cw_w - w1, cw_h - h1, 0);
		}
	}
	z_fb_hw_sync();
}

static void cw_drop(void) {
	if (gw) { gw = false; gw_fix = true; }
	if (!cw_on) return;
	if (cw_focus) z_color_window_off();
	if (cw_blitting) { z_fb_hw_sync(); cw_blitting = false; }
	cw_clear();
	z_fb_set_rows(480);
	cw_on = false;
}

/* tell wm whether we want it: a colour film, in the window, small enough */
static void cw_want_update(void) {
	/* a film in colour: its classes; one in black and white: its greys */
	/* (640x480 only, asked each time: the resolution can change under an
	 * open window, and at another the second sheet's rows are the
	 * bottom of the screen -- the planes showed there) */
	bool fits = cw_avail && cw_mode_ok() && have_file && !cw_off && out_kind == OUT_WINDOW && !game_entering &&
	            (view == ZV_VIEW_MONO_HALF || view == ZV_VIEW_MONO_FULL) && pw <= CW_W && ph <= CW_H;
	bool want = fits && col_have;
	uint32_t wm;
	/* (at full size only: the half plane view writes sixteen bytes a
	 * block where the mono one writes four, and a 640-wide film has no
	 * such time to spare on a 48 MHz board -- it stays dithered) */
	gw_want = fits && !col_have && view == ZV_VIEW_MONO_FULL;
	if (want && !cw_track) cw_all = true;    /* the planes from scratch */
	cw_track = want;
	if (!want && !gw_want) cw_drop();
	want = want || gw_want;
	if (want == cw_wanted || win.id < 0) return;
	cw_wanted = want;
	if (z_pid_lookup("wm0", &wm)) z_msg_new_send(wm, Z_WM_COLOR_WANT, (uint32_t)win.id, z_obj_uint32(want ? 1u : 0u));
}

/* once a pass: take the colour window, or give it back */
static void cw_check(void) {
	z_clip_t c, v;
	int px, py, n;
	bool ok;
	if ((cw_on || cw_wanted) && !cw_mode_ok()) cw_want_update();   /* the resolution changed */
	else if (cw_avail && !cw_wanted && cw_mode_ok() && have_file) cw_want_update();    /* or changed back */
	if (gw_fix) {
		gw_fix = false;
		if (have_file && dec.view != dec_view()) dec_view_apply();     /* back to black and white */
	}
	if (!cw_track && !gw_want) return;
	ok = cw_focus && !z_win_frozen(&win) && !z_game_enabled();
	if (ok) {
		z_win_content_rect(&win, &c);           /* loads the region */
		pic_place(&px, &py);
		n = z_gfx_visible_count();
		ok = px >= 0 && py >= 0 && (n == 0 ||
		     (n == 1 && z_gfx_visible_clip(0, &c, &v) && v.x0 == c.x0 && v.y0 == c.y0 && v.x1 == c.x1 && v.y1 == c.y1));
		if (ok && (!cw_on || cw_x != c.x0 + px || cw_y != c.y0 + py || cw_w != pw || cw_h != ph ||
		           !z_color_window_on())) {
			cw_x = c.x0 + px;
			cw_y = c.y0 + py;
			if ((cw_x & 31) != cw_a) cw_all = true;    /* the planes, aligned again */
			cw_a = cw_x & 31;
			cw_w0 = cw_x >> 5;
			cw_w = pw;
			cw_h = ph;
			z_fb_set_rows((int)z_color_rows());
			if (gw_want) {
				cw_on = true;
				gw_all = true;
				if (!gw) { gw = true; dec_view_apply(); }      /* decodes, and shows the planes */
				else gw_show();
				gw_palette();
				z_fb_hw_sync();
				cw_blitting = false;
				z_color_window_set(cw_x, cw_y, cw_x + pw, cw_y + ph);
			} else {
				cw_flush();
				cw_blit(0, ph);
				z_color_window_set(cw_x, cw_y, cw_x + pw, cw_y + ph);
				cw_on = true;
				cw_pal_shown = -2;
				cw_palette(front);
				cw_dy0 = CW_H; cw_dy1 = 0;
			}
		}
	}
	if (!ok) cw_drop();
}

/* a picture shown in the window: its planes' changed rows, its palette */
static void cw_shown(int k) {
	bool few;
	if (!cw_track) return;
	/* a frame's changes, by the CPU, a table load a row of cells
	 * (cw_tab); a fifth of the picture or more (a seek, a group's
	 * first frame of new classes), by the blitter, the rows they are in */
	if (!cw_on) {
		/* nothing to keep up: all of it again when it is taken */
		int i2;
		for (i2 = 0; i2 < cw_npend; i2++) cw_pmark[(cw_pend[i2] >> 8) * dec.bw + (cw_pend[i2] & 0xff)] = 0;
		cw_npend = 0;
		cw_all = true;
		return;
	}
	few = !cw_all && cw_npend * 5 <= dec.bw * dec.bh;
	if (few) {
		int n = cw_npend;
		int i2;
		for (i2 = 0; i2 < n; i2++) cw_pmark[(cw_pend[i2] >> 8) * dec.bw + (cw_pend[i2] & 0xff)] = 0;
		cw_vram_blocks(cw_pend, n);       /* the planes in memory too */
		cw_npend = 0;
		cw_dy0 = CW_H; cw_dy1 = 0;
		cw_palette(k);
		return;
	}
	cw_flush();
	cw_blit(0, ph);
	cw_dy0 = CW_H; cw_dy1 = 0;
	cw_palette(k);
}

static int32_t last_key = -1;   /* the last seek point decoded */
/* Blocks a call of the decoder while playing. A third of a 320x240
 * picture: an ordinary frame, which changes fewer, is still one call --
 * each extra pass of the loop cost more on Sergei ML2 than it looked
 * (frames were shown late and skipped at 128) -- and a cut or a seek
 * point is three or four. */
#define DEC_STEP 400
static bool dec_part;           /* a frame is part decoded */
/* decode the record at the head of the queue into picture `k` */
static bool decode_head(int k) {
	rq_t *r = rq_first_video();
	zv_rec_t rec;
	int rv;
	if (!r || !r->sound) return false;
	if (zv_split_record(&hd, rqbuf + r->off, r->size, &rec)) { rq_pop(); return false; }
	{
		/* A piece of the frame a call (DEC_STEP blocks: about 5 ms on a
		 * board), and back to the loop between them, so a frame that
		 * is every block -- a cut, a seek point: 50 ms and more -- does
		 * not keep the window from answering (docs/responsive.md). The
		 * record stays at the queue's head until its frame is whole. */
		uint32_t t = z_cycles();
		if (!dec_part) {
			col_before_decode(r->frame);    /* (a decode's cost, colour included) */
			if (rec.flags & ZV_RF_KEY) last_key = (int32_t)r->frame;
		}
		rv = zv_dec_video(&dec, rec.video, rec.video_len, rec.flags & ZV_RF_KEY, pic_out(k), pstride);
		dec_part = rv == 2;
		if (!dec_part) col_after_decode(k);
		t = z_cycles() - t;
		st_dec_cycles += t;
		if (t > st_dec_worst) st_dec_worst = t;     /* the longest the loop was away */
		if (dec_part) return false;
		st_dec_n++;
		if (t > st_dec_max) st_dec_max = t;
	}
	if (rv < 0) dbg("cinema: a damaged frame\n");
	back_frame = (int32_t)r->frame;
	rq_pop();
	return true;
}

static void copy_words(uint32_t *d, const uint32_t *s, uint32_t n) {
	while (n >= 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; s += 4; n -= 4; }
	while (n--) *d++ = *s++;
}

/* the next frame into the back picture, if its record is here */
static bool prepare_back(void) {
	int b = front ^ 1;
	if (back_frame >= 0) return true;
	if (dec_part) return decode_head(b);            /* on with the frame begun */
	if (!rq_first_video() || !rq_first_video()->sound) return false;
	if (chunky_out()) {
		/* the back may still be on screen until a flip is taken */
		if (flip_wait) {
			if ((uint16_t)z_game_frame() == flip_fc) return false;
			flip_wait = false;
		}
		/* the blitter may still be reading the picture to be written */
		if (out_kind == OUT_CHUNKY_VRAM && use_hw_blit) z_fb_hw_sync();
		if (sync_due) { copy_blocks(dmap_sync, front, b); sync_due = false; }
	} else if (cw_blitting) {
		/* the blitter may still be reading the planes */
		z_fb_hw_sync();
		cw_blitting = false;
	}
	return decode_head(b);
}

/* -- the window ---------------------------------------------------------- */

static void blit_sw(const uint32_t *src, int src_x, int src_y, int dst_x, int dst_y, int w, int h) {
	int j;
	for (j = 0; j < h; j++) {
		volatile uint32_t *drow = &VRAM[(dst_y + j) * VRAM_WPL];
		const uint32_t *srow = src + (src_y + j) * PIC_WPL;
		int dx0 = dst_x, dx1 = dst_x + w, w0 = dx0 >> 5, w1 = (dx1 - 1) >> 5, wi;
		for (wi = w0; wi <= w1; wi++) {
			int first = wi * 32, sx0 = src_x + (first - dst_x), sw = sx0 >> 5, s = sx0 & 31;
			uint32_t lo = (sw >= 0 && sw < PIC_WPL) ? srow[sw] : 0;
			uint32_t hi = (sw + 1 >= 0 && sw + 1 < PIC_WPL) ? srow[sw + 1] : 0;
			uint32_t val = s ? ((lo >> s) | (hi << (32 - s))) : lo;
			uint32_t mask = 0xFFFFFFFFu;
			if (dx0 > first) mask &= 0xFFFFFFFFu << (dx0 - first);
			if (dx1 < first + 32) mask &= 0xFFFFFFFFu >> (32 - (dx1 - first));
			drow[wi] = (drow[wi] & ~mask) | (val & mask);
		}
	}
}

/* where the picture sits in the window's content, centred (a little
 * past its edges when it is a few pixels bigger: full screen keeps the
 * window's 2-pixel frame, so a 640-wide film loses 2 pixels a side) */
static void pic_place(int *x, int *y) {
	*x = (z_win_content_w(&win) - pw) / 2;
	*y = (z_win_content_h(&win) - bar_h() - ph) / 2;
}

/* the front picture to the screen, whole (after a seek, a change of view) */
static void show_front_window(void);
static void show_front(void) {
	int y;
	if (!have_file) return;
	switch (out_kind) {
	case OUT_GAME_MONO:
		if (!use_hw_blit || !z_fb_hw_blit_mem(pic[0], PIC_WPL * 4, 0, 0,
		                                      (Z_CHUNKY_W - pw) / 2, (Z_CHUNKY_H - ph) / 2, pw, ph))
			blit_sw(pic[0], 0, 0, (Z_CHUNKY_W - pw) / 2, (Z_CHUNKY_H - ph) / 2, pw, ph);
		return;
	case OUT_CHUNKY_VRAM:
		for (y = 0; y < Z_CHUNKY_H; y += 8) z_chunky_put_rows(pic[0], Z_CHUNKY_W, (uint32_t)y, 8);
		return;
	case OUT_CHUNKY_MEM:
		z_chunky_show(pic[front]);
		flip_wait = true;
		flip_fc = (uint16_t)z_game_frame();
		return;
	default:
		show_front_window();
		return;
	}
}

/* chunky VRAM by the blitter: a segment's rows are framebuffer rows
 * (row r of segment s at r + 240 (s & 1) + 480 (s >> 1)), so four lines
 * of a segment are a 640x4 one-bit rectangle, and a byte a pixel copies
 * as eight bits a pixel. For each block row with changes, one blit for
 * each segment's span of changed blocks, started and not waited for;
 * the CPU only programs them. Without the blitter's memory source: the
 * CPU copies the blocks. */
static void blit_changed(const uint8_t *map, int k) {
	int bw = dec.bw, bh = dec.bh, by, s2, bs = pic_bs;
	(void)k;
	/* A segment at a time, down the picture: block rows with changes
	 * that follow one another go as one blit, over the widest of their
	 * spans. A blit a row and segment was up to 120 calls a frame, and
	 * the calls cost more than the words (zsys, whose blitter takes no
	 * time, still spent 260k cycles a frame here). */
	for (s2 = 0; s2 < 4; s2++) {
		int lo = s2 * Z_CHUNKY_SEG_PX - pic_ox, bx0 = lo > 0 ? (lo + bs - 1) / bs : 0, bx1, bx;
		int ra = -1, rz = -1, a = 0, z = 0;     /* the rows gathered, and their span */
		bx1 = ((s2 + 1) * Z_CHUNKY_SEG_PX - pic_ox) / bs - 1;
		if (bx0 < 0) bx0 = 0;
		if (bx1 >= bw) bx1 = bw - 1;
		if (bx0 > bx1) continue;
		for (by = 0; by <= bh; by++) {
			int ca = -1, cz = -1;
			if (by < bh) {
				const uint8_t *m = map + by * bw;
				for (bx = bx0; bx <= bx1; bx++) if (m[bx]) { if (ca < 0) ca = bx; cz = bx; }
			}
			if (ca >= 0) {
				if (ra < 0) { ra = by; a = ca; z = cz; }
				else { if (ca < a) a = ca; if (cz > z) z = cz; }
				rz = by;
				continue;
			}
			if (ra >= 0) {
				int px = pic_ox + a * bs;               /* chunky pixel */
				int r0 = pic_oy + ra * bs;
				int dy = r0 + ((s2 & 1) ? 240 : 0) + ((s2 & 2) ? 480 : 0);
				z_fb_hw_blit_mem_async(pic[0], Z_CHUNKY_W, px * 8, r0,
				                       (px - s2 * Z_CHUNKY_SEG_PX) * 8, dy, (z - a + 1) * bs * 8, (rz - ra + 1) * bs, Z_ROP_COPY);
				ra = -1;
			}
		}
	}
}

/* the next picture on: the back becomes the front */
static void swap_pictures(void) {
	int b = front ^ 1;
	col_show(b);
	switch (out_kind) {
	case OUT_CHUNKY_VRAM:
		if (use_hw_blit) blit_changed(dmap_acc, b);
		else copy_blocks(dmap_acc, b, -1);
		break;
	case OUT_CHUNKY_MEM:
		z_chunky_show(pic[b]);
		flip_wait = true;
		flip_fc = (uint16_t)z_game_frame();
		break;
	default:
		break;
	}
	front = b;
	if (chunky_out()) {
		/* the new back (the old front) lacks what the new front got */
		memcpy(dmap_sync, dmap_acc, sizeof(dmap_sync));
		memset(dmap_acc, 0, sizeof(dmap_acc));
		sync_due = out_kind == OUT_CHUNKY_MEM;
	} else if (out_kind == OUT_GAME_MONO) {
		show_front();
	} else {
		show_front_window();
		cw_shown(front);
	}
}

/* the window's: clipped to what of it shows */
static void show_front_window(void) {
	z_clip_t c;
	int px, py, sx = 0, sy = 0, dx, dy, w, h;
	if (!have_file || out_kind != OUT_WINDOW) return;
	z_win_content_rect(&win, &c);
	pic_place(&px, &py);
	dx = c.x0 + px;
	dy = c.y0 + py;
	w = pw;
	h = ph;
	if (dx < c.x0) { sx = c.x0 - dx; w -= sx; dx = c.x0; }
	if (dy < c.y0) { sy = c.y0 - dy; h -= sy; dy = c.y0; }
	if (dx + w - 1 > c.x1) w = c.x1 - dx + 1;
	if (dy + h - 1 > c.y1) h = c.y1 - dy + 1;
	if (w <= 0 || h <= 0) return;
	if (gw) { gw_show(); return; }
	if (!use_hw_blit || !z_fb_hw_blit_mem(pic[0], PIC_WPL * 4, sx, sy, dx, dy, w, h))
		blit_sw(pic[0], sx, sy, dx, dy, w, h);
}


/* -- the controls: a bar under the picture (c) ---------------------------
 *
 * Play/pause, loop, a timeline to click or drag, the time. Drawn into a
 * bitmap in memory and put on the screen whole, by the same blit as the
 * picture: nothing is cleared on the screen first, so nothing flashes.
 * It is drawn again only when what it shows has changed -- the state,
 * the timeline by a pixel, the time by a second: two or three blits of
 * 14 rows a second at most, a few thousand cycles. */
#define BAR_H       14
#define BAR_BTN     16          /* a button's width */
#define BAR_TIME_W  88          /* "1:23:45 / 1:30:00" at 5 pixels */
static bool bar_on = true;
static uint32_t bar_bm[BAR_H * PIC_WPL];
static uint32_t bar_key = 0xffffffffu, bar_key2;   /* what the bar on the screen shows */
static bool bar_drag;           /* the timeline's knob is held */
static int32_t bar_drag_f;      /* and where */

static int bar_h(void) { return bar_on && out_kind == OUT_WINDOW ? BAR_H : 0; }

static void bar_fill(int x0, int y0, int w, int h, int on) {
	int x, y;
	for (y = y0; y < y0 + h; y++) {
		if (y < 0 || y >= BAR_H) continue;
		for (x = x0; x < x0 + w; x++) {
			if (x < 0 || x >= PIC_WPL * 32) continue;
			if (on) bar_bm[y * PIC_WPL + (x >> 5)] |= 1u << (x & 31);
			else bar_bm[y * PIC_WPL + (x >> 5)] &= ~(1u << (x & 31));
		}
	}
}

static int bar_text(int x, int y, const char *t) {
	const z_font_t *f = &z_font_5x8;
	for (; *t; t++, x += f->w) {
		int g = z_font_index(f, (uint8_t)*t), r, c;
		if (g < 0) continue;
		for (r = 0; r < f->h; r++) {
			uint8_t bits = f->glyphs[g * f->h + r];
			for (c = 0; c < f->w; c++) if (bits & (0x80 >> c)) bar_fill(x + c, y + r, 1, 1, 1);
		}
	}
	return x;
}

/* m:ss, or h:mm:ss */
static int bar_time(char *b, uint32_t secs) {
	int i = 0;
	uint32_t h = secs / 3600, m = (secs / 60) % 60, s2 = secs % 60;
	if (h) { b[i++] = (char)('0' + h % 10); b[i++] = ':'; b[i++] = (char)('0' + m / 10); }
	else if (m >= 10) b[i++] = (char)('0' + m / 10);
	b[i++] = (char)('0' + m % 10);
	b[i++] = ':';
	b[i++] = (char)('0' + s2 / 10);
	b[i++] = (char)('0' + s2 % 10);
	b[i] = 0;
	return i;
}

/* the timeline's place in the bar, for a content width */
static void bar_track(int cw, int *x0, int *w) {
	*x0 = 2 * BAR_BTN + 6;
	*w = cw - *x0 - BAR_TIME_W - 6;
	if (*w < 16) *w = cw - *x0 - 4;     /* a narrow window: no time */
}

static uint32_t bar_frame(void) {
	if (bar_drag) return (uint32_t)bar_drag_f;
	if (seek_want >= 0) return (uint32_t)seek_want;
	return (uint32_t)(front_frame > 0 ? front_frame : 0);
}

static void bar_show(bool force) {
	z_clip_t c;
	int cw, tx, tw, knob, i, x, y;
	uint32_t f, secs, key, key2;
	char t[24];
	if (!bar_h() || win.id < 0) return;
	cw = z_win_content_w(&win);
	if (cw > PIC_WPL * 32) cw = PIC_WPL * 32;
	bar_track(cw, &tx, &tw);
	f = have_file ? bar_frame() : 0;
	knob = have_file && hd.frames > 1 ? (int)((uint64_t)f * (uint32_t)(tw - 3) / (hd.frames - 1)) : 0;
	secs = have_file ? (uint32_t)((uint64_t)f * hd.fps_den / hd.fps_num) : 0;
	key = (uint32_t)knob | (paused ? 1u << 16 : 0) | (looping ? 1u << 17 : 0) | (have_file ? 1u << 18 : 0) |
	      ((uint32_t)cw << 20);
	key2 = secs;
	if (!force && key == bar_key && key2 == bar_key2) return;
	bar_key = key; bar_key2 = key2;
	memset(bar_bm, 0, sizeof(bar_bm));
	bar_fill(0, 0, cw, 1, 1);                           /* a line under the picture */
	/* play (a triangle) or pause (two bars): what a click will do is
	 * the other, as on any player -- the sign shows the state it is in */
	if (paused || !have_file) for (i = 0; i < 4; i++) bar_fill(5 + i, 4 + i, 1, 8 - 2 * i, 1);
	else { bar_fill(5, 4, 2, 7, 1); bar_fill(9, 4, 2, 7, 1); }
	/* loop: a ring with a gap, filled in when on */
	x = BAR_BTN + 3; y = 4;
	if (looping) bar_fill(x - 2, y - 2, 13, 11, 1);
	bar_fill(x + 1, y, 7, 1, !looping); bar_fill(x + 1, y + 6, 7, 1, !looping);
	bar_fill(x, y + 1, 1, 5, !looping); bar_fill(x + 8, y + 1, 1, 5, !looping);
	bar_fill(x + 3, y - 1, 3, 3, looping);              /* the gap, and its arrow head */
	bar_fill(x + 5, y, 1, 1, !looping); bar_fill(x + 4, y - 1, 1, 1, !looping); bar_fill(x + 4, y + 1, 1, 1, !looping);
	/* the timeline: a track, what has played filled, a knob */
	bar_fill(tx, 6, tw, 1, 1); bar_fill(tx, 9, tw, 1, 1);
	bar_fill(tx, 6, 1, 4, 1); bar_fill(tx + tw - 1, 6, 1, 4, 1);
	bar_fill(tx, 7, knob + 1, 2, 1);
	bar_fill(tx + knob, 3, 3, 10, 1);
	if (have_file && tw >= 16 && cw - tx - tw >= BAR_TIME_W) {
		i = bar_time(t, secs);
		t[i++] = ' '; t[i++] = '/'; t[i++] = ' ';
		i += bar_time(t + i, (uint32_t)((uint64_t)hd.frames * hd.fps_den / hd.fps_num));
		bar_text(cw - 2 - i * 5, 4, t);
	}
	z_win_content_rect(&win, &c);                       /* loads the region */
	{
		int dx = c.x0, dy = c.y1 - BAR_H + 1, w = cw;
		if (dx + w - 1 > c.x1) w = c.x1 - dx + 1;
		if (w <= 0 || dy < c.y0) return;
		if (!use_hw_blit || !z_fb_hw_blit_mem(bar_bm, PIC_WPL * 4, 0, 0, dx, dy, w, BAR_H))
			blit_sw(bar_bm, 0, 0, dx, dy, w, BAR_H);
	}
}

static void bar_update(void) { bar_show(false); }

/* a mouse sample: the buttons, and the timeline (a click jumps; held,
 * the knob follows and the film jumps when it is let go) */
static void toggle_pause(void);
static void bar_mouse(uint32_t packed) {
	static uint8_t last;
	int cx, cy, tx, tw, cw, ch;
	bool inside = z_win_mouse_content_xy(&win, packed, &cx, &cy);
	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (buttons & Z_MOUSE_BTN_LEFT) && !(last & Z_MOUSE_BTN_LEFT);
	bool up = !(buttons & Z_MOUSE_BTN_LEFT) && (last & Z_MOUSE_BTN_LEFT);
	last = buttons;
	if (!bar_h() || !have_file) { bar_drag = false; return; }
	cw = z_win_content_w(&win); ch = z_win_content_h(&win);
	bar_track(cw, &tx, &tw);
	if (bar_drag) {
		int k = cx - tx - 1;
		if (k < 0) k = 0;
		if (k > tw - 3) k = tw - 3;
		bar_drag_f = (int32_t)((uint64_t)k * (hd.frames - 1) / (uint32_t)(tw - 3));
		if (up || !(buttons & Z_MOUSE_BTN_LEFT)) {
			bar_drag = false;
			seek_ask(bar_drag_f, 0);
		}
		return;
	}
	if (!down || !inside || cy < ch - BAR_H) return;
	if (cx < BAR_BTN) toggle_pause();
	else if (cx < 2 * BAR_BTN + 3) looping = !looping;
	else if (cx >= tx - 2 && cx < tx + tw + 2 && tw > 3) {
		int k = cx - tx - 1;
		if (k < 0) k = 0;
		if (k > tw - 3) k = tw - 3;
		bar_drag = true;
		bar_drag_f = (int32_t)((uint64_t)k * (hd.frames - 1) / (uint32_t)(tw - 3));
	}
}

static void repaint(void);
static void bar_toggle(void) {
	int snug = have_file && z_win_content_w(&win) == pw && z_win_content_h(&win) == ph + bar_h();
	bar_on = !bar_on;
	if (out_kind != OUT_WINDOW) return;
	/* a window that was just the film's size stays so */
	if (snug) z_win_resize(&win, (uint32_t)(pw + Z_WIN_CHROME_W), (uint32_t)(ph + bar_h() + Z_WIN_CHROME_H));
	if (have_file) set_view(view_for_window());
	repaint();
}

static void repaint(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win) - bar_h(), px, py;
	if (out_kind != OUT_WINDOW) return;         /* the screen is ours: wm's desktop is not shown */
	if (!have_file) {
		z_win_fill_rect(&win, 0, 0, cw, ch, 0);
		z_win_draw_text(&win, 8, 8, "No film.\n\nOpen one with the icon\nin the title bar (or o),\nor from a web server (u).", 1, &z_font_5x8);
		bar_show(true);
		return;
	}
	pic_place(&px, &py);
	/* black around the picture */
	if (py > 0) z_win_fill_rect(&win, 0, 0, cw, py, 0);
	if (py + ph < ch) z_win_fill_rect(&win, 0, py + ph, cw, ch - py - ph, 0);
	if (px > 0) z_win_fill_rect(&win, 0, py > 0 ? py : 0, px, ph, 0);
	if (px + pw < cw) z_win_fill_rect(&win, px + pw, py > 0 ? py : 0, cw - px - pw, ph, 0);
	show_front_window();
	bar_show(true);
}

static const char *basename_of(const char *p) {
	const char *b = p;
	for (; *p; p++) if (*p == '/') b = p + 1;
	return b;
}

static void update_title(void) {
	const char *n = have_file ? basename_of(filename) : "";
	int i = 0;
	const char *a = "cinema";
	while (*a && i < (int)sizeof(title_buf) - 1) title_buf[i++] = *a++;
	if (*n) {
		const char *sep = " - ";
		while (*sep && i < (int)sizeof(title_buf) - 1) title_buf[i++] = *sep++;
		while (*n && i < (int)sizeof(title_buf) - 1) title_buf[i++] = *n++;
	}
	/* (No running time: wm redraws a title bar it is given by clearing
	 * it first, and once a second that showed as a black flash.) */
	if (have_file && net_src && (net_failed || net_stall)) {
		const char *pz = net_failed ? " (stopped: " : " (buffering)";
		while (*pz && i < (int)sizeof(title_buf) - 1) title_buf[i++] = *pz++;
		if (net_failed) {
			pz = ns_error();
			while (*pz && i < (int)sizeof(title_buf) - 2) title_buf[i++] = *pz++;
			title_buf[i++] = ')';
		}
	} else if (paused && have_file) {
		const char *pz = " (paused)";
		while (*pz && i < (int)sizeof(title_buf) - 1) title_buf[i++] = *pz++;
	}
	title_buf[i] = 0;
	z_win_set_title(&win, title_buf);
}

/* the view for the window's size: full when the film fits whole */
static int view_for_window(void) {
	if (!have_file) return ZV_VIEW_MONO_HALF;
	if (z_win_content_w(&win) >= hd.w - 8 && z_win_content_h(&win) - bar_h() >= hd.h - 8) return ZV_VIEW_MONO_FULL;
	return ZV_VIEW_MONO_HALF;
}

/* -- playing ---------------------------------------------------------- */

/* to frame f: from the seek point before it, quietly, then the sound
 * from f on */
static void seek_to(uint32_t f) {
	uint32_t start, i, koff = ZV_HDR_SIZE, kf = 0;
	if (!have_file) return;
	if (f >= hd.frames) f = hd.frames - 1;
	mixer_off();
	rq_flush();
	dec_part = false;
	dec.step_blocks = 0;            /* whole frames here; a piece at a time again below */
	start = zv_start_for(&hd, f);
	for (i = 0; i < nkeys; i++)
		if (keys[i * 2] <= start) { kf = keys[i * 2]; koff = keys[i * 2 + 1]; }
	if (net_src) {
		/* (the records end where the index starts: no asking past them) */
		ns_stream_seek(koff, hd.index_off > koff ? hd.index_off : 0);
		net_wait = true;            /* the frames up to f are waited for */
		if (net_stall) { net_stall = false; }
	} else file_seek(koff);
	rd_pos = koff;
	next_read = kf;
	zv_dec_seek(&dec, kf);
	memset(pic, 0, sizeof(pic));
	front = 0;
	front_frame = back_frame = -1;
	mix_head = 0;
	/* quietly up to f: these frames' sound is not wanted */
	while (front_frame < (int32_t)f) {
		rq_t *r;
		if (!rq_n && !rq_read()) break;
		r = rq_first_video();
		r->sound = true;
		if (r->frame < f) {
			decode_head(front);
			front_frame = back_frame;
			back_frame = -1;
		} else break;
	}
	/* frame f itself: its sound is the first in the ring */
	if (rq_n) {
		rq_t *r = rq_first_video();
		r->sound = !sound;
		if (r->sound || sound_put(r)) {
			r->sound = true;
			decode_head(front);
			front_frame = back_frame;
			back_frame = -1;
		}
	}
	clock_frame = (uint32_t)(front_frame >= 0 ? front_frame : (int32_t)f);
	/* the back as the front, whole; nothing outstanding */
	if (out_kind == OUT_CHUNKY_MEM) copy_words(pic[front ^ 1], pic[front], PIC_WORDS);
	gw_all = true;
	pic_group[front ^ 1] = pic_group[front];
	col_show(front);
	if (cw_track) { cw_rebuild(); if (cw_on) { cw_dy0 = 0; cw_dy1 = ph; } }
	memset(dmap_acc, 0, sizeof(dmap_acc));
	sync_due = false;
	flip_wait = false;
	net_wait = false;
	feed();
	vs_start = zv_frame_vsync(&hd, clock_frame, 60);
	fc_start = (uint16_t)z_game_frame();
	last_fc = fc_start;
	if (!paused) mixer_start();
	show_front();
	cw_shown(front);
	update_title();
	dec.step_blocks = DEC_STEP;
}

static void set_view(int v) {
	if (v == view && dec.view == dec_view() && !chunky_out()) { view_layout(); cw_want_update(); return; }
	view = v;
	cw_drop();                  /* (the greys window: taken again by cw_check()) */
	gw_fix = false;
	zv_dec_set_view(&dec, v, ZV_MASK_VAC8);
	view_layout();
	cw_want_update();
	if (have_file) seek_to(front_frame >= 0 ? (uint32_t)front_frame : 0);
}

/* The decoder into dec_view(): between the window's black and white
 * and its greys, with no seek -- the picture is turned into the other
 * layout as it stands and the film carries on (taking or losing the
 * colour window is a click on another window: it may not cost a
 * second of decoding). Into greys the dithered picture comes as it was
 * and turns grey as its blocks are drawn, within a seek point's
 * refresh. */
static void dec_view_apply(void) {
	int to = dec_view(), words = (pw + 31) / 32;
	bool to_pl = to == ZV_VIEW_PL_FULL || to == ZV_VIEW_PL_HALF;
	/* (just after a seek point -- a film's start above all -- the few
	 * frames since are decoded again instead: quick, and the greys are
	 * whole at once rather than arriving over two seconds) */
	if (have_file && to != dec.view && ph <= 240 && words * 4 <= GW_ST &&
	    !(to_pl && last_key >= 0 && front_frame - last_key <= 8)) {
		if (to_pl) zv_mono_to_planes(pic[0], PIC_WPL * 4, pic[1], GW_ST, GW_PL, words, ph);
		else zv_planes_to_mono(pic[0], GW_ST, GW_PL, pic[1], PIC_WPL * 4, words, ph, ZV_MASK_VAC8);
		copy_words(pic[0], pic[1], to_pl ? (uint32_t)(GW_PL * 4 / 4) : (uint32_t)(PIC_WPL * ph));
		zv_dec_set_view(&dec, to, ZV_MASK_VAC8);
		view_layout();
		gw_all = true;
		memset(dmap_acc, 0, sizeof(dmap_acc));
		show_front_window();
		return;
	}
	zv_dec_set_view(&dec, to, ZV_MASK_VAC8);
	view_layout();
	if (have_file) seek_to(front_frame >= 0 ? (uint32_t)front_frame : 0);
}

static void close_file(void) {
	mixer_off();
	if (fh >= 0) { fs_close_handle(fh); fh = -1; }
	col_close();
	ns_close();
	net_src = net_stall = net_failed = net_wait = false;
	have_file = false;
	sound = false;
	cw_want_update();
}

static const char *open_err = "";
static bool is_url(const char *p) {
	return (p[0] == 'h' || p[0] == 'H') && (p[1] == 't' || p[1] == 'T') && (p[2] == 't' || p[2] == 'T') &&
	       (p[3] == 'p' || p[3] == 'P') && p[4] == ':';
}

static bool open_file(const char *path) {
	uint8_t hb[ZV_HDR_SIZE];
	uint32_t i;
	close_file();
	open_err = "";
	if (is_url(path)) {
		if (!ns_open(path)) {
			open_err = ns_error();
			dbg("cinema: "); dbg(open_err); dbg("\n");
			return false;
		}
		net_src = true;
		file_pos = 0;
		st_fs_ok = false;
	} else {
		fh = fs_open_read(path);
		if (fh < 0) { open_err = "cannot open the file"; dbg("cinema: cannot open the file\n"); return false; }
		st_fs_ok = zfs_stats(&st_fs0) == 0;
	}
	if (read_full(hb, ZV_HDR_SIZE) || zv_parse_header(hb, ZV_HDR_SIZE, &hd) || hd.max_rec > RQ_BYTES / 2) {
		/* (the server's own reason first: not found, no ranges, no answer) */
		open_err = net_src && ns_error()[0] ? ns_error() : "not a ZV file this player can show";
		dbg("cinema: "); dbg(open_err); dbg("\n");
		if (net_src) {
			static char keep[64];
			uint32_t k = 0;
			while (open_err[k] && k < sizeof(keep) - 1) { keep[k] = open_err[k]; k++; }
			keep[k] = 0;
			open_err = keep;
			ns_close();
			net_src = false;
		} else {
			fs_close_handle(fh);
			fh = -1;
		}
		return false;
	}
	nkeys = 0;
	if (hd.index_n && hd.index_off) {
		/* in one read (a call a seek point was ~2,700 calls for a
		 * feature film at 2 s apart), straight into keys[] */
		uint32_t n = hd.index_n < SEEK_POINTS ? hd.index_n : SEEK_POINTS;
		file_seek(hd.index_off);
		if (!read_full(keys, n * 8)) {
			for (i = 0; i < n * 2; i++) keys[i] = rd32((const uint8_t *)&keys[i]);
			nkeys = n;
		}
	}
	col_open(path);
	i = 0;
	while (path[i] && i < sizeof(filename) - 1) { filename[i] = path[i]; i++; }
	filename[i] = 0;
	have_file = true;
	sound = mix_avail && (hd.flags & ZV_F_AUDIO) && hd.afmt == ZV_A_PCM8;
	paused = false;
	/* the window opens at half size -- or whole, for a small film
	 * (archive.org's 320x240 copies) */
	view = hd.w <= 352 ? ZV_VIEW_MONO_FULL : ZV_VIEW_MONO_HALF;
	zv_dec_init(&dec, &hd, view, ZV_MASK_VAC8, 0);
	view_layout();
	cw_want_update();
	z_win_resize(&win, (uint32_t)(pw + Z_WIN_CHROME_W), (uint32_t)(ph + bar_h() + Z_WIN_CHROME_H));
	seek_to(0);
	update_title();
	return true;
}

static void toggle_pause(void) {
	if (!have_file) return;
	paused = !paused;
	if (paused) {
		if (sound) z_audio_mixer_enable(false);
	} else {
		/* without sound the clock restarts from the frame shown */
		if (sound) z_audio_mixer_enable(true);
		else {
			vs_start = zv_frame_vsync(&hd, (uint32_t)(front_frame >= 0 ? front_frame : 0), 60);
			fc_start = (uint16_t)z_game_frame();
		}
	}
	update_title();
}

/* A seek asked for by a key or the timeline is not done there and
 * then: it is noted, and done once when the messages waiting have all
 * been read (seek_service()) -- so ten presses of Right that arrived
 * together are one seek 100 s on, not ten. And it lands on a seek
 * point, the nearest in the direction asked: one frame to decode, where
 * an exact frame is up to two seconds of them (0.6-0.9 s on a board,
 * with nothing else answered meanwhile; docs/responsive.md). */
static int32_t seek_want = -1;          /* the frame asked for, or -1 */
static int seek_dir;                    /* > 0: not before it; else not after it */

static void seek_ask(int32_t f, int dir) {
	if (!have_file) return;
	if (f < 0) f = 0;
	if (f >= (int32_t)hd.frames) f = (int32_t)hd.frames - 1;
	seek_want = f;
	seek_dir = dir;
}

static void seek_rel(int32_t seconds) {
	int32_t base;
	if (!have_file) return;
	base = seek_want >= 0 ? seek_want : (front_frame >= 0 ? front_frame : 0);
	seek_ask(base + (int32_t)((int64_t)seconds * hd.fps_num / hd.fps_den), seconds);
}

static void seek_service(void) {
	uint32_t i, f, best;
	if (seek_want < 0) return;
	f = (uint32_t)seek_want;
	seek_want = -1;
	if (!have_file) return;
	best = 0;
	if (nkeys) {
		/* the last seek point not after f; forwards, the next one
		 * (if there is one), so a step on always moves on */
		for (i = 0; i < nkeys && keys[i * 2] <= f; i++) best = keys[i * 2];
		if (seek_dir > 0 && best < f && i < nkeys) best = keys[i * 2];
	} else best = f;
	seek_to(best);
}

/* the work of one pass: sound, the next picture, the refresh */
#define LAG_MAX 5       /* frames the picture may trail the sound before any are skipped: ~0.2 s */
static void play_step(void) {
	uint16_t fc;
	uint32_t vs;
	if (!have_file || paused) return;
	feed();
	if (net_src) {
		/* The network is behind: stop the sound -- the clock with it --
		 * before the mixer runs past what is in its ring and plays the
		 * old sound there again, and start it when half a second is
		 * queued. (A silent film's clock is the refresh: it shows the
		 * frames late instead.) And a connection that is gone for good:
		 * say so, and stop. */
		if (ns_error()[0] && !net_failed && at_end && !rq_n) {
			net_failed = true;
			net_stall = false;
			dbg("cinema: the film stopped: "); dbg(ns_error()); dbg("\n");
			paused = true;
			mixer_off();
			update_title();
			return;
		}
		if (sound) {
			uint32_t u = mixer_used(), sec = hd.arate * hd.achan;
			if (!net_stall && !at_end && u < sec / 8 && !rq_n) {
				net_stall = true;
				z_audio_mixer_enable(false);
				update_title();
			} else if (net_stall && (at_end || u >= sec / 2 || u >= MIXRING / 2)) {
				net_stall = false;
				z_audio_mixer_enable(true);
				update_title();
			}
		}
	}
	prepare_back();
	fc = (uint16_t)z_game_frame();
	if (fc == last_fc) return;
	last_fc = fc;
	vs = now_vsync();
	/* behind: decode frames without showing them, up to the one due --
	 * but a few at a time, so keys and the window are still seen to and
	 * something is shown; a machine too slow for the film falls behind
	 * the sound rather than freezing */
	{
		int n = 0;
		/* (Only when LAG_MAX frames behind. A frame that took long -- a
		 * cut, a seek point: several frames' time -- is shown late and
		 * the next ones as soon as each is ready, which catches up in
		 * a few frames when the average leaves time to spare; skipping
		 * them, as this did for any lateness, cost five frames a cut.) */
		while (n < 3 && back_frame >= 0 && (uint32_t)back_frame + 1 + LAG_MAX < hd.frames &&
		       zv_frame_vsync(&hd, (uint32_t)back_frame + 1 + LAG_MAX, 60) <= vs) {
			rq_t *r = rq_first_video();
			if (!r || !r->sound) break;
			back_frame = -1;                        /* this one is not shown */
			st_dropped++;
			n++;
			if (!decode_head(front ^ 1)) break;     /* (a piece of the next: the rest next pass) */
		}
	}
	if (back_frame >= 0 && zv_frame_vsync(&hd, (uint32_t)back_frame, 60) <= vs) {
		uint32_t t = z_cycles();
		st_shown++;
		if (zv_frame_vsync(&hd, (uint32_t)back_frame, 60) + 1 < vs) st_late++;
		swap_pictures();
		st_show_cycles += z_cycles() - t;
		/* wm repaints our window's frame once as it takes the colour
		 * window back, and that can land after game mode has the
		 * screen: in a film with black bars it stayed there, a line in
		 * the bar. Wiped once, a third of a second in. */
		if (game_clean && out_kind == OUT_CHUNKY_VRAM && --game_clean == 0) {
			z_fb_hw_sync();
			chunky_zero();
			show_front();
		}
		front_frame = back_frame;
		back_frame = -1;
		prepare_back();
	}
	/* every five seconds, how it is going (UART0) */
	if (st_lines && (uint16_t)(fc - st_last_fc) >= 300 && st_dec_n != st_last_n) {
		uint32_t n = st_shown + st_dropped;
		st_last_fc = fc;
		st_last_n = st_dec_n;           /* (paused, or at the end: no new line) */
		dbg("cinema: frame "); dbg_dec((uint32_t)front_frame);
		dbg(", due "); dbg_dec(vs * hd.fps_num / (60u * hd.fps_den));
		dbg(": shown "); dbg_dec(st_shown); dbg(", late "); dbg_dec(st_late);
		dbg(", not shown "); dbg_dec(st_dropped);
		dbg("; a frame: decode "); dbg_dec(st_dec_n ? st_dec_cycles / st_dec_n : 0);
		dbg(" (worst "); dbg_dec(st_dec_worst); dbg(")");
		st_dec_worst = 0;
		dbg(", read "); dbg_dec(n ? st_read_cycles / n : 0);
		dbg(" ("); dbg_dec(n ? st_read_bytes / n : 0); dbg(" bytes, ");
		dbg_dec(st_read_calls); dbg(" calls)");
		dbg(", show "); dbg_dec(st_shown ? st_show_cycles / st_shown : 0);
		dbg("; sound ahead "); dbg_dec(sound ? mixer_used() * 1000 / (hd.arate * hd.achan) : 0); dbg(" ms\n");
		report_fs();
		report_cpu();
		st_read_calls = 0;
	}
	/* The end. With sound, the ring is the clock, and the mixer does
	 * not stop at the write head: it goes round, playing the ring's
	 * last three seconds again, and the clock stops just short of the
	 * head (mixer_consumed()). Waiting for the ring to be exactly empty
	 * was waiting for one pass to land on one byte: missed, the film's
	 * last seconds of sound went round for good, loop or no loop, and
	 * the last frame was never due. So: once every record's sound is in
	 * the ring, an eighth of a second of silence goes in after it, and
	 * the film is over when the mixer is inside that -- or has stopped
	 * getting nearer, should a pass ever be long enough to miss it. */
	if (sound && at_end && !end_pad) {
		int i;
		bool all = true;
		for (i = 0; i < rq_n; i++) if (!rq[(rq_head + i) % RQ_N].sound) all = false;
		if (all) {
			uint32_t n = hd.arate * hd.achan / 8, k;
			if (mix_head + n - mixer_consumed() > MIXRING - 64) n = 0;
			for (k = 0; k < n; k++) mixring[(mix_head + k) & MIXMASK] = 0;
			mix_head += n;
			end_pad = true;
			end_used = mixer_used();
			end_stall = 0;
		}
	}
	if (at_end && !rq_n) {
		bool over = !sound;
		if (sound && end_pad) {
			uint32_t u = mixer_used();
			if (u < end_used) { end_used = u; end_stall = 0; }
			else end_stall++;
			over = u <= hd.arate * hd.achan / 16 || end_stall >= 30;
		}
		if (over && back_frame >= 0 && sound) {      /* the last frame, if the clock stopped short of it */
			st_shown++;
			swap_pictures();
			front_frame = back_frame;
			back_frame = -1;
		}
		if (over && back_frame < 0 && front_frame + 1 >= (int32_t)hd.frames) {
			report();
			if (looping) seek_to(0);
			else { paused = true; mixer_off(); update_title(); }
		}
	}
}

/* -- messages ---------------------------------------------------------- */

static void forward_msg(z_msg_t *msg, void *user) {
	(void)user;
	switch (msg->subject) {
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
		if (out_kind == OUT_WINDOW) set_view(view_for_window());
		break;
	default:
		break;
	}
}

static void do_open(void) {
	char path[80];
	bool was = paused;
	if (have_file && !paused) toggle_pause();
	if (!z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path))) {
		if (have_file && !was) toggle_pause();
		return;
	}
	{
		int i = 0, slash = -1;
		for (; path[i] && i < (int)sizeof(last_dir) - 1; i++) { last_dir[i] = path[i]; if (path[i] == '/') slash = i; }
		last_dir[slash > 0 ? slash : 1] = 0;
	}
	open_file(path);
	repaint();
}

/* A film on a web server (u): its address, asked for until it opens or
 * the box is cancelled -- what went wrong is the box's next message,
 * over the address as it was typed. */
static char last_url[sizeof(filename)] = "http://";

static void do_open_url(void) {
	char url[sizeof(filename)], msg[96];
	bool was = paused;
	const char *why = "";
	if (have_file && !paused) toggle_pause();
	for (;;) {
		int i = 0;
		const char *a = why[0] ? "Could not open it: " : "The address of a .zv film on a web server:";
		while (*a && i < (int)sizeof(msg) - 1) msg[i++] = *a++;
		while (*why && i < (int)sizeof(msg) - 1) msg[i++] = *why++;
		msg[i] = 0;
		if (!z_dialog_prompt(&dlg_ctx, "Open address", msg, last_url, url, sizeof(url))) {
			if (have_file && !was && paused) toggle_pause();
			repaint();
			return;
		}
		for (i = 0; url[i] && i < (int)sizeof(last_url) - 1; i++) last_url[i] = url[i];
		last_url[i] = 0;
		if (!is_url(url)) { why = "it must start with http://"; continue; }
		repaint();
		if (open_file(url)) break;
		why = open_err[0] ? open_err : "no film there";
		was = true;             /* the film that was playing is closed now */
	}
	repaint();
}

/* -- game mode: the whole screen, in as many colours as the board has --
 *
 * Chunky boards (the 45F ones, and the consoles' picture in main
 * memory): the half view in 256 colours -- the grey palette, so a black
 * and white film is shown in greys rather than dithered. Elsewhere: the
 * half view in black and white, doubled. */

/* a film that fits game mode's 320x240 whole: shown at full size there */
static bool game_small(void) {
	return hd.w <= Z_CHUNKY_W && hd.h <= Z_CHUNKY_H;
}

static void chunky_zero(void) {
	uint32_t y, s2, i;
	for (y = 0; y < Z_CHUNKY_H; y++)
		for (s2 = 0; s2 < 4; s2++) {
			volatile uint32_t *d = z_chunky_seg(y, s2);
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i++) d[i] = 0;
		}
}

static void enter_game(void) {
	int i;
	if (!have_file || out_kind != OUT_WINDOW || !z_game_available()) return;
	z_game_set_pointer(false);
	/* the colour window back to wm before game mode takes the second
	 * sheet: told afterwards, wm went on trying to tint its desktop
	 * under the game (16% of the CPU, in zsys) */
	game_entering = true;
	cw_want_update();
	game_entering = false;
	gw_fix = false;
	if (!z_game_set_enabled(true, false)) return;
	z_game_set_view(0, 0);
	z_gfx_clear_visible();
	z_gfx_blit_scissor_reset();
	z_fb_hw_fill_rect(0, 0, Z_SCREEN_W, Z_SCREEN_H, 0);
	if (z_color_chunky_available()) {
		ngreys = zv_grey_palette(greys, ZV_PAL_MAX, dec.greys);
		for (i = 0; i < 256; i++) z_color_set_palette((uint32_t)i, i < ngreys ? greys[i] : 0);
		if (z_vram_sdram()) {
			if (!z_color_set_chunky(true)) goto mono;
			z_chunky_set_pic(pic[front], true);
			out_kind = OUT_CHUNKY_MEM;
		} else {
			z_fb_hw_sync();
			chunky_zero();
			if (!z_color_set_chunky(true)) goto mono;
			/* the blitter is to reach the second sheet (rows 480..959),
			 * and the whole screen, not the window's visible part */
			z_gfx_clear_visible();
			z_gfx_blit_scissor_reset();
			z_fb_set_rows(960);
			out_kind = OUT_CHUNKY_VRAM;
			game_clean = 20;
		}
		/* a colour film: its colour layer, over the true greys */
		col_on = col_have && dec.greys == ZV_GREYS_TRUE;
		col_g = -1;
		col_nch = 0;
		memset(col_cmap, 0, sizeof(col_cmap));
		memset(col_cls, 0, sizeof(col_cls));
		memset(col_w, 0, sizeof(col_w));
		pal_shown = -2;
		/* the palette view, whole: decoded again from the seek point --
		 * at full size for a film that fits 320x240, else half */
		memset(pic, 0, sizeof(pic));
		set_view(game_small() ? ZV_VIEW_PAL_FULL : ZV_VIEW_PAL_HALF);
		return;
	}
mono:
	out_kind = OUT_GAME_MONO;
	set_view(game_small() ? ZV_VIEW_MONO_FULL : ZV_VIEW_MONO_HALF);
	show_front();
}

static void leave_game(bool revoked) {
	if (out_kind == OUT_WINDOW) return;
	col_on = false;
	dec.colw = NULL;
	if (chunky_out()) {
		if (out_kind == OUT_CHUNKY_VRAM) { z_fb_hw_sync(); chunky_zero(); z_fb_set_rows(480); }
		z_color_set_chunky(false);
	}
	if (!revoked) z_game_set_enabled(false, false);
	out_kind = OUT_WINDOW;
	/* wm repaints the desktop and asks the window to redraw */
	set_view(view_for_window());
}

static void handle_key(uint32_t k) {
	if (out_kind != OUT_WINDOW) {
		/* game mode: Esc or g leave it; no boxes there */
		if (k == 27 || k == 'g') { leave_game(false); return; }
		if (k == Z_KEY_HELP || k == 'o' || k == 'u') return;
	}
	if (k == Z_KEY_HELP) {
		static const char help[] =
			"A film player, for ZV files (made from mp4 and the like with "
			"tools/zv/zvenc on another computer).\n\n"
			"Space\tPlay / pause\n"
			"Left/Right\t10 seconds back / on\n"
			"Up/Down\tA minute on / back\n"
			"Home\tThe start\n"
			"l\tLoop on / off\n"
			"c\tControls under the picture, on / off\n"
			"i\tTiming lines on the serial console, on / off\n"
			"o\tOpen a film\n"
			"u\tOpen a film on a web server, by its address\n"
			"F11\tFull screen (the film at full size)\n"
			"g\tGame mode: the whole screen, in greys (or a colour film's\n"
			"\tcolours) where the board has them (Esc or g leaves it)\n"
			"F10\tColour on or off: a colour film's colours in the\n"
			"\twindow, where the board has them\n"
			"q\tQuit\n\n"
			"The window shows the film at half size; made big enough, it "
			"shows it whole.";
		bool was = paused;
		if (have_file && !paused) toggle_pause();
		z_dialog_help(&dlg_ctx, "Cinema help", help, "cinema_app");
		if (have_file && !was) toggle_pause();
		return;
	}
	switch (k) {
	case ' ': toggle_pause(); break;
	case Z_KEY_LEFT: seek_rel(-10); break;
	case Z_KEY_RIGHT: seek_rel(10); break;
	case Z_KEY_UP: seek_rel(60); break;
	case Z_KEY_DOWN: seek_rel(-60); break;
	case Z_KEY_HOME: seek_ask(0, 0); break;
	case 'c': bar_toggle(); break;
	case 'l': looping = !looping; break;
	case 'i':
		st_lines = !st_lines;
		dbg(st_lines ? "cinema: timing lines on (UART0, every five seconds)\n" : "cinema: timing lines off\n");
		break;
	case 'o': do_open(); break;
	case 'u': do_open_url(); break;
	case 'g': enter_game(); break;
	case Z_KEY_COLOUR:
		/* F10: the colour window off, or on again */
		if (cw_avail) { cw_off = !cw_off; cw_want_update(); }
		break;
	case 'q': quit_now = true; break;
	default: break;
	}
}

int main(void) {
	char launch_arg[sizeof(filename)];
	bool have_arg;

	memset(pic, 0, sizeof(pic));
	rq_flush();
	have_file = false;
	fh = -1;

	have_arg = z_launch_path_take(launch_arg, sizeof(launch_arg));

	/* 32 colours in the window (the 45F boards): asked for only while
	 * a colour film is showing (Z_WIN_FLAG_COLOUR_LATER, cw_want_update()) */
	cw_avail = z_color_window_available() && z_color_window_planes() == 4 && z_fb_hw_blit_mem_available();
	if (z_win_create_flags(&win, "cinema", NO_FILE_W + Z_WIN_CHROME_W, NO_FILE_H + Z_WIN_CHROME_H, -1, -1,
	                       Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_OPEN_ICON |
	                       (cw_avail ? Z_WIN_FLAG_COLOR | Z_WIN_FLAG_COLOUR_LATER | Z_WIN_FLAG_COLOUR_ICON : 0)) != Z_OK) {
		dbg("cinema: no window -- is wm running?\n");
		return 1;
	}
	use_hw_blit = z_fb_hw_blit_mem_available();

	mix_avail = z_audio_present() && z_audio_mixer_present();
	if (mix_avail) {
		z_audio_start_hz(z_audio_preferred_hz());
		out_hz = z_audio_rate_hz();
		if (!out_hz) mix_avail = false;
		mixer_off();
	}
	if (!mix_avail) dbg("cinema: no mixer: films play without sound\n");

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	zv_header_t none;
	memset(&none, 0, sizeof(none));
	none.w = 320; none.h = 240; none.fps_num = 24; none.fps_den = 1;
	zv_dec_init(&dec, &none, ZV_VIEW_MONO_HALF, ZV_MASK_VAC8, 0);
	view_layout();

	if (have_arg) open_file(launch_arg);
	update_title();
	repaint();

	for (;;) {
		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {
			if (ns_msg(&msg)) continue;         /* the network's (netsrc.c) */
			switch (msg.subject) {
			case Z_WM_KEY:
				if (msg.obj.type != Z_UINT32) break;
				if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;
				handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32));
				break;
			case Z_WM_MOUSE:
				if (msg.obj.type == Z_UINT32 && out_kind == OUT_WINDOW) bar_mouse(msg.obj.val.uint32);
				break;
			case Z_WM_TITLEBAR_ICON: {
				uint32_t v;
				if (msg.obj.type != Z_UINT32) break;
				v = msg.obj.val.uint32;
				if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;
				if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_OPEN) do_open();
				else if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_COLOUR) handle_key(Z_KEY_COLOUR);
				break;
			}
			case Z_WM_COLOR:
				/* wm says we may have the colour window, or no longer:
				 * then it has cleared COLWIN itself */
				if (msg.obj.type != Z_UINT32) break;
				cw_focus = msg.obj.val.uint32 != 0;
				if (!cw_focus && cw_on) {
					cw_on = false;
					if (gw) { gw = false; gw_fix = true; }
					if (cw_blitting) { z_fb_hw_sync(); cw_blitting = false; }
					cw_clear();
					z_fb_set_rows(480);
				}
				break;
			case Z_WM_GAME_REVOKED:
				leave_game(true);
				break;
			case Z_WM_CLOSE:
				if (msg.obj.type == Z_UINT32 && (int32_t)msg.obj.val.uint32 == win.id) quit_now = true;
				break;
			default:
				forward_msg(&msg, NULL);
				break;
			}
		}
		if (quit_now) {
			report();
			cw_drop();
			leave_game(false);
			close_file();
			z_win_destroy(&win);
			return 0;
		}
		if (net_src) ns_poll();
		seek_service();
		play_step();
		/* the colour window and the bar: once a refresh is as often as
		 * either can show anything, and the loop comes round far more
		 * often than that (every tick while it waits) */
		{
			static uint16_t ui_fc = 0xffff;
			uint16_t now_fc = (uint16_t)z_game_frame();
			if (now_fc != ui_fc || bar_drag || seek_want >= 0) {
				ui_fc = now_fc;
				cw_check();
				bar_update();
			}
		}
		/* a refresh is 16.7 ms; the scheduler's tick 1.4: wait a tick
		 * when there is nothing to do before the next refresh */
		if (!have_file || paused) z_proc_wait(Z_TICK_HZ / 30);
		else if (back_frame >= 0 || !rq_n) z_proc_wait(1);
	}
}
