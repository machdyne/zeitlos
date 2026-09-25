/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The mesh window. See mesh_ui.h and docs/mesh_app.md, "User interface".
 *
 *   +-------------------+-----------------------------------------+
 *   | #LongFast       2 | @ALI Alice  2 hops  snr 5.5  87%  3m ago |
 *   | #LD               |-----------------------------------------|
 *   |-------------------| 12:04 ALI: anyone near the river?        |
 *   | ALI  Alice      1 | 12:06 me: yes, east bank  [delivered]    |
 *   | BOB  Bob          |                                         |
 *   |                   |-----------------------------------------|
 *   |                   | > _                                     |
 *   +-------------------+-----------------------------------------+
 *   | HB !849b6ba0  fw 2.7.3  12 nodes                    F1 help |
 *   +-------------------------------------------------------------+
 *
 * 1bpp: ink is colour 1 on 0, the selection is inverse video. Text is
 * drawn with the UTF-8 calls throughout -- names and messages come from
 * other people's phones -- in z_font_5x8, the font wm keeps in glyph
 * memory. Columns follow z_cp_width(), the same rule the renderer uses,
 * so a CJK name takes two cells in the wrap and on screen alike.
 *
 * Nothing here formats with printf: see docs/app_runtime.md.
 */

#include <string.h>

#include "zeitlos.h"
#include "zwin.h"
#include "zwm.h"
#include "zkbd.h"
#include "zfont.h"
#include "zutf8.h"
#include "zrtc.h"
#include "zcfg.h"
#include "zcaption.h"

#include "mesh_ui.h"
#include "mesh_view.h"

#define FONT		(&z_font_5x8)
#define CW			5			// cell width
#define LH			9			// line pitch: 8-pixel cell, one gap

#define WIN_W		480
#define WIN_H		300

#define MODE_CHAT	0
#define MODE_LOG	1
#define MODE_HELP	2

#define D_SIDE		0x01
#define D_HDR		0x02
#define D_MSGS		0x04
#define D_INPUT		0x08
#define D_STATUS	0x10
#define D_FRAME		0x20
#define D_ALL		0x3f

#define LOG_LINES	48
#define LOG_COLS	128

static z_win_t win;
static mesh_model_t *M;
static mesh_session_t *S;
static mesh_view_t V;
static mesh_input_t IN;

static int in_start;			// input: first byte shown
static int link = LINK_NO_SERIAL;
static int mode = MODE_CHAT;
static int scroll;				// message lines up from the bottom
static int sb_top;				// first conversation shown in the sidebar
static unsigned dirty = D_ALL;
static bool rebuild = true;
static bool notify = true;
static uint8_t buttons_was;
static uint32_t title_unread = 0xffffffffu;

static char notice[96];			// transient status text
static uint32_t notice_until;

static char logbuf[LOG_LINES][LOG_COLS];
static int log_head, log_count;

static z_tz_t tz;
static bool tz_ok;

// Layout, from relayout().
static int W, H;
static int sb_w, sb_cols, sb_rows;
static int rx, rcols;
static int y_rule0, y_msg0, msg_rows, y_rule1, y_in, y_rule2, y_status;

// -- small string building, no printf --

static int scat(char *d, int cap, int at, const char *s) {
	while (*s && at < cap - 1) d[at++] = *s++;
	d[at] = 0;
	return at;
}

static int putu(char *d, int cap, int at, uint32_t v) {
	char t[12];
	int n = 0;
	if (!v) t[n++] = '0';
	while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
	while (n && at < cap - 1) d[at++] = t[--n];
	d[at] = 0;
	return at;
}

// Tenths, as "5.5" / "-3.5".
static int putx10(char *d, int cap, int at, int32_t v) {
	if (v < 0) { at = scat(d, cap, at, "-"); v = -v; }
	at = putu(d, cap, at, (uint32_t)v / 10);
	at = scat(d, cap, at, ".");
	return putu(d, cap, at, (uint32_t)v % 10);
}

static int put2(char *d, int cap, int at, int v) {
	char t[3] = { (char)('0' + v / 10 % 10), (char)('0' + v % 10), 0 };
	return scat(d, cap, at, t);
}

