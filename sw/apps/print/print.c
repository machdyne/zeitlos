/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * print -- the print service. docs/print.md.
 *
 *   print -j JOB -c PID:SERIAL     print a job an app wrote (zprint.h)
 *   print                          ask the printer in zeitlos.cfg what it
 *   print ADDRESS                  takes, and show it (setting up)
 *
 * The work is run.c's, the same as in the host tests; this file is the
 * board around it: the job file, the window (what it is doing, Cancel,
 * any error), net's raw socket as the transport, and the caller.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwin.h"
#include "../../common/zwm.h"
#include "../../common/zfont.h"
#include "../../common/zfsapp.h"
#include "../../common/zport.h"
#include "../../common/znet.h"
#include "../../common/zdns.h"
#include "../../common/zcfg.h"
#include "../../common/zpool.h"
#include "../../common/zprint.h"
#include "../../common/zargs.h"

#include "run.h"
#include "res.h"

// apps.print.format: auto, pwg, pdf or ps
static const char *fmt_pref(void) {
	static char f[16];
	z_cfg_get("apps.print.format", f, sizeof(f));
	return f[0] ? f : "auto";
}

// -- the window -------------------------------------------------------------------

#define WIN_W		300
#define WIN_H		60
#define INFO_H		112
#define BAR_Y		28
#define BAR_H		8
#define BTN_W		50
#define BTN_H		14

static z_win_t win;
static bool win_up, cancelled, dismissed;
static bool done_shown;
static int permille;
static char w_title[64];			// "A tour of slide"
static char w_line[96];				// what it is doing, or how it ended
static char w_info[8][64];			// a printer's answer, for print ADDRESS
static int w_ninfo;

static int btn_x(void) { return z_win_content_w(&win) - BTN_W - 6; }
static int btn_y(void) { return z_win_content_h(&win) - BTN_H - 6; }

static void text(int x, int y, const char *s) { z_win_draw_text(&win, x, y, s, 1, &z_font_5x8); }

static void draw_button(const char *label) {
	int x = btn_x(), y = btn_y();
	int tw = (int)strlen(label) * z_font_5x8.w;
	z_win_fill_rect(&win, x, y, BTN_W, BTN_H, 1);
	z_win_fill_rect(&win, x + 1, y + 1, BTN_W - 2, BTN_H - 2, 0);
	text(x + (BTN_W - tw) / 2, y + (BTN_H - z_font_5x8.h) / 2, label);
}

static void draw_bar(void) {
	int w = z_win_content_w(&win) - 12, f;
	if (w < 4 || done_shown || w_ninfo) return;
	z_win_fill_rect(&win, 6, BAR_Y, w, BAR_H, 1);
	z_win_fill_rect(&win, 7, BAR_Y + 1, w - 2, BAR_H - 2, 0);
	f = (int)((long)(w - 4) * permille / 1000);
	if (f > 0) z_win_fill_rect(&win, 8, BAR_Y + 2, f, BAR_H - 4, 1);
}

static void draw_line(void) {
	int cw = z_win_content_w(&win);
	z_win_fill_rect(&win, 6, 15, cw - 12, 9, 0);
	text(6, 16, w_line);
}

static void draw_all(void) {
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win), i;
	z_win_fill_rect(&win, 0, 0, cw, ch - Z_WIN_GRIP_INSET, 0);
	z_win_fill_rect(&win, 0, ch - Z_WIN_GRIP_INSET, cw - Z_WIN_GRIP_INSET, Z_WIN_GRIP_INSET, 0);
	text(6, 4, w_title);
	text(6, 16, w_line);
	for (i = 0; i < w_ninfo; i++) text(6, 28 + i * 10, w_info[i]);
	draw_bar();
	draw_button(done_shown ? "OK" : "Cancel");
}

// -- the transport: net's raw socket ---------------------------------------------

static uint32_t net_pid;
static z_port_t sock;
static bool sock_open, sock_wait, sock_refused, peer_closed;
static char sock_err[64];
static uint8_t rx[4096];
static int rx_len, rx_pos;

