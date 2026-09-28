/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- everything a caller sees goes through here.
 *
 * One design for every terminal (docs/bbs.md, "Screens"): what a
 * screen means is carried by its text, its layout and reverse video;
 * colour only decorates, and is dropped for a caller without it --
 * Zeitlos's own `term` is one bit deep. Text is UTF-8 until the last
 * moment, then sent as the caller's terminal wants it: UTF-8, CP437, or
 * ASCII with line drawing as - | +.
 *
 * Text that came from a caller -- a handle, a location -- is written
 * with out_text(), which lets no control character through: nobody gets
 * to send escape sequences to everyone who reads the user list.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bbs_int.h"

uint32_t out_room(const node_t *n) {
	return BBS_OUT_RING - n->out_len;
}

void out_raw(node_t *n, const void *d, uint32_t len) {
	const uint8_t *p = d;
	if (len > out_room(n)) {
		// Nobody is reading (a stalled connection): keep what fits and
		// say so once. A screen is well under the ring's size.
		if (!n->out_lost) bbs_logf("node output full: %u bytes dropped", (unsigned)(len - out_room(n)));
		n->out_lost = true;
		len = out_room(n);
	}
	for (uint32_t i = 0; i < len; i++) {
		n->out[(n->out_head + n->out_len) % BBS_OUT_RING] = p[i];
		n->out_len++;
	}
}

static void out_cp(node_t *n, uint32_t cp) {
	char b[4];
	if (n->charset == CS_UTF8) {
		int k = utf8_put(cp, b);
		out_raw(n, b, (uint32_t)k);
	} else if (n->charset == CS_CP437) {
		uint8_t c = cp437_from(cp);
		if (!c) c = (uint8_t)ascii_from(cp);
		out_raw(n, &c, 1);
	} else {
		char c = ascii_from(cp);
		out_raw(n, &c, 1);
	}
}

void out_textn(node_t *n, const char *s, uint32_t len) {
	const char *end = s + len;
	while (s < end) {
		uint32_t cp = utf8_next(&s, end);
		if (cp == '\n') { OUT_LIT(n, "\r\n"); continue; }
		if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) continue;   // controls: never
		out_cp(n, cp);
	}
}

void out_text(node_t *n, const char *s) {
	out_textn(n, s, (uint32_t)strlen(s));
}

void out_nl(node_t *n) {
	OUT_LIT(n, "\r\n");
}

void out_attr_reset(node_t *n) {
	if (n->ansi) OUT_LIT(n, "\x1b[0m");
}

void out_rev(node_t *n, bool on) {
	if (!n->ansi) return;
	if (on) OUT_LIT(n, "\x1b[7m");
	else OUT_LIT(n, "\x1b[27m");
}

// DOS colour numbers (the pipe codes' 0-15) to ANSI's 0-7.
static const uint8_t dos_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

void out_fg(node_t *n, int c) {
	char b[16];
	if (!n->ansi || !n->color || c < 0 || c > 15) return;
	int k = snprintf(b, sizeof(b), "\x1b[%d;3%dm", c >= 8 ? 1 : 22, dos_ansi[c & 7]);
	out_raw(n, b, (uint32_t)k);
}

static void out_bg(node_t *n, int c) {
	char b[12];
	if (!n->ansi || !n->color || c < 0 || c > 7) return;
	int k = snprintf(b, sizeof(b), "\x1b[4%dm", dos_ansi[c]);
	out_raw(n, b, (uint32_t)k);
}

void out_cls(node_t *n) {
	if (n->ansi) OUT_LIT(n, "\x1b[0m\x1b[2J\x1b[H");
	else OUT_LIT(n, "\r\n\r\n");
}

void out_rule(node_t *n) {
	int w = (n->cols > 1 ? n->cols : 80) - 1;
	out_fg(n, 8);
	for (int i = 0; i < w; i++) out_cp(n, 0x2500);        // ─
	out_fg(n, 7);
	out_nl(n);
}