static uint32_t now_ms(void) {
	return (uint32_t)((uint64_t)z_uptime_ticks() * 1000u / Z_TICK_HZ);
}

// A plausible epoch time: after 2020. A node without GPS or a phone to
// set its clock reports seconds since it booted.
static bool epoch_ok(uint32_t t) { return t > 1577836800u; }

// The wall clock. tests/render.c supplies its own: the RTC is MMIO.
#ifdef MESH_UI_HOST
uint32_t mesh_ui_host_clock(void);
static uint32_t now_epoch(void) { return mesh_ui_host_clock(); }
#else
static uint32_t now_epoch(void) {
	return z_rtc_valid() ? z_rtc_seconds() : 0;
}
#endif

// "HH:MM " in local time, or nothing.
static int put_time(char *d, int cap, int at, uint32_t t) {
	z_tm_t tm;
	if (!epoch_ok(t)) return at;
	z_time_to_tm(tz_ok ? z_tz_local(&tz, t, NULL) : t, &tm);
	at = put2(d, cap, at, tm.hour);
	at = scat(d, cap, at, ":");
	at = put2(d, cap, at, tm.min);
	return scat(d, cap, at, " ");
}

// "3m ago"
static int put_ago(char *d, int cap, int at, uint32_t t) {
	uint32_t now = now_epoch(), a;
	if (!epoch_ok(t) || !epoch_ok(now) || t > now + 60) return at;
	a = now - t;
	if (a < 60) return scat(d, cap, at, "just now");
	if (a < 3600) { at = putu(d, cap, at, a / 60); return scat(d, cap, at, "m ago"); }
	if (a < 86400) { at = putu(d, cap, at, a / 3600); return scat(d, cap, at, "h ago"); }
	at = putu(d, cap, at, a / 86400);
	return scat(d, cap, at, "d ago");
}

// -- drawing primitives --

// Draw `s`, cut to `cols` columns, at (x, y). inv: inverse video.
static void text(int x, int y, const char *s, int cols, bool inv) {
	char b[256];
	size_t n;
	if (cols <= 0) return;
	n = z_utf8_fit_cols(s, strlen(s), cols);
	if (n > sizeof(b) - 1) n = z_utf8_fit(s, n, sizeof(b) - 1);
	memcpy(b, s, n);
	b[n] = 0;
	if (inv) z_win_draw_utf8_2(&win, x, y, b, 0, 1, FONT);
	else z_win_draw_utf8(&win, x, y, b, 1, FONT);
}

static void relayout(void) {
	W = z_win_content_w(&win);
	H = z_win_content_h(&win);
	sb_cols = W >= 400 ? 22 : 15;
	sb_w = sb_cols * CW + 4;
	rx = sb_w + 3;
	rcols = (W - rx - 2) / CW;
	y_status = H - LH;
	y_rule2 = y_status - 2;
	y_in = y_rule2 - LH - 1;
	y_rule1 = y_in - 3;
	y_rule0 = LH + 2;
	y_msg0 = y_rule0 + 3;
	msg_rows = (y_rule1 - 2 - y_msg0) / LH;
	if (msg_rows < 1) msg_rows = 1;
	sb_rows = (y_rule2 - 2) / LH;
	if (sb_rows < 1) sb_rows = 1;
	if (rcols < 8) rcols = 8;
	dirty = D_ALL;
}

// -- the sidebar --

static void conv_label(const mesh_conv_t *c, char *b, int cap) {
	int n = 0;
	if (c->kind == CONV_CHAN) {
		n = scat(b, cap, n, "#");
		scat(b, cap, n, mesh_chan_name(M, c->chan));
	} else {
		mesh_node_t *nd = mesh_node_find(M, c->peer);
		char id[12];
		if (nd && nd->has_user) {
			int k;
			n = scat(b, cap, n, nd->short_name);
			for (k = z_utf8_cols(nd->short_name, strlen(nd->short_name)); k < 5; k++)
				n = scat(b, cap, n, " ");
			scat(b, cap, n, nd->long_name);
		} else {
			mesh_node_id(c->peer, id);
			scat(b, cap, n, id);
		}
	}
}