static void on_msg(z_msg_t *m, void *user);

static void pump(void) {
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) on_msg(&m, NULL);
}

static void on_port(z_msg_t *m) {
	switch (m->subject) {
	case Z_PORT_CONNECTED:
		if (!sock_wait) break;
		sock.peer_pid = m->from;
		sock.conn_id = m->obj.type == Z_UINT32 ? m->obj.val.uint32 : 0;
		sock.connected = true;
		sock_wait = false;
		sock_open = true;
		break;
	case Z_PORT_REFUSED:
		if (!sock_wait) break;
		sock_wait = false;
		sock_refused = true;
		if (m->obj.type == Z_STR && m->obj.val.str) {
			int i;
			for (i = 0; i < (int)sizeof(sock_err) - 1 && m->obj.val.str[i]; i++) sock_err[i] = m->obj.val.str[i];
			sock_err[i] = 0;
		}
		break;
	case Z_PORT_DATA: {
		uint32_t len = z_blob_len(&m->obj);
		const uint8_t *d = z_blob_data(&m->obj);
		if (sock_open && m->tag == sock.conn_id && d && len) {
			// keep what fits; a printer's answer is small (zprint reads
			// it out before more arrives)
			if (rx_pos == rx_len) rx_pos = rx_len = 0;
			if (len > sizeof(rx) - (uint32_t)rx_len) len = sizeof(rx) - (uint32_t)rx_len;
			memcpy(rx + rx_len, d, len);
			rx_len += (int)len;
		}
		z_port_send_ack(m);			// always, and after copying
		break;
	}
	case Z_PORT_DATA_ACK:
		if (sock_open) z_port_handle_ack(&sock, m);
		break;
	case Z_PORT_CLOSE:
		if (sock_open && m->tag == sock.conn_id) { peer_closed = true; sock.connected = false; }
		break;
	default:
		break;
	}
}

static bool t_connect(void *ctx, const ipp_url_t *u, char *err, int n) {
	uint32_t ip, start;
	z_obj_t arg;
	(void)ctx;
	memset(&sock, 0, sizeof(sock));
	sock_open = sock_refused = peer_closed = false;
	rx_len = rx_pos = 0;
	sock_err[0] = 0;
	if (!net_pid && !z_pid_lookup("net0", &net_pid)) {
		run_say(err, n, "the network is not running: run net", NULL);
		return false;
	}
	// z_resolve_host() blocks; the window says why first
	if (win_up) draw_line();
	if (!z_resolve_host(u->host, &ip, NULL, 0)) {
		run_say(err, n, "cannot find ", u->host);
		return false;
	}
	arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(ip));
	z_map_set(&arg, "port", z_obj_uint32((uint32_t)u->port));
	sock_wait = true;
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);
	start = z_uptime_ticks();
	// net answers when the TCP handshake does: up to its retry budget
	while (sock_wait && !cancelled && z_uptime_ticks() - start < Z_TICK_HZ * 45u) {
		z_proc_wait(Z_TICK_HZ / 20);
		pump();
	}
	if (sock_open) return true;
	if (cancelled) run_say(err, n, "cancelled", NULL);
	else if (sock_refused) run_say(err, n, sock_err[0] ? sock_err : "the printer refused the connection", NULL);
	else run_say(err, n, "no answer from ", u->host);
	sock_wait = false;
	return false;
}

static bool s_write(out_t *o, const void *d, uint32_t n) {
	const uint8_t *p = (const uint8_t *)d;
	(void)o;
	while (n) {
		uint32_t k = n > 1024 ? 1024 : n;
		if (cancelled || peer_closed || !sock_open) return false;
		if (z_port_send(&sock, p, k) == Z_OK) {
			p += k;
			n -= k;
			continue;
		}
		// eight sends unread: wait for an ack (net holds what the TCP
		// queue cannot take yet)
		z_proc_wait(1);
		pump();
	}
	return true;
}