void out_title(node_t *n, const char *title) {
	char line[160];
	int w = (n->cols > 1 ? n->cols : 80) - 1;
	out_cls(n);
	// " name │ title": the separator is a line, which every terminal
	// here can draw -- CP437 has it, ASCII makes it |, and Zeitlos's
	// own term has it since phase 2. Not an em dash: Latin-9 has none,
	// and term would show the missing-glyph box.
	snprintf(line, sizeof(line), " %s \xE2\x94\x82 %s", bbs_cfg.name, title);
	int len = utf8_chars(line);
	if (n->ansi) {
		// The bar is reverse video on every terminal; colour, where
		// there is any, only tints it.
		out_rev(n, true);
		out_fg(n, 15);
		out_text(n, line);
		for (int i = len; i < w; i++) OUT_LIT(n, " ");
		out_attr_reset(n);
		out_nl(n);
	} else {
		out_text(n, line);
		out_nl(n);
		out_rule(n);
	}
	out_nl(n);
}

/* -- pipe codes --
 *
 * Bulletins and the screens in <datadir>/text are written with the
 * codes Mystic, Renegade and friends use, so text brought over from
 * such a board keeps its look:
 *
 *   |00-|15  foreground colour (DOS order: 7 light grey, 15 white)
 *   |16-|23  background colour
 *   |CL      clear the screen       |CR  a new line
 *   |BN      the BBS's name         |SN  the sysop
 *   |UH      the caller's handle    |ND  their node number
 *   |DA      the date and time now (UTC)
 *   |RV |RO  reverse video on, off -- ours, for meaning that must
 *            survive a terminal without colour
 *   ||       a |
 *
 * Anything else after a | is printed as it is. */
// Does the two-character code at c do something? Does it, if `n`.
static bool mci_code(node_t *n, char a, char b) {
	char tmp[24];
	if (a >= '0' && a <= '9' && b >= '0' && b <= '9') {
		int v = (a - '0') * 10 + (b - '0');
		if (v > 23) return false;
		if (n) { if (v <= 15) out_fg(n, v); else out_bg(n, v - 16); }
		return true;
	}
	if (a == 'C' && b == 'L') { if (n) out_cls(n); return true; }
	if (a == 'C' && b == 'R') { if (n) out_nl(n); return true; }
	if (a == 'B' && b == 'N') { if (n) out_text(n, bbs_cfg.name); return true; }
	if (a == 'S' && b == 'N') { if (n) out_text(n, bbs_cfg.sysop); return true; }
	if (a == 'U' && b == 'H') { if (n) out_text(n, n->logged_in ? n->user.handle : ""); return true; }
	if (a == 'N' && b == 'D') {
		if (n) {
			int idx = 0;
			for (int i = 0; i < BBS_NODES_MAX; i++) if (bbs_node[i] == n) idx = i;
			snprintf(tmp, sizeof(tmp), "%d", idx + 1);
			out_text(n, tmp);
		}
		return true;
	}
	if (a == 'D' && b == 'A') { if (n) { fmt_time(plat_now(), tmp); out_text(n, tmp); } return true; }
	if (a == 'R' && b == 'V') { if (n) out_rev(n, true); return true; }
	if (a == 'R' && b == 'O') { if (n) out_rev(n, false); return true; }
	return false;
}

void out_mcin(node_t *n, const char *s, uint32_t len) {
	const char *end = s + len, *run = s;
	while (s < end) {
		if (*s != '|') { s++; continue; }
		if (s + 1 < end && s[1] == '|') {
			out_textn(n, run, (uint32_t)(s - run));
			OUT_LIT(n, "|");
			s += 2;
			run = s;
			continue;
		}
		if (s + 3 > end || !mci_code(NULL, s[1], s[2])) { s++; continue; }   // text
		out_textn(n, run, (uint32_t)(s - run));
		mci_code(n, s[1], s[2]);
		s += 3;
		run = s;
	}
	out_textn(n, run, (uint32_t)(end - run));
}

void out_mci(node_t *n, const char *s) {
	out_mcin(n, s, (uint32_t)strlen(s));
}

void out_fmt(node_t *n, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	out_mci(n, buf);
}