static void draw_side(void) {
	int i, cur, row, first_node = -1;
	char b[64], u[8];

	z_win_fill_rect(&win, 0, 0, sb_w, y_rule2, 0);
	cur = mesh_view_cur(&V);
	if (cur >= 0) {
		if (cur < sb_top) sb_top = cur;
		if (cur >= sb_top + sb_rows) sb_top = cur - sb_rows + 1;
	}
	if (sb_top > V.nconv - sb_rows) sb_top = V.nconv - sb_rows;
	if (sb_top < 0) sb_top = 0;

	for (i = 0; i < V.nconv; i++)
		if (V.conv[i].kind == CONV_DM) { first_node = i; break; }

	for (i = sb_top, row = 0; i < V.nconv && row < sb_rows; i++, row++) {
		const mesh_conv_t *c = &V.conv[i];
		int y = 1 + row * LH;
		bool sel = i == cur;
		int ucols = 0;
		// The inverse bar covers the glyph cell and the gap BELOW it, so
		// the pixel row above stays free for the channels/nodes rule.
		if (sel) z_win_fill_rect(&win, 0, y, sb_w, LH, 1);
		if (c->unread) {
			int k = putu(u, sizeof(u), 0, c->unread);
			ucols = k + 1;
			text(sb_w - 2 - k * CW, y, u, k, sel);
		}
		conv_label(c, b, sizeof(b));
		text(2, y, b, sb_cols - ucols, sel);
		// A rule between the channels and the nodes.
		if (i == first_node && row > 0)
			z_win_fill_rect(&win, 2, y - 1, sb_w - 4, 1, 1);
	}
}

// -- the header --

static void draw_hdr(void) {
	char b[160];
	int n = 0;
	z_win_fill_rect(&win, rx, 0, W - rx, y_rule0, 0);
	if (mode == MODE_LOG) {
		text(rx, 1, "node console -- F2 or Esc to go back", rcols, false);
		return;
	}
	if (mode == MODE_HELP) {
		text(rx, 1, "keys -- F1 or Esc to go back", rcols, false);
		return;
	}
	if (V.cur_kind == CONV_CHAN) {
		n = scat(b, sizeof(b), n, "#");
		n = scat(b, sizeof(b), n, mesh_chan_name(M, V.cur_chan));
		if (M->chan[V.cur_chan].role == CH_ROLE_PRIMARY)
			n = scat(b, sizeof(b), n, "  (primary)");
		n = scat(b, sizeof(b), n, "  channel ");
		putu(b, sizeof(b), n, V.cur_chan);
	} else {
		mesh_node_t *nd = mesh_node_find(M, V.cur_peer);
		char id[12];
		mesh_node_id(V.cur_peer, id);
		n = scat(b, sizeof(b), n, "@");
		if (nd && nd->has_user) {
			n = scat(b, sizeof(b), n, nd->short_name);
			n = scat(b, sizeof(b), n, " ");
			n = scat(b, sizeof(b), n, nd->long_name);
			n = scat(b, sizeof(b), n, "  ");
		}
		n = scat(b, sizeof(b), n, id);
		if (nd) {
			if (nd->hops_away == 0) n = scat(b, sizeof(b), n, "  direct");
			else if (nd->hops_away > 0) {
				n = scat(b, sizeof(b), n, "  ");
				n = putu(b, sizeof(b), n, (uint32_t)nd->hops_away);
				n = scat(b, sizeof(b), n, nd->hops_away == 1 ? " hop" : " hops");
			}
			if (nd->snr_x10 != MESH_UNKNOWN_SNR) {
				n = scat(b, sizeof(b), n, "  snr ");
				n = putx10(b, sizeof(b), n, nd->snr_x10);
			}
			if (nd->battery == 101) n = scat(b, sizeof(b), n, "  powered");
			else if (nd->battery <= 100) {
				n = scat(b, sizeof(b), n, "  ");
				n = putu(b, sizeof(b), n, nd->battery);
				n = scat(b, sizeof(b), n, "%");
			}
			if (epoch_ok(nd->last_heard)) {
				n = scat(b, sizeof(b), n, "  ");
				put_ago(b, sizeof(b), n, nd->last_heard);
			}
		}
	}
	text(rx, 1, b, rcols, false);
}