static out_t sock_out = { s_write, NULL, 0, false };

static int t_read(void *ctx, uint8_t *buf, int max) {
	uint32_t start = z_uptime_ticks();
	(void)ctx;
	// a printer answers once it has taken the job; a slow one, a few
	// minutes later
	while (rx_pos == rx_len) {
		if (peer_closed || cancelled || !sock_open) return 0;
		if (z_uptime_ticks() - start > Z_TICK_HZ * 300u) return -1;
		z_proc_wait(Z_TICK_HZ / 10);
		pump();
	}
	{
		int k = rx_len - rx_pos;
		if (k > max) k = max;
		memcpy(buf, rx + rx_pos, (size_t)k);
		rx_pos += k;
		return k;
	}
}

static bool linger;		// a socket printer: give net time to send the last bytes

static void t_close(void *ctx) {
	(void)ctx;
	if (!sock_open) return;
	// every byte read by net; then, for a printer that never answers, a
	// moment for its 2KB queue to reach the wire (a CLOSE drops it)
	z_port_drain(&sock, Z_TICK_HZ * 10);
	if (linger) {
		uint32_t start = z_uptime_ticks();
		while (!peer_closed && z_uptime_ticks() - start < Z_TICK_HZ * 2u) {
			z_proc_wait(Z_TICK_HZ / 20);
			pump();
		}
	}
	z_port_close(&sock);
	sock_open = false;
}

// -- the caller, progress ----------------------------------------------------------

static uint32_t caller_pid, caller_serial;
static uint32_t last_ui, last_sent;

static void send(uint32_t subject, uint32_t tag) {
	z_msg_t m;
	if (!caller_pid || z_proc_serial(caller_pid) != caller_serial) return;
	memset(&m, 0, sizeof m);
	m.to = caller_pid;
	m.subject = subject;
	m.tag = tag;
	m.obj = z_obj_none();
	z_msg_send(&m);
}

static void status(const char *a, const char *b) {
	run_say(w_line, sizeof(w_line), a, b);
	if (win_up) draw_line();
}

static bool tick(void *ctx, int sheet, int sheets, int y, int h) {
	uint32_t now = z_uptime_ticks();
	(void)ctx;
	if (sheets > 0) permille = (int)(((int64_t)sheet * 1000 + (int64_t)y * 1000 / (h ? h : 1)) / sheets);
	if (now - last_ui >= Z_TICK_HZ / 20) {
		char b[48];
		int k = 0;
		last_ui = now;
		k += fmt_int(b + k, sheet + 1);
		run_say(b + k, (int)sizeof(b) - k, " of ", NULL);
		k = (int)strlen(b);
		fmt_int(b + k, sheets);
		status("Sheet ", b);
		pump();
		if (win_up) draw_bar();
	}
	if (now - last_sent >= Z_TICK_HZ / 10) {
		last_sent = now;
		send(Z_PRINT_PROGRESS, (uint32_t)permille);
	}
	return !cancelled;
}

static bool wait_for(void *ctx, int ms, const char *why) {
	uint32_t start = z_uptime_ticks(), need = (uint32_t)ms * Z_TICK_HZ / 1000;
	(void)ctx;
	status("Waiting: ", why);
	while (!cancelled && z_uptime_ticks() - start < need) {
		z_proc_wait(Z_TICK_HZ / 10);
		pump();
	}
	status("Trying again", NULL);
	return !cancelled;
}

// -- messages -------------------------------------------------------------------------

