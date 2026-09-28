/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zetta -- the Zeitlos Editor for TexT Applications. docs/zetta.md; the
 * interface in zetta.h.
 *
 * -- the text --
 *
 * The caller's buffer holds UTF-8 paragraphs: '\n' only where Enter was
 * pressed. What is shown is each paragraph wrapped at words to the text
 * width -- computed when needed, from the row at the top of the screen,
 * so there are no row tables and a long text costs nothing more. One
 * function, row_span(), decides every wrap, for the screen and for
 * zetta_text(), so what is saved is what was shown.
 *
 * The text is one column narrower than the screen: writing the last
 * column would leave a terminal about to wrap.
 *
 * -- the screen --
 *
 *   row 0             the title, reversed
 *   rows 1..nfields   the app's header fields: "Subject: ..."
 *   ...               the text (or a dialog's list, or the help)
 *   rows-3            the status line: a message, a question, a prompt
 *   rows-2, rows-1    the keys
 *
 * Each row is built, fingerprinted, and sent only if its fingerprint
 * changed: a caller on a slow link sees a keystroke cost one row, not a
 * screen.
 */
#include <string.h>
#include <stdio.h>
#include "zetta.h"

enum { M_EDIT = 0, M_MENU, M_CONFIRM, M_PROMPT, M_PICK, M_HELP };

#define ESC_MS    150			// a lone Esc: nothing after it for this long

// -- UTF-8 --

static uint32_t next_ch(const zetta_t *ed, uint32_t i) {
	if (i >= ed->len) return ed->len;
	i++;
	while (i < ed->len && ((uint8_t)ed->buf[i] & 0xC0) == 0x80) i++;
	return i;
}

static uint32_t prev_ch(const zetta_t *ed, uint32_t i) {
	if (!i) return 0;
	i--;
	while (i && ((uint8_t)ed->buf[i] & 0xC0) == 0x80) i--;
	return i;
}

static int width_of(const zetta_t *ed) {
	int w = ed->cfg.cols - 1;
	return w < 10 ? 10 : w;
}

// -- the layout --

static uint32_t para_start(const zetta_t *ed, uint32_t i) {
	while (i > 0 && ed->buf[i - 1] != '\n') i--;
	return i;
}

static uint32_t para_end(const zetta_t *ed, uint32_t i) {
	while (i < ed->len && ed->buf[i] != '\n') i++;
	return i;
}

// The row starting at s, `w` columns wide: what it shows is [s, *end);
// the next row starts at *next. A row ends at the last space that fits
// (shown at its end); a space falling just past the edge is taken into
// the row unshown; a word wider than the row is cut.
static void row_span_w(const char *b, uint32_t len, uint32_t s, int w, uint32_t *end, uint32_t *next) {
	uint32_t pe = s, i = s, lastsp = 0;
	int n = 0;
	while (pe < len && b[pe] != '\n') pe++;
	while (i < pe && n < w) {
		if (b[i] == ' ') lastsp = i + 1;
		i++;
		while (i < pe && ((uint8_t)b[i] & 0xC0) == 0x80) i++;
		n++;
	}
	if (i >= pe) { *end = pe; *next = pe < len ? pe + 1 : len; return; }
	if (b[i] == ' ') { *end = i; *next = i + 1; return; }
	if (lastsp > s) { *end = lastsp; *next = lastsp; return; }
	*end = i; *next = i;
}

static void row_span(const zetta_t *ed, uint32_t s, uint32_t *end, uint32_t *next) {
	row_span_w(ed->buf, ed->len, s, width_of(ed), end, next);
}

static uint32_t row_of(const zetta_t *ed, uint32_t p);

// Is there a row starting at s? At the very end, an empty one exists when
// the text is empty, ends with Enter, or its last row wrapped exactly at
// the end -- a space taken in just past the edge: the cursor after it
// goes to the next row, as in nano. (The random-key test found the last
// case: the cursor was on a row that was never drawn.)
static bool row_exists(const zetta_t *ed, uint32_t s) {
	if (s < ed->len) return true;
	if (s != ed->len) return false;
	if (ed->len == 0 || ed->buf[ed->len - 1] == '\n') return true;
	uint32_t e, nx;
	row_span(ed, row_of(ed, ed->len - 1), &e, &nx);
	return nx == ed->len && e < ed->len;
}

// The start of the row holding position p.
static uint32_t row_of(const zetta_t *ed, uint32_t p) {
	uint32_t s = para_start(ed, p), e, nx;
	for (;;) {
		row_span(ed, s, &e, &nx);
		if (p < nx || e == ed->len || nx == s) return s;
		s = nx;
	}
}

// The row after s, or s itself if there is none.
static uint32_t row_next(const zetta_t *ed, uint32_t s) {
	uint32_t e, nx;
	row_span(ed, s, &e, &nx);
	if (nx == s || !row_exists(ed, nx)) return s;
	return nx;
}

// The row before s, or s itself at the first.
static uint32_t row_prev(const zetta_t *ed, uint32_t s) {
	if (s == 0) return 0;
	return row_of(ed, s - 1);
}

// Characters from s to p.
static int cols_between(const zetta_t *ed, uint32_t s, uint32_t p) {
	int n = 0;
	for (uint32_t i = s; i < p; i = next_ch(ed, i)) n++;
	return n;
}

// The position `col` characters into the row at s, kept inside the row.
static uint32_t pos_at(const zetta_t *ed, uint32_t s, int col) {
	uint32_t e, nx, i = s;
	row_span(ed, s, &e, &nx);
	for (int n = 0; n < col && i < e; n++) i = next_ch(ed, i);
	// the end of a row that wraps with no space is the next row's start:
	// one back, so the cursor stays on this row
	if (i == nx && nx == e && e != para_end(ed, s) && i > s) i = prev_ch(ed, i);
	return i;
}

// -- the screen --

static int text_top(const zetta_t *ed) { return 1 + ed->cfg.nfields; }
static int text_rows(const zetta_t *ed) { int h = ed->cfg.rows - 3 - text_top(ed); return h < 1 ? 1 : h; }
static int status_row(const zetta_t *ed) { return ed->cfg.rows - 3; }

static void out(zetta_t *ed, const char *s, uint32_t n) { if (ed->cfg.write && n) ed->cfg.write(ed->cfg.ctx, s, n); }
static void outs(zetta_t *ed, const char *s) { out(ed, s, (uint32_t)strlen(s)); }

// Keeps the cursor's row on the screen.
static void scroll_to_cursor(zetta_t *ed) {
	// an edit may have reflowed the text: the top back to a row's start
	if (ed->top > ed->len) ed->top = ed->len;
	ed->top = row_of(ed, ed->top);
	uint32_t cr = row_of(ed, ed->cur);
	if (cr < ed->top) { ed->top = cr; return; }
	int n = 0;
	for (uint32_t s = ed->top; s != cr; ) {
		uint32_t nx = row_next(ed, s);
		if (nx == s) break;
		s = nx;
		n++;
	}
	while (n >= text_rows(ed)) { ed->top = row_next(ed, ed->top); n--; }
}

static uint32_t fnv(const char *s, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

// A row's content: the bytes to draw it (with SGR for reverse), then
// "erase to the end of the line". Sent only if different.
static void put_row(zetta_t *ed, int r, const char *content, uint32_t n) {
	uint32_t h = fnv(content, n);
	if (ed->drawn && ed->shown[r] == h) return;
	ed->shown[r] = h;
	char pos[16];
	int pl = snprintf(pos, sizeof(pos), "\x1b[%d;1H", r + 1);
	out(ed, pos, (uint32_t)pl);
	out(ed, content, n);
	outs(ed, "\x1b[0m\x1b[K");
}

// Appends up to `max` characters of s (UTF-8) to b; the characters used.
static int add_chars(char *b, uint32_t *bl, uint32_t cap, const char *s, uint32_t n, int max) {
	int c = 0;
	for (uint32_t i = 0; i < n && c < max; ) {
		uint32_t j = i + 1;
		while (j < n && ((uint8_t)s[j] & 0xC0) == 0x80) j++;
		if (*bl + (j - i) >= cap) break;
		for (uint32_t k = i; k < j; k++) b[(*bl)++] = (s[k] == '\t' || ((uint8_t)s[k] < 0x20)) ? ' ' : s[k];
		c++;
		i = j;
	}
	return c;
}

static void pad(char *b, uint32_t *bl, uint32_t cap, int n) {
	while (n-- > 0 && *bl + 1 < cap) b[(*bl)++] = ' ';
}

// The two rows of keys, the app's among them.
static void key_name(uint32_t k, char *out, int cap) {
	if (k >= 1 && k <= 26) snprintf(out, (size_t)cap, "^%c", (char)('@' + k));
	else if (k == ZK_ESC) snprintf(out, (size_t)cap, "Esc");
	else if (k >= 0x20 && k < 0x7f) snprintf(out, (size_t)cap, "%c", (char)k);
	else snprintf(out, (size_t)cap, "?");
}

static void help_rows(zetta_t *ed) {
	char b[ZETTA_COLS_MAX * 2 + 64], kn[8];
	uint32_t bl = 0;
	int used = 0;
	// row 1: the app's actions, then Ctrl-Z for its main one
	for (int i = 0; i < ed->cfg.nactions; i++) {
		const zetta_action_t *a = &ed->cfg.actions[i];
		if (!a->key) continue;
		key_name(a->key, kn, sizeof(kn));
		int need = (int)strlen(kn) + 1 + (int)strlen(a->label) + 2;
		if (used + need > ed->cfg.cols) break;
		bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m%s\x1b[0m %s  ", kn, a->label);
		used += need;
	}
	put_row(ed, ed->cfg.rows - 2, b, bl);
	bl = 0;
	static const char *const std[][2] = {
		{ "^G", "Help" }, { "^X", "Exit" }, { "^K", "Cut" }, { "^U", "Paste" }, { "^W", "Search" }, { "Esc", "Menu" },
	};
	used = 0;
	for (unsigned i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
		int need = (int)strlen(std[i][0]) + 1 + (int)strlen(std[i][1]) + 2;
		if (used + need > ed->cfg.cols) break;
		bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m%s\x1b[0m %s  ", std[i][0], std[i][1]);
		used += need;
	}
	put_row(ed, ed->cfg.rows - 1, b, bl);
}

static const char *const help_text[] = {
	"Moving:     the arrows, Home, End, PgUp, PgDn; ^A start of line, ^E end",
	"Editing:    type; Enter starts a new paragraph -- lines wrap by themselves",
	"            Backspace, Delete; ^K cuts the paragraph, ^U pastes it back",
	"Finding:    ^W, then the words",
	"Fields:     Up from the first line goes to the fields above; Enter leaves one",
	"Menu:       Esc -- everything, for a terminal whose Ctrl keys are awkward",
	"Leaving:    ^X; the app says what else (post, save) in the bar below",
	"",
	"Any key goes back.",
};

static void render(zetta_t *ed) {
	char b[ZETTA_COLS_MAX * 4 + 64];
	uint32_t bl;
	int cols = ed->cfg.cols, w = width_of(ed);

	// the title
	bl = 0;
	bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m ");
	int c = 1 + add_chars(b, &bl, sizeof(b), ed->cfg.title ? ed->cfg.title : "zetta",
		ed->cfg.title ? (uint32_t)strlen(ed->cfg.title) : 5, cols - 12);
	pad(b, &bl, sizeof(b), cols - c - 10);
	c = cols - 10;
	const char *m = ed->modified ? "Modified" : "";
	bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "%s", m);
	pad(b, &bl, sizeof(b), cols - c - (int)strlen(m));
	put_row(ed, 0, b, bl);

	// the header fields
	for (int f = 0; f < ed->cfg.nfields; f++) {
		const zetta_field_t *fd = &ed->cfg.fields[f];
		bl = 0;
		bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "%s%s: ", ed->focus == f ? "\x1b[7m" : "", fd->label);
		if (ed->focus == f) bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[0m");
		add_chars(b, &bl, sizeof(b), fd->buf, (uint32_t)strlen(fd->buf), w - (int)strlen(fd->label) - 2);
		put_row(ed, 1 + f, b, bl);
	}

	// the text area: the text, or a dialog over it
	int top = text_top(ed), h = text_rows(ed);
	if (ed->mode == M_HELP) {
		for (int r = 0; r < h; r++) {
			bl = 0;
			if (r < (int)(sizeof(help_text) / sizeof(help_text[0])))
				add_chars(b, &bl, sizeof(b), help_text[r], (uint32_t)strlen(help_text[r]), w);
			put_row(ed, top + r, b, bl);
		}
	} else if (ed->mode == M_PICK || ed->mode == M_MENU) {
		int n = ed->mode == M_PICK ? ed->pick_n : ed->cfg.nactions + 3;
		int at = ed->mode == M_PICK ? ed->pick_at : ed->menu_at;
		if (ed->mode == M_PICK) {
			if (at < ed->pick_top) ed->pick_top = at;
			if (at >= ed->pick_top + h - 1) ed->pick_top = at - h + 2;
		}
		int first = ed->mode == M_PICK ? ed->pick_top : 0;
		for (int r = 0; r < h; r++) {
			bl = 0;
			if (r == 0) {
				const char *t = ed->mode == M_PICK ? "Space chooses lines, Enter puts them in, Esc goes back" :
					"Up and Down choose, Enter does it, Esc goes back";
				bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m");
				add_chars(b, &bl, sizeof(b), t, (uint32_t)strlen(t), w);
				put_row(ed, top + r, b, bl);
				continue;
			}
			int i = first + r - 1;
			if (i < n) {
				if (i == at) bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m");
				if (ed->mode == M_PICK) {
					const char *line = ed->pick_fn(ed->pick_ctx, i);
					bool on = i < 256 && (ed->pick_on[i / 8] & (1u << (i % 8)));
					bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "[%c] ", on ? 'x' : ' ');
					if (line) add_chars(b, &bl, sizeof(b), line, (uint32_t)strlen(line), w - 4);
				} else {
					const char *label;
					char kn[8] = "";
					if (i < ed->cfg.nactions) { label = ed->cfg.actions[i].label; key_name(ed->cfg.actions[i].key, kn, sizeof(kn)); }
					else if (i == ed->cfg.nactions) { label = "Search"; strcpy(kn, "^W"); }
					else if (i == ed->cfg.nactions + 1) { label = "Help"; strcpy(kn, "^G"); }
					else { label = "Exit"; strcpy(kn, "^X"); }
					if (i < ed->cfg.nactions && !ed->cfg.actions[i].key) kn[0] = 0;
					bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "  %-24s %s", label, kn);
				}
			}
			put_row(ed, top + r, b, bl);
		}
	} else {
		scroll_to_cursor(ed);
		uint32_t s = ed->top;
		bool more = true;
		for (int r = 0; r < h; r++) {
			bl = 0;
			if (more && row_exists(ed, s)) {
				uint32_t e, nx;
				row_span(ed, s, &e, &nx);
				add_chars(b, &bl, sizeof(b), ed->buf + s, e - s, w);
				uint32_t n2 = row_next(ed, s);
				if (n2 == s) more = false;
				s = n2;
			} else more = false;
			put_row(ed, top + r, b, bl);
		}
	}

	// the status line: a question, a prompt, or a message
	bl = 0;
	int cur_col = 0;
	if (ed->mode == M_CONFIRM) {
		bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m%s (y/n)\x1b[0m ", ed->prompt_q);
		cur_col = (int)strlen(ed->prompt_q) + 7;
	} else if (ed->mode == M_PROMPT) {
		bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m%s\x1b[0m ", ed->prompt_q);
		int qc = (int)strlen(ed->prompt_q) + 1;
		add_chars(b, &bl, sizeof(b), ed->prompt_buf, (uint32_t)strlen(ed->prompt_buf), w - qc);
		cur_col = qc;
		for (uint32_t i = 0; i < ed->prompt_cur; ) {
			uint32_t j = i + 1;
			while (ed->prompt_buf[j] && ((uint8_t)ed->prompt_buf[j] & 0xC0) == 0x80) j++;
			cur_col++; i = j;
		}
	} else if (ed->status[0]) {
		if (ed->status_error) bl += (uint32_t)snprintf(b + bl, sizeof(b) - bl, "\x1b[7m");
		add_chars(b, &bl, sizeof(b), ed->status, (uint32_t)strlen(ed->status), w);
	}
	put_row(ed, status_row(ed), b, bl);
	help_rows(ed);
	ed->drawn = true;

	// the cursor
	int cr = 0, cc = 0;
	if (ed->mode == M_CONFIRM || ed->mode == M_PROMPT) { cr = status_row(ed); cc = cur_col; }
	else if (ed->mode == M_PICK) { cr = top + 1 + ed->pick_at - ed->pick_top; cc = 1; }
	else if (ed->mode == M_MENU) { cr = top + 1 + ed->menu_at; cc = 0; }
	else if (ed->mode == M_HELP) { cr = top; cc = 0; }
	else if (ed->focus >= 0) {
		const zetta_field_t *fd = &ed->cfg.fields[ed->focus];
		cr = 1 + ed->focus;
		cc = (int)strlen(fd->label) + 2;
		for (uint32_t i = 0; i < ed->fcur; ) {
			uint32_t j = i + 1;
			while (fd->buf[j] && ((uint8_t)fd->buf[j] & 0xC0) == 0x80) j++;
			cc++; i = j;
		}
	} else {
		uint32_t rs = row_of(ed, ed->cur);
		int n = 0;
		for (uint32_t s = ed->top; s != rs; ) { uint32_t nx = row_next(ed, s); if (nx == s) break; s = nx; n++; }
		cr = top + n;
		cc = cols_between(ed, rs, ed->cur);
	}
	if (cc > cols - 1) cc = cols - 1;
	char pos[16];
	int pl = snprintf(pos, sizeof(pos), "\x1b[%d;%dH", cr + 1, cc + 1);
	out(ed, pos, (uint32_t)pl);
}

// -- editing --

static bool insert(zetta_t *ed, const char *s, uint32_t n) {
	if (ed->len + n + 1 > ed->cap) { zetta_error(ed, "The text is full."); return false; }
	memmove(ed->buf + ed->cur + n, ed->buf + ed->cur, ed->len - ed->cur);
	memcpy(ed->buf + ed->cur, s, n);
	ed->len += n;
	ed->cur += n;
	ed->buf[ed->len] = 0;
	ed->modified = true;
	return true;
}

static void erase(zetta_t *ed, uint32_t from, uint32_t to) {
	if (to <= from) return;
	memmove(ed->buf + from, ed->buf + to, ed->len - to);
	ed->len -= to - from;
	ed->buf[ed->len] = 0;
	if (ed->cur > to) ed->cur -= to - from;
	else if (ed->cur > from) ed->cur = from;
	if (ed->top > from) ed->top = from;			// scroll_to_cursor() puts it on a row
	ed->modified = true;
}

static void move_rows(zetta_t *ed, int d) {
	uint32_t s = row_of(ed, ed->cur);
	if (ed->want_col < 0) ed->want_col = cols_between(ed, s, ed->cur);
	for (; d < 0; d++) { uint32_t p = row_prev(ed, s); if (p == s) break; s = p; }
	for (; d > 0; d--) { uint32_t n = row_next(ed, s); if (n == s) break; s = n; }
	ed->cur = pos_at(ed, s, ed->want_col);
}

static void cut_paragraph(zetta_t *ed) {
	if (!ed->cfg.clip || !ed->cfg.clip_cap) { zetta_error(ed, "There is nowhere to cut to here."); return; }
	uint32_t s = para_start(ed, ed->cur), e = para_end(ed, ed->cur);
	if (e < ed->len) e++;							// and its Enter
	if (s == e) return;
	if (!ed->clip_append) ed->clip_len = 0;
	uint32_t n = e - s;
	if (ed->clip_len + n + 1 > ed->cfg.clip_cap) { zetta_error(ed, "Too much to cut at once."); return; }
	memcpy(ed->cfg.clip + ed->clip_len, ed->buf + s, n);
	ed->clip_len += n;
	ed->cfg.clip[ed->clip_len] = 0;
	erase(ed, s, e);
	ed->cur = s;
	ed->clip_append = true;
}

static void find(zetta_t *ed) {
	uint32_t n = (uint32_t)strlen(ed->search);
	if (!n) return;
	for (uint32_t k = 0; k < ed->len; k++) {
		uint32_t i = (ed->cur + 1 + k) % (ed->len ? ed->len : 1);
		if (i + n > ed->len) continue;
		uint32_t j = 0;
		for (; j < n; j++) {
			char a = ed->buf[i + j], b = ed->search[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
			if (a != b) break;
		}
		if (j == n) { ed->cur = i; ed->want_col = -1; zetta_status(ed, ""); return; }
	}
	zetta_error(ed, "Not found.");
}

// -- the header fields --

static void field_leave(zetta_t *ed, int to) {
	zetta_field_t *fd = &ed->cfg.fields[ed->focus];
	const char *why = fd->check ? fd->check(fd->buf, ed->cfg.ctx) : NULL;
	if (why) { zetta_error(ed, why); return; }
	zetta_status(ed, "");
	ed->focus = to;
	if (to >= 0) ed->fcur = (uint32_t)strlen(ed->cfg.fields[to].buf);
}

static void field_key(zetta_t *ed, uint32_t k) {
	zetta_field_t *fd = &ed->cfg.fields[ed->focus];
	uint32_t len = (uint32_t)strlen(fd->buf);
	if (k == ZK_ENTER || k == ZK_TAB || k == ZK_DOWN) { field_leave(ed, ed->focus + 1 < ed->cfg.nfields ? ed->focus + 1 : -1); return; }
	if (k == ZK_UP) { if (ed->focus > 0) field_leave(ed, ed->focus - 1); return; }
	if (k == ZK_LEFT) { while (ed->fcur && ((uint8_t)fd->buf[--ed->fcur] & 0xC0) == 0x80) ; return; }
	if (k == ZK_RIGHT) { if (ed->fcur < len) { ed->fcur++; while (ed->fcur < len && ((uint8_t)fd->buf[ed->fcur] & 0xC0) == 0x80) ed->fcur++; } return; }
	if (k == ZK_HOME || k == ZK_CTRL('A')) { ed->fcur = 0; return; }
	if (k == ZK_END || k == ZK_CTRL('E')) { ed->fcur = len; return; }
	if (k == ZK_BS || k == ZK_DEL) {
		uint32_t a = ed->fcur, b = ed->fcur;
		if (k == ZK_BS) { if (!a) return; a--; while (a && ((uint8_t)fd->buf[a] & 0xC0) == 0x80) a--; }
		else { if (b >= len) return; b++; while (b < len && ((uint8_t)fd->buf[b] & 0xC0) == 0x80) b++; }
		memmove(fd->buf + a, fd->buf + b, len - b + 1);
		ed->fcur = a;
		ed->modified = true;
		return;
	}
	if (k >= 0x20 && k < 0x110000 && k != 0x7f) {
		char u[4];
		int n = k < 0x80 ? 1 : k < 0x800 ? 2 : k < 0x10000 ? 3 : 4;
		if (n == 1) u[0] = (char)k;
		else if (n == 2) { u[0] = (char)(0xC0 | (k >> 6)); u[1] = (char)(0x80 | (k & 0x3F)); }
		else if (n == 3) { u[0] = (char)(0xE0 | (k >> 12)); u[1] = (char)(0x80 | ((k >> 6) & 0x3F)); u[2] = (char)(0x80 | (k & 0x3F)); }
		else { u[0] = (char)(0xF0 | (k >> 18)); u[1] = (char)(0x80 | ((k >> 12) & 0x3F)); u[2] = (char)(0x80 | ((k >> 6) & 0x3F)); u[3] = (char)(0x80 | (k & 0x3F)); }
		if (len + (uint32_t)n + 1 > fd->cap) return;
		memmove(fd->buf + ed->fcur + n, fd->buf + ed->fcur, len - ed->fcur + 1);
		memcpy(fd->buf + ed->fcur, u, (size_t)n);
		ed->fcur += (uint32_t)n;
		ed->modified = true;
	}
}

// -- one key --

static int utf8_put(uint32_t k, char *u) {
	if (k < 0x80) { u[0] = (char)k; return 1; }
	if (k < 0x800) { u[0] = (char)(0xC0 | (k >> 6)); u[1] = (char)(0x80 | (k & 0x3F)); return 2; }
	if (k < 0x10000) { u[0] = (char)(0xE0 | (k >> 12)); u[1] = (char)(0x80 | ((k >> 6) & 0x3F)); u[2] = (char)(0x80 | (k & 0x3F)); return 3; }
	u[0] = (char)(0xF0 | (k >> 18)); u[1] = (char)(0x80 | ((k >> 12) & 0x3F)); u[2] = (char)(0x80 | ((k >> 6) & 0x3F)); u[3] = (char)(0x80 | (k & 0x3F));
	return 4;
}

static int run_action(zetta_t *ed, int i) {
	if (i < ed->cfg.nactions) { ed->action = ed->cfg.actions[i].id; return ZE_ACTION; }
	if (i == ed->cfg.nactions) { zetta_prompt(ed, "Search:", ed->search, sizeof(ed->search), -1); return ZE_NONE; }
	if (i == ed->cfg.nactions + 1) { ed->mode = M_HELP; return ZE_NONE; }
	return ZE_EXIT;
}

static int key(zetta_t *ed, uint32_t k) {
	char u[4];
	if (k != ZK_CTRL('K')) ed->clip_append = false;

	switch (ed->mode) {
	case M_HELP:
		ed->mode = M_EDIT;
		return ZE_NONE;
	case M_CONFIRM:
		if (k == 'y' || k == 'Y' || k == 'n' || k == 'N' || k == ZK_ESC) {
			ed->mode = M_EDIT;
			ed->answer = k == ZK_ESC ? 0 : (k == 'y' || k == 'Y') ? 'y' : 'n';
			ed->answer_tag = ed->prompt_tag;
			ed->prompt_q[0] = 0;
			return ZE_ANSWER;
		}
		return ZE_NONE;
	case M_PROMPT: {
		uint32_t len = (uint32_t)strlen(ed->prompt_buf);
		if (k == ZK_ENTER || k == ZK_ESC) {
			ed->mode = M_EDIT;
			ed->answer = k == ZK_ENTER ? 'y' : 0;
			ed->answer_tag = ed->prompt_tag;
			if (ed->prompt_tag == -1) { if (ed->answer) find(ed); return ZE_NONE; }	// our own: search
			return ZE_ANSWER;
		}
		if (k == ZK_BS && ed->prompt_cur) {
			uint32_t a = ed->prompt_cur - 1;
			while (a && ((uint8_t)ed->prompt_buf[a] & 0xC0) == 0x80) a--;
			memmove(ed->prompt_buf + a, ed->prompt_buf + ed->prompt_cur, len - ed->prompt_cur + 1);
			ed->prompt_cur = a;
		} else if (k >= 0x20 && k < 0x110000 && k != 0x7f) {
			int n = utf8_put(k, u);
			if (len + (uint32_t)n + 1 <= ed->prompt_cap) {
				memmove(ed->prompt_buf + ed->prompt_cur + n, ed->prompt_buf + ed->prompt_cur, len - ed->prompt_cur + 1);
				memcpy(ed->prompt_buf + ed->prompt_cur, u, (size_t)n);
				ed->prompt_cur += (uint32_t)n;
			}
		}
		return ZE_NONE;
	}
	case M_PICK:
		if (k == ZK_UP && ed->pick_at > 0) ed->pick_at--;
		else if (k == ZK_DOWN && ed->pick_at + 1 < ed->pick_n) ed->pick_at++;
		else if (k == ' ' && ed->pick_at < 256) ed->pick_on[ed->pick_at / 8] ^= (uint8_t)(1u << (ed->pick_at % 8));
		else if (k == ZK_ESC) { ed->mode = M_EDIT; ed->answer = 0; ed->answer_tag = ed->prompt_tag; return ZE_ANSWER; }
		else if (k == ZK_ENTER) {
			bool any = false;
			for (int i = 0; i < ed->pick_n && i < 256; i++) if (ed->pick_on[i / 8] & (1u << (i % 8))) any = true;
			for (int i = 0; i < ed->pick_n && i < 256; i++) {
				// nothing chosen: the line under the cursor
				bool on = any ? (ed->pick_on[i / 8] & (1u << (i % 8))) != 0 : i == ed->pick_at;
				if (!on) continue;
				const char *l = ed->pick_fn(ed->pick_ctx, i);
				if (!l) continue;
				if (!insert(ed, ed->pick_prefix, (uint32_t)strlen(ed->pick_prefix)) ||
						!insert(ed, l, (uint32_t)strlen(l)) || !insert(ed, "\n", 1)) break;
			}
			ed->mode = M_EDIT;
			ed->answer = 'y';
			ed->answer_tag = ed->prompt_tag;
			return ZE_ANSWER;
		}
		return ZE_NONE;
	case M_MENU: {
		int n = ed->cfg.nactions + 3;
		if (k == ZK_UP && ed->menu_at > 0) ed->menu_at--;
		else if (k == ZK_DOWN && ed->menu_at + 1 < n) ed->menu_at++;
		else if (k == ZK_ESC) ed->mode = M_EDIT;
		else if (k == ZK_ENTER) { ed->mode = M_EDIT; return run_action(ed, ed->menu_at); }
		return ZE_NONE;
	}
	default:
		break;
	}

	// a message lasts until the next key
	ed->status[0] = 0;
	ed->status_error = false;

	// the app's own keys first
	for (int i = 0; i < ed->cfg.nactions; i++)
		if (ed->cfg.actions[i].key && ed->cfg.actions[i].key == k) { ed->action = ed->cfg.actions[i].id; return ZE_ACTION; }
	if (k == ZK_ESC) { ed->mode = M_MENU; ed->menu_at = 0; return ZE_NONE; }
	if (k == ZK_CTRL('X')) return ZE_EXIT;
	if (k == ZK_CTRL('Z')) { if (ed->cfg.main_action >= 0) { ed->action = ed->cfg.main_action; return ZE_ACTION; } return ZE_NONE; }
	if (k == ZK_CTRL('G')) { ed->mode = M_HELP; return ZE_NONE; }
	if (k == ZK_CTRL('W')) { zetta_prompt(ed, "Search:", ed->search, sizeof(ed->search), -1); return ZE_NONE; }

	if (ed->focus >= 0) { field_key(ed, k); return ZE_NONE; }

	if (k != ZK_UP && k != ZK_DOWN && k != ZK_PGUP && k != ZK_PGDN) ed->want_col = -1;
	switch (k) {
	case ZK_LEFT: ed->cur = prev_ch(ed, ed->cur); break;
	case ZK_RIGHT: ed->cur = next_ch(ed, ed->cur); break;
	case ZK_UP:
		if (row_of(ed, ed->cur) == 0 && ed->cfg.nfields > 0) { ed->focus = ed->cfg.nfields - 1; ed->fcur = (uint32_t)strlen(ed->cfg.fields[ed->focus].buf); }
		else move_rows(ed, -1);
		break;
	case ZK_DOWN: move_rows(ed, 1); break;
	case ZK_PGUP: move_rows(ed, -(text_rows(ed) - 1)); break;
	case ZK_PGDN: move_rows(ed, text_rows(ed) - 1); break;
	case ZK_HOME: case ZK_CTRL('A'): ed->cur = row_of(ed, ed->cur); break;
	case ZK_END: case ZK_CTRL('E'): { uint32_t s = row_of(ed, ed->cur), e, nx; row_span(ed, s, &e, &nx); ed->cur = pos_at(ed, s, 1 << 30); (void)e; (void)nx; break; }
	case ZK_BS: if (ed->cur) { uint32_t p = prev_ch(ed, ed->cur); erase(ed, p, ed->cur); } break;
	case ZK_DEL: if (ed->cur < ed->len) erase(ed, ed->cur, next_ch(ed, ed->cur)); break;
	case ZK_ENTER: insert(ed, "\n", 1); break;
	case ZK_TAB: { int col = cols_between(ed, row_of(ed, ed->cur), ed->cur); int n = 4 - col % 4; while (n--) if (!insert(ed, " ", 1)) break; break; }
	case ZK_CTRL('K'): cut_paragraph(ed); break;
	case ZK_CTRL('U'): if (ed->cfg.clip && ed->clip_len) insert(ed, ed->cfg.clip, ed->clip_len); break;
	default:
		if (k >= 0x20 && k < 0x110000 && k != 0x7f) {
			int n = utf8_put(k, u);
			insert(ed, u, (uint32_t)n);
		}
	}
	return ZE_NONE;
}

// -- bytes to keys --

// An escape sequence complete in ed->esc: its key, 0 not yet, -1 not one.
static int esc_key(zetta_t *ed) {
	uint8_t *e = ed->esc;
	int n = ed->esc_len;
	if (n < 2) return 0;
	if (e[1] != '[' && e[1] != 'O') return -1;
	if (n < 3) return 0;
	uint8_t last = e[n - 1];
	if (e[1] == 'O' || (e[1] == '[' && !(last >= '0' && last <= '9') && last != ';')) {
		switch (last) {
		case 'A': return (int)ZK_UP; case 'B': return (int)ZK_DOWN; case 'C': return (int)ZK_RIGHT; case 'D': return (int)ZK_LEFT;
		case 'H': return (int)ZK_HOME; case 'F': return (int)ZK_END;
		case '~': {
			int v = 0;
			for (int i = 2; i < n - 1 && e[i] >= '0' && e[i] <= '9'; i++) v = v * 10 + (e[i] - '0');
			switch (v) {
			case 1: case 7: return (int)ZK_HOME; case 4: case 8: return (int)ZK_END;
			case 2: return (int)ZK_INS; case 3: return (int)ZK_DEL; case 5: return (int)ZK_PGUP; case 6: return (int)ZK_PGDN;
			}
			return -1;
		}
		}
		return -1;
	}
	return n >= (int)sizeof(ed->esc) ? -1 : 0;
}

static int decode(zetta_t *ed, uint8_t b, uint32_t now_ms) {
	if (ed->esc_len) {
		ed->esc[ed->esc_len++] = b;
		int k = esc_key(ed);
		if (k == 0) return ZE_NONE;
		ed->esc_len = 0;
		if (k > 0) return key(ed, (uint32_t)k);
		// not a sequence: Esc, then the byte for itself (Alt-x, or a slow Esc)
		int r = key(ed, ZK_ESC);
		if (r) return r;
		return decode(ed, b, now_ms);
	}
	if (b == 0x1b) { ed->esc[0] = b; ed->esc_len = 1; ed->esc_ms = now_ms; return ZE_NONE; }
	if (ed->utf_need) {
		if ((b & 0xC0) != 0x80) { ed->utf_need = 0; return decode(ed, b, now_ms); }
		ed->utf[ed->utf_len++] = b;
		if (ed->utf_len < ed->utf_need) return ZE_NONE;
		uint32_t cp = ed->utf[0] & (ed->utf_need == 2 ? 0x1F : ed->utf_need == 3 ? 0x0F : 0x07);
		for (int i = 1; i < ed->utf_need; i++) cp = (cp << 6) | (ed->utf[i] & 0x3F);
		ed->utf_need = 0;
		return key(ed, cp);
	}
	if (b >= 0xC2 && b <= 0xF4) { ed->utf[0] = b; ed->utf_len = 1; ed->utf_need = b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4; return ZE_NONE; }
	if (b >= 0x80) return ZE_NONE;						// not UTF-8: dropped
	bool cr = ed->last_cr;
	ed->last_cr = b == '\r';
	if (b == '\n' && cr) return ZE_NONE;				// \r\n: one Enter
	if (b == '\r' || b == '\n') return key(ed, ZK_ENTER);
	if (b == 0x7f || b == 0x08) return key(ed, ZK_BS);
	if (b == '\t') return key(ed, ZK_TAB);
	return key(ed, b);
}

// -- the interface --

void zetta_init(zetta_t *ed, const zetta_cfg_t *cfg, char *buf, uint32_t cap) {
	memset(ed, 0, sizeof(*ed));
	ed->cfg = *cfg;
	if (ed->cfg.rows > ZETTA_ROWS_MAX) ed->cfg.rows = ZETTA_ROWS_MAX;
	if (ed->cfg.cols > ZETTA_COLS_MAX) ed->cfg.cols = ZETTA_COLS_MAX;
	if (ed->cfg.nfields > ZETTA_FIELDS_MAX) ed->cfg.nfields = ZETTA_FIELDS_MAX;
	if (ed->cfg.nactions > ZETTA_ACTIONS_MAX) ed->cfg.nactions = ZETTA_ACTIONS_MAX;
	ed->buf = buf;
	ed->cap = cap;
	ed->len = 0;
	while (cap && ed->len < cap - 1 && buf[ed->len]) ed->len++;
	ed->buf[ed->len] = 0;
	ed->focus = -1;
	ed->want_col = -1;
	// with empty fields, the first of them to start with
	for (int i = 0; i < ed->cfg.nfields; i++) if (!ed->cfg.fields[i].buf[0]) { ed->focus = i; break; }
	outs(ed, "\x1b[0m\x1b[2J");
	render(ed);
}

void zetta_redraw(zetta_t *ed) {
	ed->drawn = false;
	outs(ed, "\x1b[0m\x1b[2J");
	render(ed);
}

// Bytes that arrived after an event: kept for the next call.
static int pending(zetta_t *ed, const uint8_t *d, uint32_t n, uint32_t now_ms);

int zetta_key(zetta_t *ed, const uint8_t *bytes, uint32_t n, uint32_t now_ms) {
	int r = pending(ed, bytes, n, now_ms);
	render(ed);
	return r;
}

// Bytes after an event are held IN THE EDITOR -- the BBS has one a
// caller -- and taken first next time: one event a call, so the app can
// act on each, and no keystroke lost.
static int pending(zetta_t *ed, const uint8_t *d, uint32_t n, uint32_t now_ms) {
	static uint8_t work[ZETTA_HOLD + 256];
	uint32_t wn = 0;
	if (ed->hold_len) { memcpy(work, ed->hold, ed->hold_len); wn = ed->hold_len; ed->hold_len = 0; }
	if (n > sizeof(work) - wn) n = (uint32_t)sizeof(work) - wn;
	memcpy(work + wn, d, n);
	wn += n;
	for (uint32_t i = 0; i < wn; i++) {
		int r = decode(ed, work[i], now_ms);
		if (r != ZE_NONE) {
			uint32_t rest = wn - i - 1;
			if (rest > ZETTA_HOLD) rest = ZETTA_HOLD;
			memcpy(ed->hold, work + i + 1, rest);
			ed->hold_len = rest;
			return r;
		}
	}
	return ZE_NONE;
}

int zetta_key_code(zetta_t *ed, uint32_t k) {
	int r = key(ed, k);
	render(ed);
	return r;
}

int zetta_tick(zetta_t *ed, uint32_t now_ms) {
	int r = ZE_NONE;
	if (ed->esc_len == 1 && (int32_t)(now_ms - ed->esc_ms) >= ESC_MS) {
		ed->esc_len = 0;
		r = key(ed, ZK_ESC);
	}
	if (r == ZE_NONE && ed->hold_len) r = pending(ed, NULL, 0, now_ms);
	render(ed);
	return r;
}

// Shown at once: an app sets these while handling an event, after
// zetta_key() has drawn -- waiting for the next key would leave "Saved"
// or "A subject, please." unseen until then. Only the changed rows go.
void zetta_status(zetta_t *ed, const char *msg) {
	snprintf(ed->status, sizeof(ed->status), "%s", msg ? msg : "");
	ed->status_error = false;
	if (ed->drawn) render(ed);
}

void zetta_error(zetta_t *ed, const char *msg) {
	snprintf(ed->status, sizeof(ed->status), "%s", msg ? msg : "");
	ed->status_error = true;
	if (ed->drawn) render(ed);
}

void zetta_refresh(zetta_t *ed) {
	if (ed->drawn) render(ed);
}

void zetta_confirm(zetta_t *ed, const char *question, int tag) {
	snprintf(ed->prompt_q, sizeof(ed->prompt_q), "%s", question);
	ed->prompt_tag = tag;
	ed->mode = M_CONFIRM;
	render(ed);
}

void zetta_prompt(zetta_t *ed, const char *question, char *buf, uint32_t cap, int tag) {
	snprintf(ed->prompt_q, sizeof(ed->prompt_q), "%s", question);
	ed->prompt_buf = buf;
	ed->prompt_cap = cap;
	ed->prompt_cur = (uint32_t)strlen(buf);
	ed->prompt_tag = tag;
	ed->mode = M_PROMPT;
	render(ed);
}

void zetta_pick(zetta_t *ed, const char *title, zetta_lines_fn fn, void *ctx, const char *prefix, int tag) {
	(void)title;
	ed->pick_fn = fn;
	ed->pick_ctx = ctx;
	snprintf(ed->pick_prefix, sizeof(ed->pick_prefix), "%s", prefix ? prefix : "");
	ed->pick_n = 0;
	while (ed->pick_n < 10000 && fn(ctx, ed->pick_n)) ed->pick_n++;
	ed->pick_at = ed->pick_top = 0;
	memset(ed->pick_on, 0, sizeof(ed->pick_on));
	ed->prompt_tag = tag;
	ed->mode = ed->pick_n ? M_PICK : M_EDIT;
	render(ed);
}

uint32_t zetta_text(const zetta_t *ed, char *out, uint32_t cap, int cols) {
	uint32_t o = 0;
	if (!cap) return 0;
	if (cols <= 0) {
		o = ed->len < cap - 1 ? ed->len : cap - 1;
		memcpy(out, ed->buf, o);
		out[o] = 0;
		return o;
	}
	uint32_t s = 0;
	while (s < ed->len) {
		uint32_t e, nx;
		row_span_w(ed->buf, ed->len, s, cols, &e, &nx);
		uint32_t t = e;
		while (t > s && ed->buf[t - 1] == ' ' && nx != e + 1) t--;	// a wrap's trailing space: not kept
		if (o + (t - s) + 2 > cap) break;
		memcpy(out + o, ed->buf + s, t - s);
		o += t - s;
		out[o++] = '\n';
		if (nx == s) break;
		s = nx;
	}
	// the text ends where it ended: no Enter added past the last paragraph
	if (o && ed->len && ed->buf[ed->len - 1] != '\n') o--;
	out[o] = 0;
	return o;
}