// -- the messages --

// One message as it is shown: time, who, text, and our own status.
static void compose(const mesh_msg_t *g, char *b, int cap) {
	char nm[MESH_LONG_MAX];
	int n = 0;
	n = put_time(b, cap, n, g->rx_time);
	if (g->from == M->my_num && M->my_num) n = scat(b, cap, n, "me");
	else n = scat(b, cap, n, mesh_name(M, g->from, nm, sizeof(nm)));
	n = scat(b, cap, n, ": ");
	n = scat(b, cap, n, g->text);
	if (g->status != MSG_RX) {
		n = scat(b, cap, n, "  [");
		n = scat(b, cap, n, mesh_status_name(g));
		scat(b, cap, n, "]");
	}
}

typedef struct {
	int idx;		// running line index across the conversation
	int first;		// first line index shown
} draw_ctx_t;

static void emit_line(void *ctx, const char *p, int len, int line) {
	draw_ctx_t *d = (draw_ctx_t *)ctx;
	char b[256];
	int row = d->idx - d->first;
	d->idx++;
	if (row < 0 || row >= msg_rows) return;
	if (len > (int)sizeof(b) - 1) len = (int)sizeof(b) - 1;
	memcpy(b, p, (size_t)len);
	b[len] = 0;
	text(rx + (line ? 2 * CW : 0), y_msg0 + row * LH, b, line ? rcols - 2 : rcols, false);
}

static int count_lines(void) {
	char b[MESH_PAYLOAD_MAX + 96];
	uint32_t i;
	int total = 0;
	for (i = 0; i < M->msg_count; i++) {
		mesh_msg_t *g = mesh_msg_at(M, i);
		if (!mesh_view_in_cur(&V, M, g)) continue;
		compose(g, b, sizeof(b));
		total += mesh_wrap(b, rcols, 2, NULL, NULL);
	}
	return total;
}

static void draw_chat(void) {
	char b[MESH_PAYLOAD_MAX + 96];
	draw_ctx_t d;
	uint32_t i;
	int total = count_lines();

	if (scroll > total - msg_rows) scroll = total - msg_rows;
	if (scroll < 0) scroll = 0;
	d.idx = 0;
	d.first = total - msg_rows - scroll;
	if (d.first < 0) d.first = 0;

	if (!total) {
		text(rx, y_msg0, V.cur_kind == CONV_DM ?
			"No messages with this node yet. Type below to send one." :
			"No messages on this channel yet.", rcols, false);
		return;
	}
	for (i = 0; i < M->msg_count; i++) {
		mesh_msg_t *g = mesh_msg_at(M, i);
		if (!mesh_view_in_cur(&V, M, g)) continue;
		compose(g, b, sizeof(b));
		mesh_wrap(b, rcols, 2, emit_line, &d);
		if (d.idx >= d.first + msg_rows) break;
	}
}

static void draw_log(void) {
	int rows = msg_rows, k, first;
	if (!log_count) {
		text(rx, y_msg0, "Nothing from the node's console yet.", rcols, false);
		return;
	}
	first = log_count > rows ? log_count - rows : 0;
	for (k = first; k < log_count; k++) {
		int slot = (log_head - log_count + k + LOG_LINES) % LOG_LINES;
		text(rx, y_msg0 + (k - first) * LH, logbuf[slot], rcols, false);
	}
}

static const char *const help[] = {
	"Enter      send what you typed",
	"Up / Down  previous / next conversation",
	"Tab        next conversation with unread messages",
	"PgUp/PgDn  scroll the messages (or the wheel)",
	"Esc        clear the line",
	"Ctrl+W     delete a word      Ctrl+U  clear the line",
	"F2         the node's console log",
	"F4         notifications for direct messages on/off",
	"Ctrl+Q     quit",
	"",
	"A channel reaches everyone on it; a node is a direct message.",
	"The node does the radio and the encryption: mesh needs",
	"`serial` running and the node on a USB port.",
};