static void on_msg(z_msg_t *m, void *user) {
	(void)user;
	switch (m->subject) {
	case Z_PORT_CONNECTED: case Z_PORT_REFUSED: case Z_PORT_DATA:
	case Z_PORT_DATA_ACK: case Z_PORT_CLOSE:
		on_port(m);
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
		if (done_shown) dismissed = true; else cancelled = true;
		break;
	case Z_WM_KEY:
		if (win_up && m->obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(m->obj.val.uint32)) {
			uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(m->obj.val.uint32);
			if (k == 0x1b || k == 0x0d || k == ' ') { if (done_shown) dismissed = true; else cancelled = true; }
		}
		break;
	case Z_WM_MOUSE:
		if (win_up && m->obj.type == Z_UINT32) {
			int cx, cy;
			z_win_mouse_content_xy(&win, m->obj.val.uint32, &cx, &cy);
			if ((Z_WM_UNPACK_MOUSE_BUTTONS(m->obj.val.uint32) & Z_MOUSE_BTN_LEFT) &&
				cx >= btn_x() && cx < btn_x() + BTN_W && cy >= btn_y() && cy < btn_y() + BTN_H) {
				if (done_shown) dismissed = true; else cancelled = true;
			}
		}
		break;
	default:
		break;
	}
}

static void win_open(int h) {
	if (z_win_create_cb(&win, "print", WIN_W, (uint32_t)h, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON, on_msg, NULL) != Z_OK) return;
	win_up = true;
	draw_all();
}

// What the printer said (print ADDRESS) is to be read: it stays up
// until OK, like a failure.
static bool hold_open;

// The end: the outcome stays up until it is dismissed -- a few seconds
// for a job that worked, until OK for one that did not and for what a
// probe found.
static void finish(bool ok, int code) {
	uint32_t start = z_uptime_ticks();
	z_msg_t m;
	send(Z_PRINT_DONE, (uint32_t)code);
	done_shown = true;
	if (win_up) draw_all();
	while (win_up && !dismissed && (!ok || hold_open || z_uptime_ticks() - start < Z_TICK_HZ * 4u)) {
		if (z_msg_recv(&m, Z_TICK_HZ / 4) == Z_OK) on_msg(&m, NULL);
	}
	if (win_up) z_win_destroy(&win);
	z_arena_set(0, NULL, NULL);
	exit(ok ? 0 : 1);
}

// -- files -------------------------------------------------------------------------------

static int fh = -1;
static uint8_t fbuf[2048];
static int fn;

static bool f_write(out_t *o, const void *d, uint32_t n) {
	const uint8_t *p = (const uint8_t *)d;
	(void)o;
	while (n) {
		uint32_t k = (uint32_t)sizeof(fbuf) - (uint32_t)fn;
		if (k > n) k = n;
		memcpy(fbuf + fn, p, k);
		fn += (int)k;
		p += k;
		n -= k;
		if (fn == (int)sizeof(fbuf)) {
			if (fs_write_chunk(fh, fbuf, fn) != fn) return false;
			fn = 0;
		}
	}
	return true;
}

static uint8_t *load(const char *path, uint32_t *len) {
	int n = fs_size((char *)path);
	uint8_t *b;
	if (n <= 0) return NULL;
	b = pool_alloc((size_t)n + 1);
	if (!b) return NULL;
	if (fs_read_file((char *)path, (char *)b, n) != n) { pool_free(b); return NULL; }
	*len = (uint32_t)n;
	return b;
}

// -- memory ----------------------------------------------------------------------------------

static bool mem_setup(void) {
	static const uint32_t sizes[] = { 4u << 20, 2u << 20, 3u << 19 };
	static uint8_t scratch[48 * 1024];
	unsigned i;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		void *base = NULL;
		uint32_t got = 0;
		if (z_arena_set(sizes[i], &base, &got) == Z_ARENA_OK && base && got >= sizes[i]) {
			uint8_t *cache;
			pool_init(base, got);
			zc_scratch(scratch, sizeof(scratch));
			cache = pool_alloc(got >= (2u << 20) ? 192 * 1024 : 64 * 1024);
			if (cache) zc_text_cache(cache, got >= (2u << 20) ? 192 * 1024 : 64 * 1024);
			res_init(load, got >= (4u << 20) ? (1u << 20) : (got >> 2));
			return true;
		}
	}
	return false;
}

// -- setting up: what does the printer say? ---------------------------------------------------

