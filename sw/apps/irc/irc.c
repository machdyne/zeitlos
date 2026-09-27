/*
 * irc -- an IRC client
 *
 *   > run irc
 *
 * One window: the conversation, a status line, and a line to type in.
 * A view per channel and per private conversation, plus the server
 * view; Ctrl+N and Ctrl+P move between them, and the status line
 * marks the ones with something new. docs/irc_app.md.
 *
 * The protocol is irc_core.c, which has no I/O and is tested on the
 * build machine. This file is the window, the scrollback and the
 * socket: a raw TCP connection through `net`, the same kind the web
 * browser uses (docs/networking.md, "Raw TCP sockets"). net keeps
 * NET_SOCK_SLOTS of those, so browsing works while this is connected.
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
#include "../../common/zkbd.h"
#include "../../common/zedit.h"
#include "../../common/zport.h"
#include "../../common/zobj.h"
#include "../../common/zdns.h"
#include "../../common/zcfg.h"
#include "../../common/zrtc.h"

#include "irc_core.h"

#define WIN_W	480
#define WIN_H	300
#define MARGIN	2
#define ROW_H	(z_font_5x8.h + 1)
#define EDIT_H	(z_font_5x8.h + 4)

static z_win_t win;

// -- views --
//
// A view is a channel, a private conversation, or the server itself
// (the empty name, always view 0). Lines are stored with the name of
// the view they belong to, and drawing shows only the current view's.

#define VIEWS_MAX 12

typedef struct {
	char	name[IRC_TARGET_MAX];
	bool	unread;
	bool	mention;
} view_t;

static view_t views[VIEWS_MAX];
static int nviews = 1;		// views[0] is the server
static int cur;				// the view on screen

static int view_find(const char *name) {
	for (int i = 0; i < nviews; i++)
		if (!irc_casecmp(views[i].name, name)) return i;
	return -1;
}

static int view_add(const char *name) {
	int i = view_find(name);
	if (i >= 0) return i;
	if (nviews == VIEWS_MAX) return 0;		// full: it goes to the server view
	memset(&views[nviews], 0, sizeof(views[0]));
	snprintf(views[nviews].name, sizeof(views[0].name), "%s", name);
	return nviews++;
}

static void view_remove(int i) {
	if (i <= 0 || i >= nviews) return;
	memmove(&views[i], &views[i + 1], (size_t)(nviews - i - 1) * sizeof(views[0]));
	nviews--;
	if (cur == i) cur = 0;
	else if (cur > i) cur--;
}

// -- scrollback --
//
// A ring of text: each line is "view\0text\0", written one after the
// other and wrapping to the start. Writing a line over older ones
// evicts them, oldest first -- which, because the ring is written in
// order, are always the ones just ahead of the write position.
// Wrapping to the window's width happens when drawing, so a resize
// reflows everything.

#define SB_BYTES	32768
#define SB_LINES	1024
#define SB_MENTION	1

static char sb[SB_BYTES];
static uint16_t sb_off[SB_LINES];
static uint8_t sb_flags[SB_LINES];
static int sb_first, sb_count;
static uint32_t sb_write;
static int scroll_rows;			// rows scrolled back from the bottom

static uint32_t sb_len_at(uint32_t off) {
	uint32_t a = (uint32_t)strlen(sb + off) + 1;
	return a + (uint32_t)strlen(sb + off + a) + 1;
}

static bool dirty = true;

static void repaint(void);

// The time now, "14:05 ", in the configured zone (docs/config.md,
// system.rtc.timezone), or "" when the clock is not set.
static void stamp(char out[12]) {

	static z_tz_t tz;
	static bool tz_read;
	z_tm_t tm;
	uint32_t t;

	out[0] = 0;
	if (!z_rtc_available() || !z_rtc_valid()) return;
	if (!tz_read) {
		char s[64];
		tz_read = true;
		if (!z_cfg_get("system.rtc.timezone", s, sizeof(s)) || !z_tz_parse(s, &tz))
			z_tz_parse("UTC", &tz);
	}
	t = z_tz_local(&tz, z_rtc_seconds(), NULL);
	z_time_to_tm(t, &tm);
	snprintf(out, 12, "%02u:%02u ", (unsigned)tm.hour % 24, (unsigned)tm.min % 60);

}

static void add_line(const char *view, const char *text, bool mention) {

	char st[12];
	uint32_t vl, tl, len;
	int vi;

	stamp(st);
	vl = (uint32_t)strlen(view) + 1;
	tl = (uint32_t)strlen(st) + (uint32_t)strlen(text) + 1;
	if (tl > IRC_SHOW_MAX) tl = IRC_SHOW_MAX;
	len = vl + tl;

	if (sb_write + len > SB_BYTES) sb_write = 0;

	// Evict whatever this line is about to overwrite, oldest first.
	while (sb_count) {
		uint32_t o = sb_off[sb_first], ol = sb_len_at(o);
		bool overlap = o < sb_write + len && sb_write < o + ol;
		if (!overlap && sb_count < SB_LINES) break;
		sb_first = (sb_first + 1) % SB_LINES;
		sb_count--;
	}

	memcpy(sb + sb_write, view, vl);
	snprintf(sb + sb_write + vl, tl, "%s%s", st, text);
	{
		int i = (sb_first + sb_count) % SB_LINES;
		sb_off[i] = (uint16_t)sb_write;
		sb_flags[i] = mention ? SB_MENTION : 0;
		sb_count++;
	}
	sb_write += len;

	vi = view_find(view);
	if (vi < 0) vi = 0;
	if (vi != cur) {
		views[vi].unread = true;
		if (mention) views[vi].mention = true;
	} else if (scroll_rows) {
		// Reading back: stay on the same text rather than being
		// dragged to the bottom by every new line.
		scroll_rows++;
	}
	dirty = true;

}

static void say(const char *text) { add_line(views[cur].name, text, false); }

// -- the connection --

typedef enum { C_IDLE, C_CONNECTING, C_REGISTERING, C_ONLINE } conn_state_t;

static conn_state_t cstate = C_IDLE;
static uint32_t net_pid;
static z_port_t sock;
static char server[IRC_TARGET_MAX + 192];
static uint16_t server_port = 6667;
static char me[IRC_NICK_MAX] = "zeitlos";
static char channels[256];

// Outgoing lines wait here while net has as many of our sends
// outstanding as a port allows (Z_PORT_MAX_PENDING_SENDS, zport.h),
// and move on each acknowledgement.
static char txq[4096];
static uint32_t txlen;

static void tx_flush(void) {
	if (!txlen || cstate == C_IDLE || !sock.connected) return;
	if (z_port_send(&sock, txq, txlen) == Z_OK) txlen = 0;
}

static void send_wire(const char *line) {
	uint32_t n = (uint32_t)strlen(line);
	if (cstate == C_IDLE || cstate == C_CONNECTING) return;
	if (txlen + n > sizeof(txq)) { say("-- too much queued to send; dropped a line"); return; }
	memcpy(txq + txlen, line, n);
	txlen += n;
	tx_flush();
}

static void disconnected(const char *why) {
	char t[IRC_SHOW_MAX];
	if (cstate == C_IDLE) return;
	cstate = C_IDLE;
	txlen = 0;
	sock.connected = false;
	snprintf(t, sizeof(t), "-- disconnected%s%s", why ? ": " : "", why ? why : "");
	add_line("", t, false);
	dirty = true;
}

static void connect_to(const char *host, uint16_t port) {

	char err[96], t[IRC_SHOW_MAX];
	uint32_t ip;
	z_obj_t arg;

	if (cstate != C_IDLE) {
		send_wire("QUIT :reconnecting\r\n");
		z_port_close(&sock);
		disconnected("reconnecting");
	}

	if (host && host[0]) snprintf(server, sizeof(server), "%s", host);
	if (port) server_port = port;
	if (!server[0]) {
		say("-- no server: /connect host [port], or set apps.irc.server");
		return;
	}

	if (!net_pid && !z_pid_lookup("net0", &net_pid)) {
		say("-- net is not running -- try `run net`");
		return;
	}

	snprintf(t, sizeof(t), "-- looking up %s...", server);
	add_line("", t, false);
	cur = 0;
	dirty = true;

	// Blocks (bounded; zdns.h). Drawn first so the window says why.
	repaint();
	if (!z_resolve_host(server, &ip, err, sizeof(err))) {
		snprintf(t, sizeof(t), "-- could not resolve %s: %s", server, err);
		add_line("", t, false);
		return;
	}

	snprintf(t, sizeof(t), "-- connecting to %s port %u...", server, (unsigned)server_port);
	add_line("", t, false);

	arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(ip));
	z_map_set(&arg, "port", z_obj_uint32(server_port));
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);
	cstate = C_CONNECTING;

}

static void on_connected(const z_msg_t *msg) {
	char line[IRC_LINE_MAX];
	sock.peer_pid = msg->from;
	sock.conn_id = (msg->obj.type == Z_UINT32) ? msg->obj.val.uint32 : 0;
	sock.connected = true;
	cstate = C_REGISTERING;
	add_line("", "-- connected; registering...", false);
	snprintf(line, sizeof(line), "NICK %s\r\nUSER %s 0 * :Zeitlos user\r\n", me, me);
	send_wire(line);
}

// -- lines from the server --

static char rxline[IRC_LINE_MAX + 2];
static uint32_t rxlen;
static bool rx_overlong;

static void server_line(char *line) {

	static irc_event_t ev;
	irc_msg_t m;

	if (!irc_parse(line, &m)) return;
	irc_interpret(&m, me, &ev);

	if (ev.new_nick[0]) snprintf(me, sizeof(me), "%s", ev.new_nick);

	switch (ev.act) {

	case IRC_ACT_PONG:
		send_wire(ev.wire);
		return;

	case IRC_ACT_NICK_TAKEN:
		add_line("", ev.text, false);
		if (cstate == C_REGISTERING) {
			char l[IRC_LINE_MAX];
			size_t n = strlen(me);
			if (n + 1 < sizeof(me)) { me[n] = '_'; me[n + 1] = 0; }
			snprintf(l, sizeof(l), "NICK %s\r\n", me);
			send_wire(l);
		}
		return;

	case IRC_ACT_WELCOME:
		cstate = C_ONLINE;
		add_line("", ev.text, false);
		if (channels[0]) {
			char l[IRC_LINE_MAX];
			snprintf(l, sizeof(l), "JOIN %s\r\n", channels);
			send_wire(l);
		} else {
			add_line("", "-- connected. /join #channel to start talking; /help for commands", false);
		}
		return;

	case IRC_ACT_REPLY:
		send_wire(ev.wire);
		add_line("", ev.text, false);
		return;

	case IRC_ACT_SHOW: {
		const char *v = ev.target;
		if (v[0]) {
			if (ev.parted) {
				int i = view_find(v);
				add_line("", ev.text, false);
				if (i > 0) view_remove(i);
				dirty = true;
				return;
			}
			view_add(v);
		}
		add_line(v, ev.text, ev.mention);
		if (ev.joined) { cur = view_find(v); scroll_rows = 0; }
		return;
	}

	default:
		return;

	}

}

static void on_data(const uint8_t *d, uint32_t n) {

	for (uint32_t i = 0; i < n; i++) {
		char c = (char)d[i];
		if (c == '\n') {
			if (rxlen && rxline[rxlen - 1] == '\r') rxlen--;
			rxline[rxlen] = 0;
			if (!rx_overlong) server_line(rxline);
			rxlen = 0;
			rx_overlong = false;
		} else if (rxlen < IRC_LINE_MAX) {
			rxline[rxlen++] = c;
		} else {
			rx_overlong = true;		// longer than the protocol allows: dropped
		}
	}

}

// -- typing --

static char input_buf[IRC_LINE_MAX - 64];
static z_edit_t input;

static void show_help(void) {
	static const char *const lines[] = {
		"-- commands:",
		"--   /connect [host [port]]   /quit [reason]",
		"--   /join #channel   /part [#channel] [reason]   /topic [text]",
		"--   /msg nick text   /query nick   /me action   /notice nick text",
		"--   /nick newnick   /names [#channel]   /whois nick   /raw LINE",
		"-- keys: Ctrl+N / Ctrl+P next and previous view; PageUp / PageDown scroll",
		"-- anything not starting with / goes to the current channel or person",
	};
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) say(lines[i]);
}

static void submit(void) {

	static irc_input_t in;

	irc_input(input_buf, views[cur].name, me, &in);
	z_edit_set(&input, "");
	scroll_rows = 0;
	dirty = true;

	switch (in.kind) {

	case IRC_IN_SEND:
		if (cstate != C_ONLINE && cstate != C_REGISTERING) {
			say("-- not connected: /connect first");
			return;
		}
		send_wire(in.wire);
		if (in.arg1[0]) {
			// /msg: the conversation gets a view, and what was said
			// is shown in it.
			int v = view_add(in.arg1);
			char t[IRC_SHOW_MAX];
			const char *msg = strstr(in.text, "> ");
			snprintf(t, sizeof(t), "<%s> %.500s", me, msg ? msg + 2 : in.text);
			add_line(views[v].name, t, false);
		} else if (in.text[0]) {
			say(in.text);
		}
		return;

	case IRC_IN_TARGET: {
		int v = view_add(in.arg1);
		cur = v;
		views[v].unread = views[v].mention = false;
		return;
	}

	case IRC_IN_CONNECT:
		connect_to(in.arg1[0] ? in.arg1 : NULL, in.arg1[0] ? in.port : 0);
		return;

	case IRC_IN_QUIT:
		if (cstate == C_IDLE) { say("-- not connected"); return; }
		send_wire(in.wire);
		cstate = C_ONLINE;		// let the QUIT go out; the server closes
		return;

	case IRC_IN_HELP:
		show_help();
		return;

	case IRC_IN_ERROR:
		say(in.text);
		return;

	default:
		return;

	}

}

static void switch_view(int dir) {
	cur = (cur + dir + nviews) % nviews;
	views[cur].unread = views[cur].mention = false;
	scroll_rows = 0;
	dirty = true;
}

// -- drawing --

// Where the next wrapped row of `s` ends: at most `cols` characters
// (UTF-8 code points), broken at a space when there is one in the
// second half of the row. Returns the byte length of the row.
static uint32_t wrap_row(const char *s, int cols) {

	uint32_t i = 0, brk = 0;
	int n = 0;

	while (s[i] && n < cols) {
		if (s[i] == ' ' && n >= cols / 2) brk = i + 1;
		i++;
		while (((unsigned char)s[i] & 0xC0) == 0x80) i++;	// rest of the code point
		n++;
	}
	if (!s[i]) return i;
	return brk ? brk : i;

}

static int cols_now(void) {
	int w = z_win_content_w(&win) - 2 * MARGIN;
	return w > 0 ? w / z_font_5x8.w : 1;
}

static void repaint(void) {

	z_clip_t c, tc;
	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	int edit_y = ch - MARGIN - EDIT_H;
	int status_y = edit_y - ROW_H - 1;
	int rows = (status_y - MARGIN) / ROW_H;
	int cols = cols_now();
	int skip, drawn = 0;

	z_win_content_rect(&win, &c);

	// Text area, above the status line; clear of the resize grip.
	z_win_fill_rect(&win, 0, 0, cw, status_y, 0);
	tc = c;
	tc.x0 = c.x0 + MARGIN;
	tc.x1 = c.x0 + cw - MARGIN - 1;
	tc.y1 = c.y0 + status_y - 1;

	// Bottom up: the newest line of this view at the bottom, wrapped,
	// less however far the reader has scrolled back.
	if (scroll_rows < 0) scroll_rows = 0;
	skip = scroll_rows;

	for (int k = sb_count - 1; k >= 0 && drawn < rows; k--) {

		int li = (sb_first + k) % SB_LINES;
		const char *v = sb + sb_off[li];
		const char *text = v + strlen(v) + 1;
		uint32_t starts[64], lens[64];
		int nr = 0;

		if (irc_casecmp(v, views[cur].name)) continue;

		for (uint32_t at = 0; nr < 64; ) {
			uint32_t l = wrap_row(text + at, cols);
			starts[nr] = at; lens[nr] = l; nr++;
			at += l;
			if (!text[at] || l == 0) break;
		}

		for (int r = nr - 1; r >= 0 && drawn < rows; r--) {
			char row[IRC_SHOW_MAX + 1];
			bool inv = (sb_flags[li] & SB_MENTION) != 0;
			int y;
			if (skip) { skip--; continue; }
			y = c.y0 + MARGIN + (rows - 1 - drawn) * ROW_H;
			memcpy(row, text + starts[r], lens[r]);
			row[lens[r]] = 0;
			if (inv) z_fb_hw_fill_rect(tc.x0, y, tc.x1 - tc.x0 + 1, ROW_H, 1);
			z_fb_draw_utf8_2(tc.x0, y, row, inv ? 0 : 1, inv ? 1 : 0, &z_font_5x8, &tc);
			drawn++;
		}

	}

	// If there was not that much to scroll back through, stop at the top.
	if (skip && scroll_rows) scroll_rows -= skip;

	// The status line, reversed: where this is, who we are, and which
	// other views have something new -- '*' for new lines, '!' when
	// someone said our nick.
	{
		char st[256];
		int n;
		const char *state = cstate == C_ONLINE ? "" : cstate == C_REGISTERING
			? " (registering)" : cstate == C_CONNECTING ? " (connecting)" : " (offline)";

		n = snprintf(st, sizeof(st), " %s  %s%s%s%s",
			views[cur].name[0] ? views[cur].name : "[server]",
			me, server[0] ? "@" : "", server, state);
		for (int i = 0; i < nviews && n < (int)sizeof(st) - 40; i++) {
			if (i == cur || !views[i].unread) continue;
			n += snprintf(st + n, sizeof(st) - (size_t)n, "  %s%c",
				views[i].name[0] ? views[i].name : "[server]",
				views[i].mention ? '!' : '*');
		}
		if (scroll_rows && n < (int)sizeof(st) - 16)
			snprintf(st + n, sizeof(st) - (size_t)n, "  [scrolled]");

		z_win_fill_rect(&win, 0, status_y, cw, ROW_H + 1, 1);
		tc = c;
		tc.y0 = c.y0 + status_y;
		tc.y1 = tc.y0 + ROW_H;
		tc.x1 = c.x0 + cw - Z_WIN_GRIP_INSET - 1;
		z_fb_draw_utf8_2(c.x0 + MARGIN, c.y0 + status_y + 1, st, 0, 1, &z_font_5x8, &tc);
	}

	// The input line, left of the resize grip.
	z_win_fill_rect(&win, 0, edit_y - 1, cw - Z_WIN_GRIP_INSET, EDIT_H + MARGIN + 1, 0);
	z_edit_draw(&win, &input, MARGIN, edit_y, cw - 2 * MARGIN - Z_WIN_GRIP_INSET,
		EDIT_H, &z_font_5x8);

	dirty = false;

}

// -- input events --

static void handle_key(uint32_t k, uint8_t mods) {

	if (mods & Z_KBD_MOD_CTRL) {
		if (k == 'n' || k == 'N') { switch_view(1); return; }
		if (k == 'p' || k == 'P') { switch_view(-1); return; }
	}

	if (k == Z_KEY_PAGEUP)   { scroll_rows += (z_win_content_h(&win) / ROW_H) / 2; dirty = true; return; }
	if (k == Z_KEY_PAGEDOWN) { scroll_rows -= (z_win_content_h(&win) / ROW_H) / 2; dirty = true; return; }

	if (k == 0x0d) { submit(); return; }

	if (z_edit_key(&input, k)) {
		int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
		z_edit_draw(&win, &input, MARGIN, ch - MARGIN - EDIT_H,
			cw - 2 * MARGIN - Z_WIN_GRIP_INSET, EDIT_H, &z_font_5x8);
	}

}

// -- main --

static void handle_msg(z_msg_t *msg) {

	switch (msg->subject) {

	case Z_WM_WHEEL:
		// Three rows a notch, up = back, like term. scroll_rows counts
		// back from the bottom; repaint() clamps it at the top.
		if (msg->obj.type == Z_UINT32) {
			scroll_rows += 3 * Z_WM_WHEEL_NOTCHES(msg->obj.val.uint32);
			if (scroll_rows < 0) scroll_rows = 0;
			dirty = true;
		}
		break;

	case Z_WM_KEY:
		if (msg->obj.type == Z_UINT32 && Z_WM_UNPACK_KEY_PRESSED(msg->obj.val.uint32))
			handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg->obj.val.uint32),
				(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg->obj.val.uint32));
		break;

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
		dirty = true;
		break;

	case Z_WM_CLOSE:
		if (msg->obj.type == Z_UINT32 && (int32_t)msg->obj.val.uint32 == win.id) {
			if (cstate != C_IDLE) {
				send_wire("QUIT :Zeitlos\r\n");
				z_port_close(&sock);
			}
			z_win_destroy(&win);
			exit(0);
		}
		break;

	case Z_PORT_CONNECTED:
		if (cstate == C_CONNECTING) on_connected(msg);
		break;

	case Z_PORT_REFUSED:
		if (cstate == C_CONNECTING) {
			char t[IRC_SHOW_MAX];
			snprintf(t, sizeof(t), "-- could not connect: %s",
				(msg->obj.type == Z_STR && msg->obj.val.str) ? msg->obj.val.str : "refused");
			cstate = C_IDLE;
			add_line("", t, false);
		}
		break;

	case Z_PORT_DATA: {
		// Copy, ack, then process -- the ack frees net's slot for the
		// next chunk (web.c does the same, and says why).
		static uint8_t buf[4096];
		uint32_t n = z_blob_len(&msg->obj);
		void *d = z_blob_data(&msg->obj);
		bool ours = sock.connected && msg->tag == sock.conn_id;
		if (n > sizeof(buf)) n = sizeof(buf);
		if (d && n) memcpy(buf, d, n);
		z_port_send_ack(msg);
		if (ours && d && n) on_data(buf, n);
		break;
	}

	case Z_PORT_DATA_ACK:
		z_port_handle_ack(&sock, msg);
		tx_flush();
		break;

	case Z_PORT_CLOSE:
		if (sock.connected && msg->tag == sock.conn_id)
			disconnected("the server closed the connection");
		break;

	default:
		break;

	}

}

int main(void) {

	char cfg[IRC_TARGET_MAX + 192];

	printf("irc: starting\n");

	if (z_win_create_flags(&win, "irc", WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE) != Z_OK) {
		printf("irc: failed to create window -- is wm running?\n");
		return 1;
	}

	z_edit_init(&input, input_buf, sizeof(input_buf), "");
	input.focus = true;

	// docs/config.md: apps.irc.server "host [port]", apps.irc.nick,
	// apps.irc.channels "#a #b".
	if (z_cfg_get("apps.irc.nick", cfg, sizeof(cfg)) && cfg[0])
		snprintf(me, sizeof(me), "%.*s", (int)sizeof(me) - 1, cfg);	// a server truncates a long nick too
	if (z_cfg_get("apps.irc.channels", cfg, sizeof(cfg))) {
		// "#a #b" or "#a,#b" -> "#a,#b", which one JOIN takes
		int n = 0;
		for (const char *p = cfg; *p && n < (int)sizeof(channels) - 1; p++)
			channels[n++] = (*p == ' ') ? ',' : *p;
		while (n && channels[n - 1] == ',') n--;
		channels[n] = 0;
	}

	add_line("", "-- Zeitlos irc -- /help for commands", false);

	if (z_cfg_get("apps.irc.server", cfg, sizeof(cfg)) && cfg[0]) {
		char host[IRC_TARGET_MAX + 192];
		int port = 0;
		char *sp = strchr(cfg, ' ');
		if (sp) { *sp = 0; port = atoi(sp + 1); }
		snprintf(host, sizeof(host), "%s", cfg);
		connect_to(host, (uint16_t)((port > 0 && port < 65536) ? port : 6667));
	} else {
		add_line("", "-- /connect host [port] to start (or set apps.irc.server)", false);
	}

	repaint();

	for (;;) {

		z_msg_t msg;

		while (z_msg_read(&msg) == Z_OK) handle_msg(&msg);

		if (dirty) repaint();

		// Sleep until a message arrives or a tick passes: the server
		// can take seconds to say anything, and a chat client that
		// spins takes a full scheduler share from everything else
		// (docs/app_runtime.md).
		z_proc_wait(Z_TICK_HZ / 30);

	}

	return 0;

}