static void draw_help(void) {
	int i;
	for (i = 0; i < (int)(sizeof(help) / sizeof(help[0])) && i < msg_rows; i++)
		text(rx, y_msg0 + i * LH, help[i], rcols, false);
}

static void draw_msgs(void) {
	z_win_fill_rect(&win, rx, y_rule0 + 1, W - rx, y_rule1 - y_rule0 - 1, 0);
	if (mode == MODE_LOG) draw_log();
	else if (mode == MODE_HELP) draw_help();
	else draw_chat();
}

// -- the input line --

static void draw_input(void) {
	int cols = rcols - 2, x0 = rx + 2 * CW, cx;
	char cnt[12];
	const char *cur_ch;
	char under[5];

	z_win_fill_rect(&win, rx, y_rule1 + 1, W - rx, y_rule2 - y_rule1 - 1, 0);
	// The byte count, once it matters.
	if (IN.len > MESH_PAYLOAD_MAX - 40) {
		int n = putu(cnt, sizeof(cnt), 0, (uint32_t)IN.len);
		n = scat(cnt, sizeof(cnt), n, "/");
		n = putu(cnt, sizeof(cnt), n, MESH_PAYLOAD_MAX);
		cols -= n + 1;
		text(rx + (rcols - n) * CW, y_in, cnt, n, false);
	}
	text(rx, y_in, ">", 1, false);
	in_start = mesh_input_scroll(&IN, in_start, cols);
	text(x0, y_in, IN.buf + in_start, cols, false);

	// The cursor: the character under it in inverse, or a block.
	cx = x0 + z_utf8_cols(IN.buf + in_start, (size_t)(IN.cur - in_start)) * CW;
	cur_ch = IN.buf + IN.cur;
	if (IN.cur < IN.len) {
		int k = z_utf8_next_off(IN.buf, IN.len, IN.cur) - IN.cur;
		memcpy(under, cur_ch, (size_t)k);
		under[k] = 0;
		z_win_fill_rect(&win, cx, y_in - 1, CW * z_utf8_cols(under, (size_t)k), LH, 1);
		text(cx, y_in, under, 2, true);
	} else {
		z_win_fill_rect(&win, cx, y_in - 1, CW, LH, 1);
	}
}

// -- the status bar --

static void draw_status(void) {
	char b[160];
	int n = 0;
	z_win_fill_rect(&win, 0, y_rule2 + 1, W, H - y_rule2 - 1, 0);

	if (notice[0] && (int32_t)(notice_until - now_ms()) > 0) {
		scat(b, sizeof(b), 0, notice);
	} else if (link == LINK_NO_SERIAL) {
		scat(b, sizeof(b), 0, "serial is not running -- start it with `run serial`");
	} else if (link == LINK_NO_DEVICE || S->state == MESH_ST_DOWN) {
		scat(b, sizeof(b), 0, "no Meshtastic node on USB -- plug one in (retrying)");
	} else if (S->state == MESH_ST_CONFIG) {
		n = scat(b, sizeof(b), n, "loading the node's configuration... ");
		n = putu(b, sizeof(b), n, (uint32_t)mesh_node_count(M));
		scat(b, sizeof(b), n, " nodes");
	} else {
		char nm[MESH_LONG_MAX], id[12];
		mesh_name(M, M->my_num, nm, sizeof(nm));
		mesh_node_id(M->my_num, id);
		n = scat(b, sizeof(b), n, nm);
		if (strcmp(nm, id)) {
			n = scat(b, sizeof(b), n, " ");
			n = scat(b, sizeof(b), n, id);
		}
		if (M->firmware[0]) {
			n = scat(b, sizeof(b), n, "  fw ");
			n = scat(b, sizeof(b), n, M->firmware);
		}
		n = scat(b, sizeof(b), n, "  ");
		n = putu(b, sizeof(b), n, (uint32_t)mesh_node_count(M));
		n = scat(b, sizeof(b), n, " nodes");
		if (M->region == 0)
			n = scat(b, sizeof(b), n, "  REGION NOT SET: it will not transmit");
		if (M->rejected) {
			n = scat(b, sizeof(b), n, "  rejected ");
			n = putu(b, sizeof(b), n, M->rejected);
		}
		if (!notify) scat(b, sizeof(b), n, "  (notify off)");
	}
	text(2, y_status, b, W / CW - 9, false);
	text(W - 7 * CW - 2, y_status, "F1 help", 7, false);
}