static void probe(const char *addr) {
	ipp_url_t u;
	ipp_info_t info;
	transport_t t;
	run_t r;
	char b[64];
	int k;
	run_say(w_title, sizeof(w_title), "Printer: ", addr);
	hold_open = true;
	win_open(INFO_H);
	status("Asking it what it takes", NULL);
	if (!ipp_url(addr, &u) || u.kind == URL_SOCKET) {
		status(u.kind == URL_SOCKET ? "A socket printer takes PostScript; it says nothing more."
			: "Not an address: ipp://ADDRESS/ipp/print", NULL);
		finish(u.kind == URL_SOCKET, 0);
	}
	memset(&t, 0, sizeof(t));
	t.connect = t_connect; t.close = t_close; t.sock = &sock_out; t.read = t_read;
	memset(&r, 0, sizeof(r));
	if (!run_probe(&r, &t, &u, &info)) { status(r.err, NULL); finish(false, Z_PRINT_E_FAILED); }
	status(info.make[0] ? info.make : "(it gives no name)", NULL);
	run_say(w_info[w_ninfo++], 64, info.state == 3 ? "Ready" : info.state == 4 ? "Printing" : "Stopped: ", info.state == 5 ? info.state_reasons : NULL);
	run_say(b, sizeof(b), info.pdf ? "PDF " : "", info.pwg ? "PWG-raster " : "");
	run_say(w_info[w_ninfo++], 64, "Takes: ", b);
	k = (int)strlen(w_info[w_ninfo - 1]);
	run_say(w_info[w_ninfo - 1] + k, 64 - k, info.ps ? "PostScript " : "", info.urf ? "URF" : "");
	if (info.ndpi) {
		int i;
		k = 0;
		for (i = 0; i < info.ndpi && k < 40; i++) { k += fmt_int(b + k, info.dpi[i]); b[k++] = ' '; }
		b[k] = 0;
		run_say(w_info[w_ninfo++], 64, "Raster dpi: ", b);
	}
	run_say(w_info[w_ninfo++], 64, "Paper: ", info.media_default);
	if (info.nready) run_say(w_info[w_ninfo++], 64, "Loaded: ", info.media_ready[0]);
	{
		const char *f = !strcmp(fmt_pref(), "auto") ? (info.pdf ? "PDF" : info.pwg ? "PWG raster" : "PostScript") : fmt_pref();
		run_say(w_info[w_ninfo++], 64, "This will print as ", f);
	}
	if (win_up) draw_all();
	finish(true, 0);
}

// -- printing a job ------------------------------------------------------------------------------

