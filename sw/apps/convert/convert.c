/*
 * Zeitlos -- convert: documents, tables and archives from one form to
 * another. docs/convert.md.
 *
 *   convert IN                  open it: a document in text or read, a
 *                               table in sheet, an archive extracted
 *                               beside it and shown in files
 *   convert IN OUT              types from the names (and IN's contents)
 *   convert -t docx IN OUT      the output type named
 *   convert IN DIR              an archive (zip, tar, tar.gz) extracted
 *   convert DIR OUT.zip         a folder packed: .zip, .tar, .tar.gz
 *   convert DIR                 ... into DIR.zip beside it
 *   convert                     a window: choose a file or folder, a
 *                               format, where it goes
 *   convert -l                  the formats
 *   convert -j JOB              a job from an app (sw/common/zconvert.h)
 *
 * The output is written as OUT.part and renamed to OUT only when it is
 * complete, so OUT is never half a file. The work itself is
 * sw/common/formats; this file is the board's side of it: files
 * through the fs driver, memory from the arena, progress and the
 * result to the app that asked, output to the shell that ran it -- and,
 * when a person is waiting on it, a small window with a progress bar
 * and Cancel.
 *
 * -- staying responsive --
 *
 * The conversion polls (zf_cancel_poll) at every 4KB it reads. Each
 * poll is cheap; every 20ms or so one of them also services the window
 * -- redraws, the Cancel button, Esc, the close icon -- so Cancel
 * takes effect within a poll, and the window never stops answering wm.
 * The window appears only after 300ms: a conversion that is done
 * before then never flashes one.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "zeitlos.h"
#include "zsoc.h"
#include "zfsapp.h"
#include "zwm.h"
#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"
#include "zobj.h"
#include "zport.h"
#include "zpaths.h"
#include "zargs.h"
#include "ztype.h"
#include "zconvert.h"
#include "zwidget.h"
#include "zkbd.h"
#include "zdialog.h"
#include "pack.h"
#include "../posix/posix.h"
#include "formats/zf.h"

// -- output: to the posix shell that ran us, else the UART --
//
// The relay is zfpga's (sw/apps/zfpga/port_dev.c), which records why
// each part is the way it is: batched sends, and no exit while a send
// is unacknowledged.

void uart_putc(char c);

static z_port_t out_port;
static bool out_connected;
static char out_buf[256];
static int out_len;

static void pump(void);

static void out_flush(void) {
	if (!out_len) return;
	if (out_connected) {
		for (int t = 0; t < 32; t++) {
			if (z_port_send(&out_port, out_buf, (uint32_t)out_len) == Z_OK) break;
			pump();
			z_proc_wait(1);
		}
	} else {
		for (int i = 0; i < out_len; i++) uart_putc(out_buf[i]);
	}
	out_len = 0;
}

static bool quiet;				// the window: no shell to tell

static void out(const char *s) {
	if (quiet) return;
	for (; *s; s++) {
		if (out_len >= (int)sizeof out_buf - 2) out_flush();
		if (*s == '\n') out_buf[out_len++] = '\r';
		out_buf[out_len++] = *s;
	}
	out_flush();
}

static void out_open(void) {
	uint32_t pid = 0;
	if (!z_pid_lookup("posix0", &pid) || !pid) return;
	if (z_port_connect_arg(&out_port, pid, z_obj_str(PX_STDOUT_TAG)) == Z_OK) out_connected = true;
}

static void out_close(void) {
	out_flush();
	if (!out_connected) return;
	uint32_t start = z_uptime_ticks();
	while (out_port.pending_count > 0 && (z_uptime_ticks() - start) < Z_TICK_HZ * 2u) {
		pump();
		if (out_port.pending_count > 0) z_proc_wait(1);
	}
	z_port_close(&out_port);
	out_connected = false;
}

// -- the window --

#define WIN_W		240
#define WIN_H		56
#define BAR_Y		16
#define BAR_H		8
#define BTN_W		50
#define BTN_H		14

static bool ui;					// a person is waiting: show progress, allow Cancel
static bool win_up;
static z_win_t win;
static uint32_t t_start, t_ui;
static int permille;
static bool cancelled;
static char ui_title[64];		// "Opening report.docx"
static char ui_msg[96];			// an error, once there is one

static int btn_x(void) { return z_win_content_w(&win) - BTN_W - 6; }
static int btn_y(void) { return z_win_content_h(&win) - BTN_H - 6; }

static void draw_button(const char *label) {
	int x = btn_x(), y = btn_y();
	z_win_fill_rect(&win, x, y, BTN_W, BTN_H, 1);
	z_win_fill_rect(&win, x + 1, y + 1, BTN_W - 2, BTN_H - 2, 0);
	int tw = (int)strlen(label) * z_font_5x8.w;
	z_win_draw_text(&win, x + (BTN_W - tw) / 2, y + (BTN_H - z_font_5x8.h) / 2, label, 1, &z_font_5x8);
}

static int bar_y = BAR_Y;		// the window (gui) puts it lower

static void draw_bar(void) {
	int w = z_win_content_w(&win) - 12;
	if (w < 4) return;
	z_win_fill_rect(&win, 6, bar_y, w, BAR_H, 1);
	z_win_fill_rect(&win, 7, bar_y + 1, w - 2, BAR_H - 2, 0);
	int f = (int)((long)(w - 4) * permille / 1000);
	if (f > 0) z_win_fill_rect(&win, 8, bar_y + 2, f, BAR_H - 4, 1);
}

static bool gui;				// convert's own window: gui_draw(), gui_msg()
static void gui_draw(void);
static bool gui_msg(z_msg_t *m);

static void draw_all(void) {
	if (gui) { gui_draw(); return; }
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	z_win_fill_rect(&win, 0, 0, cw, ch - Z_WIN_GRIP_INSET, 0);
	z_win_fill_rect(&win, 0, ch - Z_WIN_GRIP_INSET, cw - Z_WIN_GRIP_INSET, Z_WIN_GRIP_INSET, 0);
	if (ui_msg[0]) {
		z_win_draw_text(&win, 6, 4, ui_title, 1, &z_font_5x8);
		z_win_draw_text(&win, 6, BAR_Y, ui_msg, 1, &z_font_5x8);
		draw_button("OK");
		return;
	}
	z_win_draw_text(&win, 6, 4, ui_title, 1, &z_font_5x8);
	draw_bar();
	draw_button("Cancel");
}

static void on_msg(z_msg_t *m, void *user) {
	(void)user;
	if (gui && gui_msg(m)) return;
	switch (m->subject) {
	case Z_PORT_DATA_ACK:
		z_port_handle_ack(&out_port, m);
		break;
	case Z_PORT_CLOSE:
		out_connected = false;
		break;
	case Z_WM_SET_CLIP:
		if (win_up) z_win_apply_clip(&win, &m->obj);
		break;
	case Z_WM_REDRAW:
		if (!win_up || m->obj.type != Z_UINT32 || z_win_redraw_id(m->obj.val.uint32) != win.id) break;
		z_win_apply_redraw(&win, m->obj.val.uint32);
		draw_all();
		z_win_redraw_done(&win);
		break;
	case Z_WM_WINDOW_MOVED:
		if (win_up) z_win_parse_rect(&win, &m->obj);
		break;
	case Z_WM_WINDOW_RESIZED:
		if (win_up) z_win_apply_resized(&win, &m->obj);
		break;
	case Z_WM_CLOSE:
		if (win_up) cancelled = true;
		break;
	case Z_WM_KEY:
		if (win_up && m->obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32)) {
			uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32);
			if (k == 0x1b || k == 0x0d || k == ' ') cancelled = true;
		}
		break;
	case Z_WM_MOUSE:
		if (win_up && m->obj.type == Z_UINT32) {
			int cx, cy;
			z_win_mouse_content_xy(&win, m->obj.val.uint32, &cx, &cy);
			if ((Z_WM_UNPACK_MOUSE_BUTTONS(m->obj.val.uint32) & Z_MOUSE_BTN_LEFT) &&
				cx >= btn_x() && cx < btn_x() + BTN_W && cy >= btn_y() && cy < btn_y() + BTN_H)
				cancelled = true;
		}
		break;
	default:
		break;
	}
}

static void pump(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) on_msg(&m, NULL);
}

static void win_open(void) {
	if (win_up) return;
	if (z_win_create_cb(&win, "convert", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON, on_msg, NULL) != Z_OK) {
		ui = false;			// no wm: carry on without a window
		return;
	}
	win_up = true;
	draw_all();
}

// -- the caller, progress, cancelling --

static uint32_t caller_pid, caller_serial;
static uint32_t last_check, last_sent, last_ui;
static int last_permille = -1;
static bool caller_gone;

static bool cancel_poll(void) {
	uint32_t now = z_uptime_ticks();
	if (ui && now - last_ui >= Z_TICK_HZ / 50) {
		last_ui = now;
		if (!win_up && now - t_start >= Z_TICK_HZ * 3 / 10) win_open();
		pump();
		if (win_up && permille != t_ui) { t_ui = (uint32_t)permille; draw_bar(); }
	}
	if (cancelled) return true;
	if (!caller_pid) return false;
	// a few times a second: z_proc_serial is a syscall
	if (now - last_check < Z_TICK_HZ / 5) return caller_gone;
	last_check = now;
	if (z_proc_serial(caller_pid) != caller_serial) caller_gone = true;
	return caller_gone;
}

static void send(uint32_t subject, uint32_t tag) {
	z_msg_t m;
	memset(&m, 0, sizeof m);
	m.to = caller_pid;
	m.subject = subject;
	m.tag = tag;
	m.obj = z_obj_none();
	z_msg_send(&m);
}

static void progress(uint32_t pm) {
	permille = (int)pm;
	if (!caller_pid || (int)pm == last_permille) return;
	uint32_t now = z_uptime_ticks();
	if (now - last_sent < Z_TICK_HZ / 10 && pm < 1000) return;		// ten a second at most
	last_sent = now;
	last_permille = (int)pm;
	send(Z_CONVERT_PROGRESS, pm);
}

// -- files --

#define WBUF	8192

typedef struct {
	int			h;
	uint8_t		*wbuf;		// writes are gathered: the writers make many small ones
	uint32_t	wlen;
	bool		bad;
} fio_t;

// One write buffer: an archive's files and a converted file are never
// written at the same time.
static uint8_t wbuf[WBUF];

static bool w_flush(fio_t *f) {
	uint32_t done = 0;
	while (done < f->wlen) {
		uint32_t k = f->wlen - done;
		if (k > 16384) k = 16384;
		int w = fs_write_chunk(f->h, f->wbuf + done, (int)k);
		if (w <= 0) { f->bad = true; return false; }
		done += (uint32_t)w;
	}
	f->wlen = 0;
	return true;
}

static int32_t io_read(zf_io_t *io, void *buf, uint32_t n) {
	fio_t *f = (fio_t *)io->h;
	if (n > 16384) n = 16384;		// one trip into FatFs stays short
	int r = fs_read_chunk(f->h, buf, (int)n);
	if (r < 0) return -1;
	io->pos += (uint32_t)r;
	return r;
}

static int32_t io_write(zf_io_t *io, const void *buf, uint32_t n) {
	fio_t *f = (fio_t *)io->h;
	if (f->bad) return -1;
	if (f->wlen + n > WBUF && !w_flush(f)) return -1;
	if (n >= WBUF) {
		const uint8_t *p = buf;
		uint32_t done = 0;
		while (done < n) {
			uint32_t k = n - done > 16384 ? 16384 : n - done;
			int w = fs_write_chunk(f->h, p + done, (int)k);
			if (w <= 0) { f->bad = true; return -1; }
			done += (uint32_t)w;
		}
	} else {
		memcpy(f->wbuf + f->wlen, buf, n);
		f->wlen += n;
	}
	io->pos += n;
	return (int32_t)n;
}

static bool io_seek(zf_io_t *io, uint32_t pos) {
	fio_t *f = (fio_t *)io->h;
	if (f->wbuf && f->wlen && !w_flush(f)) return false;
	if (!fs_seek(f->h, pos)) return false;
	io->pos = pos;
	return true;
}

static void io_init(zf_io_t *io, fio_t *f) {
	memset(io, 0, sizeof *io);
	io->h = f;
	io->read = io_read;
	io->write = io_write;
	io->seek = io_seek;
}

static bool io_close(zf_io_t *io) {
	fio_t *f = (fio_t *)io->h;
	bool ok = true;
	if (f->wbuf && f->wlen) ok = w_flush(f);
	if (f->wbuf) ok = fs_sync(f->h) && ok;
	fs_close_handle(f->h);
	f->h = -1;
	return ok && !f->bad;
}

// Extracting: files under a folder.
static char dir_root[Z_CONV_PATH_MAX];
static fio_t dir_file = { -1, NULL, 0, false };

static void join(char *o, int cap, const char *rel) {
	snprintf(o, (size_t)cap, "%s/%s", dir_root, rel);
}

static bool d_open(zf_dir_t *d, const char *rel, zf_io_t *o) {
	static char p[Z_CONV_PATH_MAX + ZF_PATH_MAX + 2];
	(void)d;
	join(p, sizeof p, rel);
	memset(&dir_file, 0, sizeof dir_file);
	dir_file.h = fs_open_write(p);
	if (dir_file.h < 0) return false;
	dir_file.wbuf = wbuf;
	io_init(o, &dir_file);
	return true;
}

static void d_close(zf_dir_t *d, zf_io_t *o) {
	(void)d;
	io_close(o);
}

static void d_remove(zf_dir_t *d, const char *rel) {
	static char p[Z_CONV_PATH_MAX + ZF_PATH_MAX + 2];
	(void)d;
	join(p, sizeof p, rel);
	fs_unlink(p);
}

static bool exists(const char *p, bool *dir) {
	z_fs_info_t fi;
	if (!fs_stat(p, &fi)) return false;
	if (dir) *dir = (fi.attr & Z_FS_ATTR_DIR) != 0;
	return true;
}

static bool is_dir(const char *p) {
	bool d = false;
	return exists(p, &d) && d;
}

static bool d_mkdir(zf_dir_t *d, const char *rel) {
	static char p[Z_CONV_PATH_MAX + ZF_PATH_MAX + 2];
	(void)d;
	join(p, sizeof p, rel);
	return fs_mkdir(p) || is_dir(p);
}

// -- memory --

// zf_heap: what xlsx's shared strings and an epub's manifest are kept
// in. The arena when the machine has room for one (a megabyte, or what
// it will give); else this.
static uint8_t heap_small[8192];

static void heap_init(void) {
	static const uint32_t want[] = { 1024u * 1024u, 256u * 1024u, 64u * 1024u };
	for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
		void *base = NULL;
		uint32_t got = 0;
		if (z_arena_set(want[i], &base, &got) == Z_ARENA_OK && base && got) {
			zf_heap = base;
			zf_heap_size = got;
			return;
		}
	}
	zf_heap = heap_small;
	zf_heap_size = sizeof heap_small;
}

// -- the job --

static char in_path[Z_CONV_PATH_MAX], out_path[Z_CONV_PATH_MAX], part_path[Z_CONV_PATH_MAX + 8];
static char to_name[16];
static bool open_after;			// open the result in the app for it

static bool read_job(const char *job) {
	static char buf[3 * Z_CONV_PATH_MAX + 128];
	int n = fs_read_file((char *)job, buf, (int)sizeof buf - 1);
	fs_unlink((char *)job);
	if (n <= 0) return false;
	buf[n] = 0;
	char *p = buf;
	while (*p) {
		char *e = strchr(p, '\n');
		if (e) *e = 0;
		if (!strncmp(p, "in=", 3)) snprintf(in_path, sizeof in_path, "%s", p + 3);
		else if (!strncmp(p, "out=", 4)) snprintf(out_path, sizeof out_path, "%s", p + 4);
		else if (!strncmp(p, "to=", 3)) snprintf(to_name, sizeof to_name, "%s", p + 3);
		else if (!strncmp(p, "ui=1", 4)) ui = true;
		else if (!strncmp(p, "open=1", 6)) open_after = true;
		else if (!strncmp(p, "caller=", 7)) {
			char *c = p + 7;
			caller_pid = (uint32_t)strtoul(c, &c, 10);
			if (*c == ':') caller_serial = (uint32_t)strtoul(c + 1, NULL, 10);
		}
		if (!e) break;
		p = e + 1;
	}
	return in_path[0] && (out_path[0] || open_after);
}

static void usage(void) {
	out("usage: convert IN                     open it (text, read, sheet, files)\n"
		"       convert [-t TYPE] IN OUT       a document or table\n"
		"       convert IN DIR                 extract zip, tar, tar.gz\n"
		"       convert IN.gz OUT              one gzipped file\n"
		"       convert -l                     the formats\n");
}

static void list_formats(void) {
	out("documents in:  txt md html rtf docx odt epub\n"
		"documents out: txt md html rtf docx odt\n"
		"tables:        csv tsv zss xlsx ods (in and out)\n"
		"archives:      zip tar tar.gz (to a folder), gz (to a file)\n");
}

// -- opening: where the result goes, and what shows it --

static const char *base_name(const char *p) {
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

// `name` without its last extension (and a ".tar" before ".gz").
static void strip_ext(const char *name, char *o, int cap, zf_type_t t) {
	snprintf(o, (size_t)cap, "%s", name);
	char *dot = strrchr(o, '.');
	if (dot && dot != o) *dot = 0;
	if (t == ZF_T_TGZ) {
		dot = strrchr(o, '.');
		if (dot && dot != o && zf_ieq(dot, ".tar")) *dot = 0;
	}
}

// A name beside `want` that is not taken: want, want-2, ... want-9.
static bool free_name(char *want, int cap, const char *ext) {
	char base[Z_CONV_PATH_MAX];
	snprintf(base, sizeof base, "%s", want);
	for (int i = 1; i <= 9; i++) {
		if (i == 1) snprintf(want, (size_t)cap, "%s%s", base, ext);
		else snprintf(want, (size_t)cap, "%s-%d%s", base, i, ext);
		if (!exists(want, NULL)) return true;
	}
	return false;
}

// Where an opened file goes: documents and tables to /tmp as Markdown
// and the sheet's own format (gone at the next boot -- the person
// saves it where they want it), archives beside the archive.
static bool open_target(zf_type_t from, char *to, int cap) {
	char stem[Z_CONV_PATH_MAX];
	if (zf_type_is_doc(from) || zf_type_is_tab(from)) {
		strip_ext(base_name(in_path), stem, sizeof stem, from);
		snprintf(out_path, sizeof out_path, Z_DIR_TMP "/%s", stem);
		bool doc = zf_type_is_doc(from);
		snprintf(to, (size_t)cap, "%s", doc ? "md" : "zss");
		// /tmp's copy is replaced: it is ours
		int n = (int)strlen(out_path);
		snprintf(out_path + n, sizeof out_path - (size_t)n, doc ? ".md" : ".zss");
		return true;
	}
	if (zf_type_is_arch(from)) {
		strip_ext(in_path, stem, sizeof stem, from);
		snprintf(out_path, sizeof out_path, "%s", stem);
		to[0] = 0;
		if (from == ZF_T_GZ) {
			// "notes.txt.gz" -> "notes.txt"; taken -> "notes-2.txt"
			char *dot = strrchr(out_path, '.');
			char ext[16] = "";
			if (dot && dot > strrchr(out_path, '/') && strlen(dot) < sizeof ext) {
				snprintf(ext, sizeof ext, "%s", dot);
				*dot = 0;
			}
			return free_name(out_path, sizeof out_path, ext);
		}
		return free_name(out_path, sizeof out_path, "");
	}
	return false;
}

// What text can take on this machine: its own 32KB, or an arena it
// can get with room to spare. Larger documents open in read, which
// streams.
static bool fits_text(uint32_t size) {
	if (size <= 30000) return true;
	z_mem_stats_args_t ms;
	if (!z_mem_stats(&ms)) return false;
	return ms.largest_free / 2 > size + 256u * 1024u;
}

static void open_result(zf_type_t from) {
	const char *app;
	if (zf_type_is_doc(from)) app = fits_text((uint32_t)fs_size(out_path)) ? "text" : "read";
	else if (zf_type_is_tab(from)) app = "sheet";
	else if (from == ZF_T_GZ) app = z_ftype_app_for(out_path);
	else app = "files";
	if (!app) return;
	z_launch_arg_set(out_path);
	z_proc_run(app);
}

// -- the end --

static int finish(int r) {
	if (caller_pid && !caller_gone) send(Z_CONVERT_DONE, (uint32_t)r);
	if (ui && r < 0 && r != ZF_E_CANCEL) {
		// Said in the window, which stays until it is read.
		snprintf(ui_msg, sizeof ui_msg, "Not possible: %s.", zf_strerror(r));
		if (!win_up) win_open();
		if (win_up) {
			draw_all();
			cancelled = false;
			while (!cancelled) {
				z_msg_t m;
				if (z_msg_recv(&m, Z_TICK_HZ) == Z_OK) on_msg(&m, NULL);
			}
		}
	}
	if (win_up) z_win_destroy(&win);
	out_close();
	z_arena_set(0, NULL, NULL);
	exit(r < 0 ? 1 : 0);
}

// -- packing a folder: pack.c walks it, and reads each file through here --

bool pack_open_file(const char *path, uint32_t size, zf_io_t *io) {
	static fio_t f;
	memset(&f, 0, sizeof f);
	f.h = fs_open_read(path);
	if (f.h < 0) return false;
	io_init(io, &f);
	io->size = size;
	return true;
}

void pack_close_file(zf_io_t *io) {
	fio_t *f = (fio_t *)io->h;
	if (!f || f->h < 0) return;
	fs_close_handle(f->h);
	f->h = -1;
}

// -- one conversion: in_path to out_path (or opened), to_name --

// The shell gets its own words for what went wrong; the window and an
// app get r.
static bool said;				// say() has told the shell what went wrong

static void say(const char *a, const char *b, const char *c) {
	said = true;
	out("convert: "); out(a); if (b) out(b); if (c) out(c); out("\n");
}

static int convert_one(zf_type_t *from_out) {
	static zf_io_t in, outio;
	static fio_t fin, fout;
	zf_type_t from;
	int r;

	zf_tab_dropped = 0;
	pack_skipped = 0;
	*from_out = ZF_T_UNKNOWN;

	// a folder: packed
	if (is_dir(in_path)) {
		if (open_after) {
			// `convert DIR`: DIR.zip beside it
			snprintf(out_path, sizeof out_path, "%s", in_path);
			int n = (int)strlen(out_path);
			while (n > 1 && out_path[n - 1] == '/') out_path[--n] = 0;
			if (!free_name(out_path, sizeof out_path, ".zip")) return ZF_E_IO;
			open_after = false;
		}
		zf_type_t to = to_name[0] ? zf_type_from_name(to_name) : zf_detect(out_path, NULL);
		if (to != ZF_T_ZIP && to != ZF_T_TAR && to != ZF_T_TGZ) {
			say("a folder becomes a .zip, .tar or .tar.gz, not ", out_path, NULL);
			return ZF_E_UNSUPP;
		}
		snprintf(part_path, sizeof part_path, "%s.part", out_path);
		memset(&fout, 0, sizeof fout);
		fout.h = fs_open_write(part_path);
		if (fout.h < 0) { say("cannot write ", part_path, NULL); return ZF_E_IO; }
		fout.wbuf = wbuf;
		io_init(&outio, &fout);
		const char *skip[2] = { out_path, part_path };
		r = pack_folder(in_path, to, &outio, skip);
		if (!io_close(&outio) && r >= 0) r = ZF_E_IO;
		if (r >= 0) {
			int err = 0;
			fs_unlink(out_path);
			if (!fs_rename(part_path, out_path, &err)) r = ZF_E_IO;
		}
		if (r < 0) fs_unlink(part_path);
		*from_out = to;
		if (r >= 0 && pack_skipped) {
			char b[64];
			snprintf(b, sizeof b, "%lu entries left out (too deep, or a name too long)", (unsigned long)pack_skipped);
			say(b, NULL, NULL);
		}
		return r;
	}

	// the input
	memset(&fin, 0, sizeof fin);
	fin.h = fs_open_read(in_path);
	if (fin.h < 0) { say("cannot open ", in_path, NULL); return ZF_E_IO; }
	io_init(&in, &fin);
	in.size = (uint32_t)fs_size(in_path);
	from = zf_detect(in_path, &in);
	*from_out = from;
	if (from == ZF_T_UNKNOWN) {
		say("what kind of file is ", in_path, "?");
		fs_close_handle(fin.h);
		return ZF_E_FORMAT;
	}
	if (open_after && !open_target(from, to_name, sizeof to_name)) {
		fs_close_handle(fin.h);
		return ZF_E_IO;
	}

	if (zf_type_is_arch(from) && from != ZF_T_GZ) {
		// into a folder, made if need be
		snprintf(dir_root, sizeof dir_root, "%s", out_path);
		int n = (int)strlen(dir_root);
		while (n > 1 && dir_root[n - 1] == '/') dir_root[--n] = 0;
		if (!is_dir(dir_root) && !fs_mkdir(dir_root)) {
			fs_close_handle(fin.h);
			say("cannot make ", dir_root, NULL);
			return ZF_E_IO;
		}
		zf_dir_t d = { NULL, d_open, d_close, d_mkdir, d_remove };
		r = zf_convert(&in, from, NULL, ZF_T_UNKNOWN, &d);
		if (r < 0 && dir_file.h >= 0) { fs_close_handle(dir_file.h); dir_file.h = -1; }
		fs_close_handle(fin.h);
		return r;
	}

	zf_type_t to = from == ZF_T_GZ ? ZF_T_UNKNOWN
		: to_name[0] ? zf_type_from_name(to_name) : zf_detect(out_path, NULL);
	if (!zf_can_convert(from, to)) {
		fs_close_handle(fin.h);
		say("cannot make ", to == ZF_T_UNKNOWN ? "that" : zf_type_name(to), NULL);
		return ZF_E_UNSUPP;
	}
	snprintf(part_path, sizeof part_path, "%s.part", out_path);
	memset(&fout, 0, sizeof fout);
	fout.h = fs_open_write(part_path);
	if (fout.h < 0) {
		fs_close_handle(fin.h);
		say("cannot write ", part_path, NULL);
		return ZF_E_IO;
	}
	fout.wbuf = wbuf;
	io_init(&outio, &fout);
	r = zf_convert(&in, from, &outio, to, NULL);
	fs_close_handle(fin.h);
	if (!io_close(&outio) && r >= 0) r = ZF_E_IO;
	if (r >= 0) {
		int err = 0;
		fs_unlink(out_path);
		if (!fs_rename(part_path, out_path, &err)) r = ZF_E_IO;
	}
	if (r < 0) fs_unlink(part_path);
	return r;
}

// -- the window: convert run by itself --
//
// From: a file or a folder (Choose...), To: the formats it can become
// (< >), Convert: where it goes (a Save dialog), then the work with its
// progress in the same window. docs/convert.md, "The convert window".

#define GW_W		300
#define GW_H		118
#define GW_LABEL_X	8
#define GW_VAL_X	40
#define GW_ROW1		8
#define GW_ROW2		28
#define GW_STATUS	50
#define GW_BTN_W	60

enum { G_CHOOSE, G_PREV, G_NEXT, G_CONVERT, G_OPEN, G_COUNT };

static z_widget_t gw[G_COUNT];
static z_widget_set_t gset;
static z_dialog_ctx_t gctx;
static bool gui_quit, gui_busy;
static int gui_act = -1;		// a button pressed, done from the loop
static char gui_status[96];
static char gui_done[Z_CONV_PATH_MAX];	// the last result, for Open
static zf_type_t gui_done_t;
static bool gui_in_dir;
static zf_type_t gui_from;

typedef struct { const char *ext, *label; } fmt_t;

static const fmt_t docs_out[] = {
	{ "md", "Markdown (.md)" }, { "txt", "Plain text (.txt)" }, { "html", "Web page (.html)" },
	{ "rtf", "Rich text (.rtf)" }, { "docx", "Word (.docx)" }, { "odt", "OpenDocument (.odt)" },
};
static const fmt_t tabs_out[] = {
	{ "zss", "Sheet (.zss)" }, { "csv", "Comma-separated (.csv)" }, { "tsv", "Tab-separated (.tsv)" },
	{ "xlsx", "Excel (.xlsx)" }, { "ods", "OpenDocument (.ods)" },
};
static const fmt_t dirs_out[] = {
	{ "zip", "Zip archive (.zip)" }, { "tar", "Tar archive (.tar)" }, { "tar.gz", "Gzipped tar (.tar.gz)" },
};
static const fmt_t arch_out[] = { { "", "A folder of its files" } };
static const fmt_t gz_out[] = { { "", "The file inside" } };

static const fmt_t *gui_fmts;
static int gui_nfmts, gui_fmt;

static void gui_status_set(const char *a, const char *b) {
	snprintf(gui_status, sizeof gui_status, "%s%s", a, b ? b : "");
}

// What the chosen input can become, and the likeliest first: the other
// system's format for this system's own, this system's for the other's.
static void gui_choose_fmts(void) {
	gui_fmts = NULL;
	gui_nfmts = gui_fmt = 0;
	if (!in_path[0]) return;
	gui_in_dir = is_dir(in_path);
	if (gui_in_dir) {
		gui_fmts = dirs_out; gui_nfmts = 3;
		return;
	}
	static zf_io_t io;
	static fio_t f;
	memset(&f, 0, sizeof f);
	f.h = fs_open_read(in_path);
	if (f.h < 0) { gui_status_set("Cannot read it", NULL); return; }
	io_init(&io, &f);
	io.size = (uint32_t)fs_size(in_path);
	gui_from = zf_detect(in_path, &io);
	fs_close_handle(f.h);
	if (zf_type_is_doc(gui_from)) {
		gui_fmts = docs_out; gui_nfmts = 6;
		gui_fmt = gui_from == ZF_T_TXT || gui_from == ZF_T_MD ? 4 : 0;
	} else if (zf_type_is_tab(gui_from)) {
		gui_fmts = tabs_out; gui_nfmts = 5;
		gui_fmt = gui_from == ZF_T_ZSS || gui_from == ZF_T_CSV ? 3 : 0;
	} else if (gui_from == ZF_T_GZ) {
		gui_fmts = gz_out; gui_nfmts = 1;
	} else if (zf_type_is_arch(gui_from)) {
		gui_fmts = arch_out; gui_nfmts = 1;
	} else {
		gui_status_set("convert does not read this kind of file", NULL);
	}
}

static void gui_text_fit(int x, int y, int w, const char *t) {
	char b[96];
	int max = w / z_font_5x8.w;
	int n = (int)strlen(t);
	if (max <= 0) return;
	if (max >= (int)sizeof b) max = (int)sizeof b - 1;
	if (n > max) {
		// the end of a path says more than its start
		snprintf(b, sizeof b, "..%s", t + n - (max - 2));
	} else {
		snprintf(b, sizeof b, "%s", t);
	}
	z_win_draw_text(&win, x, y, b, 1, &z_font_5x8);
}

static void gui_draw(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	z_win_fill_rect(&win, 0, 0, cw, ch - Z_WIN_GRIP_INSET, 0);
	z_win_fill_rect(&win, 0, ch - Z_WIN_GRIP_INSET, cw - Z_WIN_GRIP_INSET, Z_WIN_GRIP_INSET, 0);
	z_win_draw_text(&win, GW_LABEL_X, GW_ROW1 + 3, "From", 1, &z_font_5x8);
	z_win_draw_text(&win, GW_LABEL_X, GW_ROW2 + 3, "To", 1, &z_font_5x8);
	gui_text_fit(GW_VAL_X, GW_ROW1 + 3, gw[G_CHOOSE].x - GW_VAL_X - 6,
		in_path[0] ? in_path : "(choose a file or a folder)");
	if (gui_nfmts) {
		const char *l = gui_fmts[gui_fmt].label;
		int x0 = gw[G_PREV].x + gw[G_PREV].w, x1 = gw[G_NEXT].x;
		int tw = (int)strlen(l) * z_font_5x8.w;
		z_win_draw_text(&win, x0 + (x1 - x0 - tw) / 2, GW_ROW2 + 3, l, 1, &z_font_5x8);
	}
	if (gui_busy) draw_bar();
	else gui_text_fit(GW_LABEL_X, GW_STATUS, cw - 2 * GW_LABEL_X, gui_status);
	gw[G_PREV].enabled = gw[G_NEXT].enabled = gui_nfmts > 1 && !gui_busy;
	gw[G_CHOOSE].enabled = !gui_busy;
	gw[G_CONVERT].enabled = gui_busy || gui_nfmts > 0;
	gw[G_CONVERT].label = gui_busy ? "Cancel" : "Convert";
	gw[G_OPEN].enabled = !gui_busy && gui_done[0];
	z_widget_invalidate(&gset);
	z_widget_draw_all(&gset, true);
}

// The window's own messages; false for what on_msg() does as always
// (redraws, the port).
static bool gui_msg(z_msg_t *m) {
	switch (m->subject) {
	case Z_WM_CLOSE:
		if (gui_busy) cancelled = true;
		else gui_quit = true;
		return true;
	case Z_WM_KEY: {
		if (m->obj.type != Z_UINT32 || !Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32)) return true;
		uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32);
		uint8_t mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(m->obj.val.uint32);
		if (gui_busy) { if (k == 0x1b) cancelled = true; return true; }
		if (k == 0x1b) gui_quit = true;
		else if (k == Z_KEY_HELP) gui_act = G_COUNT;		// help
		else if (k == '\t') { z_widget_focus_next(&gset, (mods & Z_KBD_MOD_SHIFT) != 0); z_widget_draw_all(&gset, false); }
		else if (k == Z_KEY_LEFT) gui_act = G_PREV;
		else if (k == Z_KEY_RIGHT) gui_act = G_NEXT;
		else if (k == 0x0d || k == ' ') gui_act = gset.focused >= 0 ? z_widget_key_activate(&gset) : G_CONVERT;
		else if (k == 'o' && (mods & Z_KBD_MOD_CTRL)) gui_act = G_CHOOSE;
		return true;
	}
	case Z_WM_MOUSE: {
		if (m->obj.type != Z_UINT32) return true;
		int cx, cy;
		z_win_mouse_content_xy(&win, m->obj.val.uint32, &cx, &cy);
		int a = z_widget_mouse(&gset, cx, cy, (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(m->obj.val.uint32));
		if (a >= 0) {
			if (gui_busy) { if (a == G_CONVERT) cancelled = true; }
			else gui_act = a;
		}
		return true;
	}
	default:
		return false;
	}
}

static void gui_parent(z_msg_t *m, void *user) { on_msg(m, user); }

static void dir_of(const char *p, char *o, int cap) {
	snprintf(o, (size_t)cap, "%s", p);
	char *s = strrchr(o, '/');
	if (s && s != o) *s = 0;
	else if (s) s[1] = 0;
}

static void gui_convert(void) {
	char start[Z_CONV_PATH_MAX], suggest[96], stem[Z_CONV_PATH_MAX], dest[Z_CONV_PATH_MAX];
	const fmt_t *f = &gui_fmts[gui_fmt];
	dir_of(in_path, start, sizeof start);
	strip_ext(base_name(in_path), stem, sizeof stem, gui_in_dir ? ZF_T_UNKNOWN : gui_from);
	if (gui_in_dir) snprintf(stem, sizeof stem, "%s", base_name(in_path));
	if (f->ext[0]) snprintf(suggest, sizeof suggest, "%.80s.%s", stem, f->ext);
	else if (gui_from == ZF_T_GZ) strip_ext(base_name(in_path), suggest, sizeof suggest, ZF_T_GZ);
	else snprintf(suggest, sizeof suggest, "%.90s", stem);
	if (!z_dialog_save(&gctx, start, suggest, dest, sizeof dest)) { gui_draw(); return; }
	// the ending typed wins over the one chosen, as in files' Convert
	if (f->ext[0]) {
		zf_type_t want = zf_type_from_name(f->ext), typed = zf_detect(dest, NULL);
		if (typed == ZF_T_UNKNOWN) {
			int n = (int)strlen(dest);
			snprintf(dest + n, sizeof dest - (size_t)n, ".%s", f->ext);
		} else if (typed != want) {
			f = NULL;		// the ending typed picks the format
		}
	}
	if (!strcmp(dest, in_path)) { gui_status_set("That is the file itself", NULL); gui_draw(); return; }
	bool d = false;
	if (exists(dest, &d) && (!f || f->ext[0])) {
		if (d) { gui_status_set("A folder of that name is there", NULL); gui_draw(); return; }
		if (z_dialog_confirm(&gctx, "Convert", "Replace the file that\nis already there?",
			Z_DIALOG_YES_NO) != Z_DIALOG_YES) { gui_draw(); return; }
	}
	snprintf(out_path, sizeof out_path, "%s", dest);
	snprintf(to_name, sizeof to_name, "%s", f ? f->ext : "");
	open_after = false;

	gui_busy = true;
	cancelled = false;
	permille = 0;
	t_ui = (uint32_t)-1;
	gui_done[0] = 0;
	heap_init();
	gui_draw();
	zf_type_t from;
	int r = convert_one(&from);
	z_arena_set(0, NULL, NULL);
	gui_busy = false;
	if (r >= 0) {
		snprintf(gui_done, sizeof gui_done, "%s", out_path);
		gui_done_t = gui_in_dir ? ZF_T_UNKNOWN : gui_from;
		gui_status_set("Saved ", out_path);
		if (pack_skipped) gui_status_set("Saved, but some entries were left out: ", out_path);
		if (zf_tab_dropped) gui_status_set("Saved; cells beyond A1:Z999 left out: ", out_path);
	} else {
		gui_status_set(r == ZF_E_CANCEL ? "Stopped: nothing written" : "Not possible: ",
			r == ZF_E_CANCEL ? NULL : zf_strerror(r));
	}
	gui_draw();
}

static void gui_open(void) {
	if (!gui_done[0]) return;
	const char *app;
	if (is_dir(gui_done)) app = "files";
	else if (zf_type_is_arch(zf_detect(gui_done, NULL))) {
		// an archive just made: show it in its folder, not unpack it again
		static char d[Z_CONV_PATH_MAX];
		dir_of(gui_done, d, sizeof d);
		z_launch_arg_set(d);
		z_proc_run("files");
		return;
	} else app = z_ftype_app_for(gui_done);
	if (!app) { gui_status_set("Nothing here opens that kind of file", NULL); gui_draw(); return; }
	z_launch_arg_set(gui_done);
	z_proc_run(app);
}

static void gui_run(void) {
	zf_cancel_poll = cancel_poll;
	zf_progress = progress;
	gui = true;
	quiet = true;
	ui = true;
	bar_y = GW_STATUS;
	if (z_win_create_cb(&win, "convert", GW_W, GW_H, -1, -1, Z_WIN_FLAG_CLOSE_ICON, on_msg, NULL) != Z_OK)
		return;
	win_up = true;
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	memset(gw, 0, sizeof gw);
	for (int i = 0; i < G_COUNT; i++) { gw[i].type = Z_WIDGET_BUTTON; gw[i].enabled = true; gw[i].h = 14; }
	gw[G_CHOOSE].label = "Choose..."; gw[G_CHOOSE].x = (int16_t)(cw - 8 - GW_BTN_W); gw[G_CHOOSE].y = GW_ROW1;
	gw[G_CHOOSE].w = GW_BTN_W;
	gw[G_PREV].label = "<"; gw[G_PREV].x = GW_VAL_X; gw[G_PREV].y = GW_ROW2; gw[G_PREV].w = 14;
	gw[G_NEXT].label = ">"; gw[G_NEXT].x = (int16_t)(cw - 8 - 14); gw[G_NEXT].y = GW_ROW2; gw[G_NEXT].w = 14;
	int by = ch - 8 - 14 - Z_WIN_GRIP_INSET;
	gw[G_CONVERT].label = "Convert"; gw[G_CONVERT].x = (int16_t)(cw - 8 - GW_BTN_W); gw[G_CONVERT].y = (int16_t)by;
	gw[G_CONVERT].w = GW_BTN_W;
	gw[G_OPEN].label = "Open"; gw[G_OPEN].x = (int16_t)(cw - 8 - 2 * GW_BTN_W - 6); gw[G_OPEN].y = (int16_t)by;
	gw[G_OPEN].w = GW_BTN_W;
	z_widget_set_init(&gset, gw, G_COUNT, &win);
	z_widget_focus_set(&gset, G_CHOOSE);
	memset(&gctx, 0, sizeof gctx);
	gctx.parent = &win;
	gctx.on_msg = gui_parent;
	gui_status_set("Choose a file to convert, or a folder to pack", NULL);
	gui_draw();

	while (!gui_quit) {
		z_msg_t m;
		if (z_msg_recv(&m, 0) == Z_OK) on_msg(&m, NULL);
		int a = gui_act;
		gui_act = -1;
		switch (a) {
		case G_CHOOSE: {
			static char pick[Z_CONV_PATH_MAX], start[Z_CONV_PATH_MAX];
			if (in_path[0]) dir_of(in_path, start, sizeof start);
			if (z_dialog_open_any(&gctx, in_path[0] ? start : NULL, pick, sizeof pick)) {
				snprintf(in_path, sizeof in_path, "%s", pick);
				gui_status[0] = 0;
				gui_done[0] = 0;
				gui_choose_fmts();
				if (gui_nfmts) z_widget_focus_set(&gset, G_CONVERT);
			}
			gui_draw();
			break;
		}
		case G_PREV:
		case G_NEXT:
			if (gui_nfmts > 1) {
				gui_fmt = (gui_fmt + (a == G_NEXT ? 1 : gui_nfmts - 1)) % gui_nfmts;
				gui_draw();
			}
			break;
		case G_CONVERT:
			if (gui_nfmts) gui_convert();
			break;
		case G_OPEN:
			gui_open();
			break;
		case G_COUNT:
			z_dialog_help(&gctx, "convert help",
				"Choose a file to convert, or a folder to pack.\n\n"
				"To: Left and Right, or < and >, pick what it becomes: "
				"a document another document, a table another table, "
				"an archive a folder of its files, a folder a .zip, "
				".tar or .tar.gz.\n\n"
				"Convert asks where the result goes; Esc stops it. "
				"Open shows the result.\n\n"
				"Tab moves between the buttons, Enter presses one, "
				"Ctrl+O chooses.", "convert");
			gui_draw();
			break;
		default:
			break;
		}
	}
	z_win_destroy(&win);
	exit(0);
}

int main(void) {
	static char arg[Z_WM_ARG_MAX + 1], argbuf[2 * Z_WM_ARG_MAX + 2];
	char *argv[8];
	int argc;

	t_start = z_uptime_ticks();
	z_launch_arg_take(arg, sizeof arg);
	argc = z_args_split(arg, argbuf, sizeof argbuf, argv, NULL, 8);

	bool job = false;
	int i = 0;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) snprintf(to_name, sizeof to_name, "%s", argv[++i]);
		else if (!strcmp(argv[i], "-j") && i + 1 < argc) { job = read_job(argv[++i]); if (!job) return 1; }
		else if (!strcmp(argv[i], "-l")) { out_open(); list_formats(); out_close(); return 0; }
		else { out_open(); usage(); out_close(); return 2; }
	}
	if (!job) {
		if (argc == 0) gui_run();			// run by itself: the window
		out_open();
		if (argc - i == 1) {
			// opened from files (or `convert x.docx`): a person is waiting
			snprintf(in_path, sizeof in_path, "%s", argv[i]);
			ui = open_after = true;
		} else if (argc - i == 2) {
			snprintf(in_path, sizeof in_path, "%s", argv[i]);
			snprintf(out_path, sizeof out_path, "%s", argv[i + 1]);
		} else {
			usage();
			out_close();
			return 2;
		}
	}
	quiet = job;
	bool folder = is_dir(in_path);
	snprintf(ui_title, sizeof ui_title, "%s %s", folder ? "Packing" : open_after ? "Opening" : "Converting",
		base_name(in_path));
	zf_cancel_poll = cancel_poll;
	zf_progress = progress;
	heap_init();

	zf_type_t from;
	int r = convert_one(&from);
	if (r < 0 && !job && !said) say(zf_strerror(r), NULL, NULL);
	if (zf_tab_dropped && !job) {
		char b[64];
		snprintf(b, sizeof b, "%lu cells outside A1:Z999 left out", (unsigned long)zf_tab_dropped);
		say(b, NULL, NULL);
	}
	if (r >= 0 && open_after && !cancelled) open_result(from);
	return finish(r);
}