static void draw_frame(void) {
	z_win_fill_rect(&win, sb_w, 0, 1, y_rule2, 1);
	z_win_fill_rect(&win, rx, y_rule0, W - rx, 1, 1);
	z_win_fill_rect(&win, rx, y_rule1, W - rx, 1, 1);
	z_win_fill_rect(&win, 0, y_rule2, W, 1, 1);
}

static void update_title(void) {
	uint32_t u = mesh_view_unread_total(&V);
	char t[24];
	int n;
	if (u == title_unread) return;
	title_unread = u;
	n = scat(t, sizeof(t), 0, "mesh");
	if (u) {
		n = scat(t, sizeof(t), n, " (");
		n = putu(t, sizeof(t), n, u);
		scat(t, sizeof(t), n, ")");
	}
	z_win_set_title(&win, t);
}

void mesh_ui_flush(void) {
	unsigned d = dirty;
	if (notice[0] && (int32_t)(notice_until - now_ms()) <= 0) {
		notice[0] = 0;
		d |= D_STATUS;
	}
	if (!d && !rebuild) return;
	dirty = 0;
	if (rebuild) {
		mesh_view_build(&V, M);
		rebuild = false;
		d |= D_SIDE;
	}
	if (d & D_FRAME) { z_win_clear(&win); draw_frame(); d = D_ALL; }
	if (d & D_SIDE) draw_side();
	if (d & D_HDR) draw_hdr();
	if (d & D_MSGS) draw_msgs();
	if (d & D_INPUT) draw_input();
	if (d & D_STATUS) draw_status();
	update_title();
}

static void say(const char *s) {
	scat(notice, sizeof(notice), 0, s);
	notice_until = now_ms() + 4000;
	dirty |= D_STATUS;
}

// -- actions --

static void show(int i) {
	mesh_view_select(&V, i);
	scroll = 0;
	mode = MODE_CHAT;
	dirty |= D_SIDE | D_HDR | D_MSGS;
}

static void send_line(void) {
	mesh_msg_t *g;
	uint32_t to = MESH_BROADCAST;
	uint8_t ch = 0;

	if (!IN.len) return;
	if (S->state != MESH_ST_LIVE) {
		say("not connected to a node -- the message is kept, try again");
		return;
	}
	if (V.cur_kind == CONV_DM) to = V.cur_peer;
	else ch = V.cur_chan;
	g = mesh_session_send_text(S, to, ch, IN.buf, (uint32_t)IN.len);
	if (!g) {
		say("the node would not take it -- try again");
		return;
	}
	if (!epoch_ok(g->rx_time)) g->rx_time = now_epoch();
	mesh_view_note(&V, M, g);
	mesh_input_clear(&IN);
	in_start = 0;
	scroll = 0;
	mode = MODE_CHAT;
	rebuild = true;
	dirty |= D_MSGS | D_INPUT | D_HDR;
}

static void next_unread(void) {
	int i, cur = mesh_view_cur(&V);
	for (i = 1; i <= V.nconv; i++) {
		int k = (cur + i) % V.nconv;
		if (V.conv[k].unread) { show(k); return; }
	}
	if (V.nconv) show((cur + 1) % V.nconv);
}