int main(void) {
	static char arg[Z_WM_ARG_MAX + 1], argbuf[Z_WM_ARG_MAX + 1];
	static uint32_t start[JOB_MAX_PAGES], bytes[JOB_MAX_PAGES];
	char *argv[8];
	int argc, i;
	const char *jobf = NULL, *addr = NULL;
	char printer[128], dest[200];
	uint8_t *data;
	uint32_t len;
	job_t j;
	run_t r;
	transport_t t;
	ipp_url_t u;
	bool ok = false;

	z_launch_arg_take(arg, sizeof arg);
	argc = z_args_split(arg, argbuf, sizeof argbuf, argv, NULL, 8);
	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-j") && i + 1 < argc) jobf = argv[++i];
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			const char *c = argv[++i];
			while (*c >= '0' && *c <= '9') caller_pid = caller_pid * 10 + (uint32_t)(*c++ - '0');
			if (*c == ':') for (c++; *c >= '0' && *c <= '9'; c++) caller_serial = caller_serial * 10 + (uint32_t)(*c - '0');
		} else addr = argv[i];
	}
	if (!mem_setup()) {
		run_say(w_title, sizeof(w_title), "print", NULL);
		status("Not enough memory to print (1.5MB)", NULL);
		win_open(WIN_H);
		finish(false, Z_PRINT_E_FAILED);
	}
	z_cfg_get("apps.print.printer", printer, sizeof(printer));
	if (!jobf) {
		if (!addr && !printer[0]) {
			run_say(w_title, sizeof(w_title), "print", NULL);
			status("No printer: set apps.print.printer (docs/print.md)", NULL);
			win_open(WIN_H);
			finish(false, Z_PRINT_E_FAILED);
		}
		probe(addr ? addr : printer);
	}

	// -- the job --
	data = load(jobf, &len);
	fs_unlink((char *)jobf);
	if (!data || !job_open(&j, data, len, start, bytes)) {
		run_say(w_title, sizeof(w_title), "print", NULL);
		status(data ? "The job is damaged" : "The job could not be read", NULL);
		win_open(WIN_H);
		finish(false, Z_PRINT_E_FAILED);
	}
	z_cfg_get("apps.print.paper", j.default_paper, sizeof(j.default_paper));
	run_say(w_title, sizeof(w_title), j.title[0] ? j.title : "Printing", NULL);
	run_say(dest, sizeof(dest), j.dest[0] ? j.dest : printer, NULL);
	win_open(WIN_H);

	memset(&r, 0, sizeof(r));
	r.tick = tick;
	r.wait = wait_for;
	memset(&t, 0, sizeof(t));
	t.connect = t_connect;
	t.close = t_close;
	t.sock = &sock_out;
	t.read = t_read;

	if (!strncmp(dest, "pdf:", 4) || !strncmp(dest, "ps:", 3)) {
		// a file
		bool pdf = dest[1] == 'd';
		const char *path = dest + (pdf ? 4 : 3);
		out_t o = { f_write, NULL, 0, false };
		status("Writing ", path);
		fh = fs_open_write(path);
		if (fh < 0) { status("Cannot write ", path); finish(false, Z_PRINT_E_FAILED); }
		ok = run_vec(&r, &j, &o, pdf ? VEC_PDF : VEC_PS);
		if (ok && fn) ok = fs_write_chunk(fh, fbuf, fn) == fn;
		if (fs_close_handle(fh) < 0) ok = false;
		if (!ok) {
			fs_unlink((char *)path);
			status(r.err[0] ? r.err : "Could not write the file", NULL);
		} else {
			status("Saved ", path);
		}
	} else if (!dest[0]) {
		status("No printer: set apps.print.printer (docs/print.md)", NULL);
	} else if (!ipp_url(dest, &u)) {
		status("Not a printer's address: ", dest);
	} else {
		status("Connecting to ", u.host);
		if (u.kind == URL_SOCKET) {
			linger = true;
			ok = run_socket(&r, &t, &u, &j);
		} else {
			char pref[16];
			if (!zp_job_get(j.data, j.len, "format", pref, sizeof(pref)) || !pref[0])
				run_say(pref, sizeof(pref), fmt_pref(), NULL);
			ok = run_ipp(&r, &t, &u, &j, pref);
		}
		if (ok) {
			char b[64];
			int k = fmt_int(b, job_sheets(&j) * (r.format && !strcmp(r.format, "PWG raster") ? j.copies : 1));
			run_say(b + k, (int)sizeof(b) - k, job_sheets(&j) == 1 ? " sheet" : " sheets", NULL);
			k = (int)strlen(b);
			run_say(b + k, (int)sizeof(b) - k, ", sent as ", r.format ? r.format : "PostScript");
			status("Printed: ", b);
		} else {
			status(r.err[0] ? r.err : "The printer did not take the job", NULL);
		}
	}
	if (ok && r.skipped) {
		char first[48];
		res_missing(first, sizeof(first));
		run_say(w_info[w_ninfo++], 64, "Not found, so left out: ", first[0] ? first : "a picture or font");
		if (win_up) {
			z_win_resize(&win, WIN_W, INFO_H);
			draw_all();
		}
	}
	permille = 1000;
	finish(ok, ok ? 0 : cancelled ? Z_PRINT_E_CANCELLED : Z_PRINT_E_FAILED);
	return 0;
}