static bool on_key(uint32_t packed) {
	uint32_t k = Z_WM_UNPACK_KEY_KEYSYM(packed);
	uint8_t mods = (uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(packed);
	int cur;

	if (!Z_WM_UNPACK_KEY_PRESSED(packed)) return true;

	if (mods & Z_KBD_MOD_CTRL) {
		uint32_t c = k | 0x20;		// letters, either case
		if (k == 0x11 || c == 'q') return false;
		if (k == 0x15 || c == 'u') { mesh_input_clear(&IN); in_start = 0; }
		else if (k == 0x17 || c == 'w') mesh_input_word(&IN);
		dirty |= D_INPUT;
		return true;
	}

	switch (k) {
	case 0x0d: case 0x0a:
		send_line();
		return true;
	case 0x1b:
		if (mode != MODE_CHAT) { mode = MODE_CHAT; dirty |= D_HDR | D_MSGS; }
		else { mesh_input_clear(&IN); in_start = 0; dirty |= D_INPUT; }
		return true;
	case 0x7f: case 0x08:
		mesh_input_backspace(&IN); dirty |= D_INPUT; return true;
	case 0x09:
		next_unread(); return true;
	case Z_KEY_DELETE: mesh_input_delete(&IN); dirty |= D_INPUT; return true;
	case Z_KEY_LEFT:   mesh_input_left(&IN);   dirty |= D_INPUT; return true;
	case Z_KEY_RIGHT:  mesh_input_right(&IN);  dirty |= D_INPUT; return true;
	case Z_KEY_HOME:   mesh_input_home(&IN);   dirty |= D_INPUT; return true;
	case Z_KEY_END:    mesh_input_end(&IN);    dirty |= D_INPUT; return true;
	case Z_KEY_UP:
		cur = mesh_view_cur(&V);
		if (cur > 0) show(cur - 1);
		return true;
	case Z_KEY_DOWN:
		cur = mesh_view_cur(&V);
		if (cur + 1 < V.nconv) show(cur + 1);
		return true;
	case Z_KEY_PAGEUP:
		scroll += msg_rows - 1; dirty |= D_MSGS; return true;
	case Z_KEY_PAGEDOWN:
		scroll -= msg_rows - 1; if (scroll < 0) scroll = 0;
		dirty |= D_MSGS; return true;
	case Z_KEY_F1:
		mode = mode == MODE_HELP ? MODE_CHAT : MODE_HELP;
		dirty |= D_HDR | D_MSGS; return true;
	case Z_KEY_F2:
		mode = mode == MODE_LOG ? MODE_CHAT : MODE_LOG;
		dirty |= D_HDR | D_MSGS; return true;
	case Z_KEY_F4:
		notify = !notify;
		say(notify ? "notifications on" : "notifications off");
		return true;
	default:
		break;
	}
	if (k < Z_KEY_NAMED_BASE && mesh_input_insert(&IN, k)) dirty |= D_INPUT;
	return true;
}

static void on_mouse(uint32_t packed) {
	int cx, cy;
	uint8_t b = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool press = (b & Z_MOUSE_BTN_LEFT) && !(buttons_was & Z_MOUSE_BTN_LEFT);
	buttons_was = b;
	if (!press || !z_win_mouse_content_xy(&win, packed, &cx, &cy)) return;
	if (cx < sb_w && cy < y_rule2) {
		int i = sb_top + cy / LH;
		if (i >= 0 && i < V.nconv && cy / LH < sb_rows) show(i);
	}
}

// -- entry points --

bool mesh_ui_open(mesh_model_t *m, mesh_session_t *s) {
	char v[64];
	M = m;
	S = s;
	mesh_view_init(&V);
	mesh_input_clear(&IN);
	if (z_win_create_flags(&win, "mesh", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE) != Z_OK)
		return false;
	v[0] = 0;
	z_cfg_get("system.rtc.timezone", v, sizeof(v));
	tz_ok = v[0] && z_tz_parse(v, &tz);
	relayout();
	mesh_ui_flush();
	return true;
}

void mesh_ui_close(void) {
	z_win_destroy(&win);
}

bool mesh_ui_wm(z_msg_t *msg) {
	switch (msg->subject) {
	case Z_WM_SET_CLIP: {
		// A window is born with no region and draws nothing until its
		// first one arrives. When it does, paint -- rather than trusting
		// that a REDRAW will follow and has not been lost.
		bool had = win.clip_n > 0;
		if (z_win_apply_clip(&win, &msg->obj) && !had && win.clip_n > 0)
			dirty |= D_ALL;
		break;
	}
	case Z_WM_REDRAW:
		if (msg->obj.type != Z_UINT32) break;
		if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;
		z_win_apply_redraw(&win, msg->obj.val.uint32);
		dirty |= D_ALL;
		mesh_ui_flush();
		z_win_redraw_done(&win);
		break;
	case Z_WM_WINDOW_MOVED:
		z_win_parse_rect(&win, &msg->obj);
		break;
	case Z_WM_WINDOW_RESIZED:
		if (z_win_apply_resized(&win, &msg->obj)) relayout();
		break;
	case Z_WM_KEY:
		return on_key(msg->obj.val.uint32);
	case Z_WM_MOUSE:
		on_mouse(msg->obj.val.uint32);
		break;
	case Z_WM_WHEEL:
		scroll += 3 * Z_WM_WHEEL_NOTCHES(msg->obj.val.uint32);
		if (scroll < 0) scroll = 0;
		dirty |= D_MSGS;
		break;
	case Z_WM_CLOSE:
		return false;
	default:
		break;
	}
	return true;
}

void mesh_ui_event(const mesh_ev_t *ev) {
	switch (ev->kind) {
	case MESH_EV_TEXT: {
		mesh_msg_t *g = ev->msg;
		// A node with no clock (no GPS, no phone) stamps seconds since
		// boot; ours is better than that.
		if (!epoch_ok(g->rx_time)) g->rx_time = now_epoch();
		mesh_view_note(&V, M, g);
		rebuild = true;
		if (mesh_view_in_cur(&V, M, g)) {
			// Scrolled back: keep what is on screen where it is.
			if (scroll) {
				char b[MESH_PAYLOAD_MAX + 96];
				compose(g, b, sizeof(b));
				scroll += mesh_wrap(b, rcols, 2, NULL, NULL);
			}
			dirty |= D_MSGS;
		} else if (g->dm && notify && S->state == MESH_ST_LIVE) {
			char b[160], nm[MESH_LONG_MAX];
			int n = scat(b, sizeof(b), 0, mesh_name(M, g->from, nm, sizeof(nm)));
			n = scat(b, sizeof(b), n, ": ");
			scat(b, sizeof(b), n, g->text);
			z_caption_show(b, Z_CAPTION_COMPACT | Z_CAPTION_TOP |
				Z_CAPTION_TIMEOUT(4000));
		}
		dirty |= D_SIDE | D_STATUS;
		break;
	}
	case MESH_EV_STATUS:
		if (mesh_view_in_cur(&V, M, ev->msg)) dirty |= D_MSGS;
		break;
	case MESH_EV_NODE:
		// During the config dump a hundred of these arrive in a row;
		// the list is rebuilt once at the end instead.
		if (S->state == MESH_ST_LIVE) {
			rebuild = true;
			dirty |= D_HDR;
		}
		dirty |= D_STATUS;
		break;
	case MESH_EV_CONFIG_DONE:
	case MESH_EV_REBOOTED:
	case MESH_EV_CHANNEL:
	case MESH_EV_MY_INFO:
		rebuild = true;
		dirty |= D_SIDE | D_HDR | D_MSGS | D_STATUS;
		if (ev->kind == MESH_EV_REBOOTED) say("the node rebooted -- reloading");
		break;
	case MESH_EV_NOTIFY:
		say(ev->text);
		break;
	case MESH_EV_REJECT:
		dirty |= D_STATUS;
		break;
	default:
		break;
	}
}

void mesh_ui_line(const char *s) {
	char *d = logbuf[log_head];
	size_t n = z_utf8_fit(s, strlen(s), LOG_COLS - 1);
	memcpy(d, s, n);
	d[n] = 0;
	log_head = (log_head + 1) % LOG_LINES;
	if (log_count < LOG_LINES) log_count++;
	if (mode == MODE_LOG) dirty |= D_MSGS;
}

void mesh_ui_link(int l) {
	if (l != link) {
		link = l;
		dirty |= D_STATUS;
	}
}
