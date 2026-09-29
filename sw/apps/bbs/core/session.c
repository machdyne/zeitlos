/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the callers: one state machine per node, nothing that waits.
 * docs/bbs.md.
 *
 * Every screen is drawn in response to something -- a key, a timer --
 * and returns at once; a screen too long for one go is a pager that
 * draws a page and waits for a key. Input a byte at a time, whatever
 * the platform delivered: escape sequences and UTF-8 are reassembled
 * here, and a CP437 caller's bytes are turned into characters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbs_int.h"

node_t *bbs_node[BBS_NODES_MAX];
static int n_nodes;

// keys that are not characters: K_* in bbs_int.h

#define DETECT_MS   2500      // for the first answer: no answer, no ANSI
#define DETECT2_MS  1000      // for the second (the screen size)
#define ESC_MS       300      // a lone ESC, not the start of a sequence

static void on_key(node_t *n, uint32_t k);
static void on_field(node_t *n);
static void welcome(node_t *n);
static void profile_show(node_t *n);
static void sysop_show(node_t *n);

int node_index(const node_t *n) {
	for (int i = 0; i < n_nodes; i++) if (bbs_node[i] == n) return i;
	return -1;
}

void set_state(node_t *n, nstate_t s) {
	n->state = s;
	n->since_ms = plat_ms();
}

static void bye(node_t *n, const char *why) {
	lastread_save(n);
	editor_free(n);
	if (why && *why) { out_nl(n); out_mci(n, why); out_nl(n); }
	out_attr_reset(n);
	set_state(n, N_BYE);
	n->close_after = true;
	n->field_on = false;
}

// ------------------------------------------------------------------
// the platform's side
// ------------------------------------------------------------------

bool bbs_init(const char *datadir) {
	snprintf(bbs_dir, sizeof(bbs_dir), "%s", datadir);
	size_t l = strlen(bbs_dir);
	while (l > 1 && bbs_dir[l - 1] == '/') bbs_dir[--l] = 0;
	if (!plat_mkdir(bbs_dir)) {
		bbs_logf("the data directory %s cannot be made", bbs_dir);
		return false;
	}
	char p[BBS_PATH_MAX];
	if (bbs_path(p, "bulletins")) plat_mkdir(p);
	if (bbs_path(p, "text")) plat_mkdir(p);
	if (!cfg_load()) return false;
	if (!node_id_load()) return false;
	if (!areas_load()) return false;
	fed_init();
	for (int i = 0; i < n_nodes; i++) { free(bbs_node[i]); bbs_node[i] = NULL; }
	n_nodes = 0;
	for (int i = 0; i < bbs_cfg.nodes; i++) {
		bbs_node[i] = calloc(1, sizeof(node_t));
		if (!bbs_node[i]) break;
		n_nodes++;
	}
	if (n_nodes < bbs_cfg.nodes)
		bbs_logf("memory for %d nodes, not %d", n_nodes, bbs_cfg.nodes);
	bbs_logf("%s: %d nodes, %d users, data in %s", bbs_cfg.name, n_nodes, users_count(), bbs_dir);
	// federated forums with nothing to federate them: said, not left to be
	// found out when posts go nowhere
	for (int a = 1; a < bbs_nareas; a++)
		if (bbs_area[a].topic[0] && !bbs_cfg.fed[0]) {
			bbs_logf("forums have zfed topics, but bbs.cfg has no fed: line -- they stay on this BBS (docs/bbs.md, \"Federation\")");
			break;
		}
	return n_nodes > 0;
}

int bbs_nodes(void) { return n_nodes; }

const char *bbs_busy_text(void) {
	return "\r\nEvery line is busy -- please call again in a little while.\r\n";
}

int bbs_connect(const bbs_conn_t *who) {
	for (int i = 0; i < n_nodes; i++) {
		node_t *n = bbs_node[i];
		if (n->state != N_FREE) continue;
		memset(n, 0, sizeof(*n));
		snprintf(n->transport, sizeof(n->transport), "%s", who->transport ? who->transport : "?");
		snprintf(n->peer, sizeof(n->peer), "%s", who->peer ? who->peer : "");
		utf8_copy(n->offered, sizeof(n->offered), who->user ? who->user : "");
		n->connected_at = plat_now();
		n->connected_ms = n->last_input_ms = plat_ms();
		n->cols = 80;
		n->rows = 24;
		n->charset = CS_ASCII;
		bbs_logf("node %d: %s call from %s", i + 1, n->transport, n->peer[0] ? n->peer : "here");
		/* Ask the terminal what it is: print ─ (three bytes of UTF-8)
		 * and ask where the cursor went. One column: it reads UTF-8.
		 * Three: it took each byte as a character -- an 8-bit
		 * terminal, CP437 in the BBS world. No answer: no ANSI at all,
		 * and then all this looks like a little noise, which is why it
		 * is sent once, at the start, before anything else. */
		set_state(n, N_DETECT);
		OUT_LIT(n, "\r\xE2\x94\x80\x1b[6n");
		return i;
	}
	return -1;
}

void bbs_hangup(int i) {
	if (i < 0 || i >= n_nodes) return;
	node_t *n = bbs_node[i];
	if (n->state == N_FREE) return;
	if (n->logged_in) bbs_logf("node %d: %s left", i + 1, n->user.handle);
	else bbs_logf("node %d: hung up", i + 1);
	lastread_save(n);
	editor_free(n);
	memset(n->pending_pw, 0, sizeof(n->pending_pw));
	memset(n->field, 0, sizeof(n->field));
	n->state = N_FREE;
}

uint32_t bbs_output(int i, const uint8_t **p) {
	if (i < 0 || i >= n_nodes) return 0;
	node_t *n = bbs_node[i];
	if (n->state == N_FREE || !n->out_len) return 0;
	*p = n->out + n->out_head;
	uint32_t run = BBS_OUT_RING - n->out_head;
	return n->out_len < run ? n->out_len : run;
}

void bbs_consumed(int i, uint32_t k) {
	if (i < 0 || i >= n_nodes) return;
	node_t *n = bbs_node[i];
	if (k > n->out_len) k = n->out_len;
	n->out_head = (n->out_head + k) % BBS_OUT_RING;
	n->out_len -= k;
	if (!n->out_len) { n->out_head = 0; n->out_lost = false; }
}

bool bbs_wants_close(int i) {
	if (i < 0 || i >= n_nodes) return false;
	node_t *n = bbs_node[i];
	return n->state != N_FREE && n->close_after && !n->out_len;
}

bool bbs_busy(void) {
	for (int i = 0; i < n_nodes; i++) if (bbs_node[i]->state != N_FREE) return true;
	return false;
}

// ------------------------------------------------------------------
// detection
// ------------------------------------------------------------------

static void detect_done(node_t *n) {
	if (!n->ansi) {
		// Nothing answered: whatever the probe printed is on this
		// line. Move off it and carry on in plain text.
		OUT_LIT(n, "\r\n");
		n->charset = CS_ASCII;
	} else {
		OUT_LIT(n, "\x1b[0m\x1b[H\x1b[2J");
	}
	// Colour for every ANSI terminal. Zeitlos's own term has none and
	// ignores it, which costs nothing; each user can turn it off.
	n->color = n->ansi;
	welcome(n);
}

// An answer to CSI 6n: ESC [ row ; col R.
static void detect_reply(node_t *n, int row, int col) {
	if (n->state != N_DETECT) return;
	if (n->detect_step == 0) {
		n->ansi = true;
		n->charset = (col == 2) ? CS_UTF8 : CS_CP437;
		n->detect_step = 1;
		n->since_ms = plat_ms();
		// Now the size: as far down and right as it goes, and ask.
		OUT_LIT(n, "\x1b[999;999H\x1b[6n");
		return;
	}
	if (row >= 10 && row <= 100) n->rows = row;
	if (col >= 40 && col <= 250) n->cols = col;
	detect_done(n);
}

// ------------------------------------------------------------------
// input
// ------------------------------------------------------------------

static void esc_done(node_t *n) {
	uint8_t *e = n->in_esc;
	int len = n->in_esc_n;
	n->in_esc_n = 0;
	if (len >= 3 && e[1] == '[' && e[len - 1] == 'R') {
		int row = 0, col = 0, *v = &row;
		for (int i = 2; i < len - 1; i++) {
			if (e[i] == ';') v = &col;
			else if (e[i] >= '0' && e[i] <= '9') *v = *v * 10 + (e[i] - '0');
		}
		detect_reply(n, row, col);
		return;
	}
	if (len == 3 && (e[1] == '[' || e[1] == 'O')) {
		switch (e[2]) {
		case 'A': on_key(n, K_UP); return;
		case 'B': on_key(n, K_DOWN); return;
		case 'C': on_key(n, K_RIGHT); return;
		case 'D': on_key(n, K_LEFT); return;
		}
	}
	// anything else (function keys, a paste bracket ...): ignored
}

static void in_byte(node_t *n, uint8_t c) {
	// an escape sequence in progress
	if (n->in_esc_n) {
		if (n->in_esc_n == 1 && c != '[' && c != 'O') {
			n->in_esc_n = 0;
			on_key(n, K_ESC);
			// and this byte as itself, below
		} else {
			if (n->in_esc_n < sizeof(n->in_esc)) n->in_esc[n->in_esc_n++] = c;
			if (n->in_esc_n > 2 && c >= 0x40 && c <= 0x7E) esc_done(n);
			else if (n->in_esc_n >= sizeof(n->in_esc)) n->in_esc_n = 0;
			return;
		}
	}
	if (c == 0x1b) { n->in_esc[0] = c; n->in_esc_n = 1; n->esc_ms = plat_ms(); return; }
	if (n->state == N_DETECT) return;               // typeahead during the probe: dropped

	// UTF-8 or CP437 characters
	if (n->in_need) {
		if ((c & 0xC0) == 0x80) {
			n->in_cp = (n->in_cp << 6) | (c & 0x3F);
			if (--n->in_need == 0) on_key(n, n->in_cp);
			return;
		}
		n->in_need = 0;
	}
	if (c >= 0x80) {
		if (n->charset == CS_UTF8) {
			if (c >= 0xC2 && c <= 0xDF) { n->in_cp = c & 0x1F; n->in_need = 1; }
			else if (c >= 0xE0 && c <= 0xEF) { n->in_cp = c & 0x0F; n->in_need = 2; }
			else if (c >= 0xF0 && c <= 0xF4) { n->in_cp = c & 0x07; n->in_need = 3; }
			return;
		}
		if (n->charset == CS_CP437) on_key(n, cp437_to(c));
		return;                                     // ASCII: nothing above 0x7F
	}
	if (c == '\r') { on_key(n, K_ENTER); return; }
	if (c == '\n' || c == 0) return;                // after a CR: the platforms send CR for Enter
	if (c == 0x08 || c == 0x7F) { on_key(n, K_BS); return; }
	if (c < 0x20) { on_key(n, c); return; }         // Ctrl-C and friends, as themselves
	on_key(n, c);
}

void bbs_input(int i, const uint8_t *d, uint32_t len) {
	if (i < 0 || i >= n_nodes) return;
	node_t *n = bbs_node[i];
	if (n->state == N_FREE) return;
	n->last_input_ms = plat_ms();
	// writing full-screen: the bytes are zetta's -- it decodes the keys
	if (n->state == N_ZETTA) { zed_input(n, d, len); return; }
	for (uint32_t k = 0; k < len && n->state != N_FREE; k++) {
		if (n->state == N_BYE) return;
		in_byte(n, d[k]);
	}
}

// ------------------------------------------------------------------
// fields: the line being typed
// ------------------------------------------------------------------

void field(node_t *n, nstate_t st, const char *prompt, int max, bool mask, const char *prefill) {
	set_state(n, st);
	out_mci(n, prompt);
	n->field_on = true;
	n->field_mask = mask;
	n->field_max = max > 0 && max < FIELD_MAX / 4 ? max : FIELD_MAX / 4;
	n->field[0] = 0;
	if (prefill && *prefill) {
		utf8_copy(n->field, sizeof(n->field), prefill);
		if (mask) for (int i = utf8_chars(n->field); i > 0; i--) OUT_LIT(n, "*");
		else out_text(n, n->field);
	}
}

static void field_key(node_t *n, uint32_t k) {
	size_t len = strlen(n->field);
	if (k == K_ENTER) {
		n->field_on = false;
		out_nl(n);
		on_field(n);
		return;
	}
	if (k == K_BS || k == K_LEFT) {
		if (!len) return;
		// back one character, not one byte
		size_t i = len - 1;
		while (i > 0 && ((uint8_t)n->field[i] & 0xC0) == 0x80) i--;
		n->field[i] = 0;
		OUT_LIT(n, "\b \b");
		return;
	}
	if (k == 0x15) {                                // Ctrl-U: the whole line
		for (int i = utf8_chars(n->field); i > 0; i--) OUT_LIT(n, "\b \b");
		n->field[0] = 0;
		return;
	}
	if (k < 0x20 || k >= 0x110000 || k == 0x7F || (k >= 0x80 && k < 0xA0)) return;
	if (utf8_chars(n->field) >= n->field_max) {
		if (n->state == N_EDIT) editor_wrap(n, k);    // the last word moves down
		return;
	}
	char b[4];
	int bl = utf8_put(k, b);
	if (len + (size_t)bl + 1 > sizeof(n->field)) return;
	memcpy(n->field + len, b, (size_t)bl);
	n->field[len + (size_t)bl] = 0;
	if (n->field_mask) OUT_LIT(n, "*");
	else {
		char one[5];
		memcpy(one, b, (size_t)bl);
		one[bl] = 0;
		out_text(n, one);
	}
}

// "-- press a key --", and then `then`.
void anykey(node_t *n, nstate_t then) {
	n->after_key = then;
	set_state(n, N_ANYKEY);
	out_mci(n, "\r\n|07-- press a key --|07");
}

static bool is_yes(uint32_t k) { return k == 'y' || k == 'Y' || k == K_ENTER; }
uint32_t upper(uint32_t k) { return (k >= 'a' && k <= 'z') ? k - 32 : k; }

// A key on a screen that has one, not a field.
static void key_prompt(node_t *n, const char *text) {
	out_mci(n, text);
}

// ------------------------------------------------------------------
// the pager: text a screen at a time
// ------------------------------------------------------------------

static char pg_buf[4096];          // one page of a file; one caller at a time uses it

static int page_lines(node_t *n) {
	int r = n->rows - 2;
	return r < 5 ? 5 : r;
}

static bool is_ans(const char *path) {
	size_t l = strlen(path);
	return l > 4 && (!strcmp(path + l - 4, ".ans") || !strcmp(path + l - 4, ".ANS"));
}

// Writes a CP437 ANSI-art line: characters translated, escape sequences
// through as they are -- to an ANSI terminal; to anything else they are
// dropped, sequence and all.
static void out_ans(node_t *n, const char *s, uint32_t len) {
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)s[i];
		if (c == 0x1b) {
			uint32_t j = i + 1;
			if (j < len && s[j] == '[') {
				j++;
				while (j < len && !((uint8_t)s[j] >= 0x40 && (uint8_t)s[j] <= 0x7E)) j++;
				if (j < len && n->ansi) out_raw(n, s + i, j - i + 1);
				i = j;
				continue;
			}
			continue;
		}
		if (c == '\r') continue;
		if (c == '\n') { out_nl(n); continue; }
		if (c == 0x1a) break;                       // SAUCE and what follows
		if (c < 0x20) continue;
		char u[4];
		int k = utf8_put(cp437_to(c), u);
		out_textn(n, u, (uint32_t)k);
	}
}

// Up to the page's remaining lines from pg_path at pg_off. True when the
// file has ended.
static bool page_file(node_t *n) {
	int h = plat_open(n->pg_path, PLAT_READ);
	if (h < 0) return true;
	int got = plat_seek(h, n->pg_off) ? plat_read(h, pg_buf, (int)sizeof(pg_buf)) : -1;
	plat_close(h);
	bool msg = n->pg_kind == PG_MSG;
	if (msg && got > 0 && n->pg_off + (uint32_t)got > n->pg_end)
		got = n->pg_off < n->pg_end ? (int)(n->pg_end - n->pg_off) : 0;
	if (got <= 0) return true;
	bool ans = !msg && is_ans(n->pg_path);
	int at = 0;
	while (at < got && n->pg_line < page_lines(n) && out_room(n) >= BBS_PAGE_ROOM) {
		int eol = at;
		while (eol < got && pg_buf[eol] != '\n') eol++;
		if (eol == got && got == (int)sizeof(pg_buf) && at > 0) break;   // the rest next time
		int len = eol - at;
		if (len && pg_buf[at + len - 1] == '\r') len--;
		// A message is a caller's text: shown as it is, no codes.
		if (msg) out_textn(n, pg_buf + at, (uint32_t)len);
		else if (ans) out_ans(n, pg_buf + at, (uint32_t)len);
		else out_mcin(n, pg_buf + at, (uint32_t)len);
		out_attr_reset(n);
		out_nl(n);
		n->pg_line++;
		at = eol + 1;
	}
	n->pg_off += (uint32_t)(at > got ? got : at);
	if (msg) return n->pg_off >= n->pg_end;
	return at >= got && got < (int)sizeof(pg_buf);
}

// The bulletins: <datadir>/bulletins/*.txt and *.ans, in name order.
static char bl_names[1024];

static int bulletin_list(void) {
	char dir[BBS_PATH_MAX];
	if (!bbs_path(dir, "bulletins")) return 0;
	int k = plat_list(dir, bl_names, sizeof(bl_names));
	if (k <= 0) return 0;
	// keep the .txt and .ans ones
	char *w = bl_names;
	int kept = 0;
	for (char *p = bl_names; k-- > 0; p += strlen(p) + 1) {
		size_t l = strlen(p);
		bool ok = l > 4 && (!strcmp(p + l - 4, ".txt") || !strcmp(p + l - 4, ".TXT") || is_ans(p));
		if (!ok) continue;
		memmove(w, p, l + 1);
		w += l + 1;
		kept++;
	}
	return kept;
}

static const char *bulletin_name(int i) {
	const char *p = bl_names;
	while (i-- > 0) p += strlen(p) + 1;
	return p;
}

// Moves to the next bulletin to show (pg_count), newer than pg_newer.
// False when there are no more.
static bool bulletin_next(node_t *n) {
	int total = bulletin_list();
	while (n->pg_count < total) {
		char rel[BBS_PATH_MAX];
		const char *bn = bulletin_name(n->pg_count);
		n->pg_count++;
		if (strlen(bn) + 11 > sizeof(rel)) continue;        // a name too long for a path: skipped
		memcpy(rel, "bulletins/", 10);
		strcpy(rel + 10, bn);
		if (!bbs_path(n->pg_path, rel)) continue;
		if (n->pg_newer && plat_mtime(n->pg_path) <= n->pg_newer) continue;
		// a title for it: the file name without the extension, and
		// without a leading "01-" that is only there to sort it
		char title[64];
		const char *name = bn;
		const char *t = name;
		while (*t >= '0' && *t <= '9') t++;
		if (t != name && (*t == '-' || *t == '_' || *t == ' ')) t++;
		else t = name;
		utf8_copy(title, sizeof(title), t);
		char *dot = strrchr(title, '.');
		if (dot) *dot = 0;
		for (char *c = title; *c; c++) if (*c == '_') *c = ' ';
		out_title(n, title);
		n->pg_line = 2;
		n->pg_off = 0;
		return true;
	}
	return false;
}

static void who_line(node_t *n, int i, char *out, size_t cap) {
	node_t *o = bbs_node[i];
	const char *doing = "";
	switch (o->state) {
	case N_DETECT: case N_LOGIN_NAME: case N_LOGIN_PASS: doing = "logging in"; break;
	case N_NEW_NAME: case N_NEW_PASS: case N_NEW_PASS2: case N_NEW_LOC: case N_NEW_CONFIRM:
		doing = "joining"; break;
	case N_PAGER:
		doing = o->pg_kind == PG_BULLETINS ? "reading bulletins" : o->pg_kind == PG_USERS ?
			"looking at the users" : o->pg_kind == PG_WHO ? "looking at who is on" : "reading"; break;
	case N_MENU: case N_ANYKEY: doing = "at the menu"; break;
	case N_AREAS: case N_AREA_MENU: case N_READ: doing = "reading messages"; break;
	case N_POST_TO: case N_POST_SUBJ: case N_QUOTE_ASK: case N_EDIT: case N_ZETTA: doing = "writing a message"; break;
	case N_BYE: doing = "leaving"; break;
	default:
		doing = (o->state >= N_PROFILE && o->state <= N_PROF_ROWS) ? "in their profile" : "sysop work";
		break;
	}
	uint32_t mins = (plat_ms() - o->connected_ms) / 60000u;
	char h[BBS_HANDLE_MAX * 4 + 1];
	utf8_pad(h, sizeof(h), o->logged_in ? o->user.handle : "-", BBS_HANDLE_MAX);
	snprintf(out, cap, " %2d  %s  %-6s  %-22s %4u min%s", i + 1, h, o->transport, doing,
		(unsigned)mins, o == n ? "  (you)" : "");
}

// Up to the page's remaining lines of the current list. True when the
// list has ended.
static bool page_list(node_t *n) {
	char line[400];
	while (n->pg_line < page_lines(n) && out_room(n) >= BBS_PAGE_ROOM) {
		if (n->pg_kind == PG_WHO) {
			while ((int)n->pg_off < n_nodes && bbs_node[n->pg_off]->state == N_FREE) n->pg_off++;
			if ((int)n->pg_off >= n_nodes) return true;
			who_line(n, (int)n->pg_off, line, sizeof(line));
		} else {
			user_t u;
			if ((int)n->pg_off >= n->pg_count) return true;
			if (!users_read((int)n->pg_off, &u)) { n->pg_off++; continue; }
			char last[20];
			fmt_time(u.last_login, last);
			char h[BBS_HANDLE_MAX * 4 + 1], loc[24 * 4 + 1];
			utf8_pad(h, sizeof(h), u.handle, BBS_HANDLE_MAX);
			utf8_pad(loc, sizeof(loc), u.location, 24);
			snprintf(line, sizeof(line), " %4u  %s  %s  %s%s", (unsigned)u.id, h, loc, last,
				(u.flags & USER_F_DISABLED) ? "  (disabled)" : (u.flags & USER_F_SYSOP) ? "  (sysop)" : "");
		}
		out_text(n, line);
		out_nl(n);
		n->pg_off++;
		n->pg_line++;
	}
	return false;
}

static void pager_end(node_t *n) {
	nstate_t then = n->pg_then;
	n->pg_kind = PG_NONE;
	if (then == N_READ) reader_after_body(n);
	else if (then == N_MENU) show_menu(n);
	else anykey(n, N_MENU);
}

// Draws the next page, or ends.
void pager_run(node_t *n) {
	for (;;) {
		bool ended;
		if (n->pg_kind == PG_FILE || n->pg_kind == PG_BULLETINS || n->pg_kind == PG_MSG) {
			if (n->pg_kind == PG_BULLETINS && !n->pg_path[0]) {
				if (!bulletin_next(n)) { pager_end(n); return; }
			}
			ended = page_file(n);
			if (ended && n->pg_kind == PG_BULLETINS) {
				// on to the next bulletin, after a pause
				n->pg_path[0] = 0;
				n->pg_line = page_lines(n);
				set_state(n, N_PAGER);
				key_prompt(n, "|RV -- Enter to go on, Q to stop -- |RO");
				return;
			}
		} else ended = page_list(n);
		if (ended) { pager_end(n); return; }
		// A full page -- or a full output ring, on a small machine with
		// a dense page: the rest after a key, never cut off.
		if (n->pg_line >= page_lines(n) || out_room(n) < BBS_PAGE_ROOM) {
			set_state(n, N_PAGER);
			key_prompt(n, "|RV -- Enter to go on, Q to stop -- |RO");
			return;
		}
	}
}

static void pager_key(node_t *n, uint32_t k) {
	// erase the prompt, then carry on or stop
	if (n->ansi) OUT_LIT(n, "\r\x1b[K"); else out_nl(n);
	if (upper(k) == 'Q' || k == K_ESC) {
		bool msg = n->pg_kind == PG_MSG;
		n->pg_kind = PG_NONE;
		if (msg) reader_after_body(n);          // the rest of a message skipped: its prompt
		else show_menu(n);
		return;
	}
	n->pg_line = 0;
	if (n->pg_kind == PG_BULLETINS && !n->pg_path[0]) n->pg_line = 0;
	pager_run(n);
}

void pager_start(node_t *n, int kind, nstate_t then) {
	n->pg_kind = kind;
	n->pg_then = then;
	n->pg_off = 0;
	n->pg_line = 0;
	n->pg_count = 0;
	n->pg_path[0] = 0;
	set_state(n, N_PAGER);
	pager_run(n);
}

// ------------------------------------------------------------------
// arriving: the welcome screen, the login
// ------------------------------------------------------------------

// <datadir>/text/<name>.txt if the sysop wrote one, else nothing.
static bool show_text(node_t *n, const char *name) {
	char rel[64], path[BBS_PATH_MAX];
	snprintf(rel, sizeof(rel), "text/%s.txt", name);
	if (!bbs_path(path, rel) || plat_size(path) <= 0) return false;
	snprintf(n->pg_path, sizeof(n->pg_path), "%s", path);
	n->pg_off = 0;
	n->pg_line = -1000;          // no paging: these are one screen by definition
	for (int guard = 0; guard < 64 && out_room(n) >= BBS_PAGE_ROOM && !page_file(n); guard++) ;
	n->pg_path[0] = 0;
	return true;
}

static void ask_handle(node_t *n) {
	const char *pre = "";
	char key[40];
	handle_key(n->offered, key, sizeof(key));
	// What the SSH client offered, when it could be a handle: `ssh
	// phil@bbs...` starts with "phil" typed, `ssh new@bbs...` with NEW.
	// Not "bbs" -- the Linux server's SSH account (docs/bbs.md).
	if (n->offered[0] && strcmp(key, "bbs") && (!strcmp(key, "new") || !handle_problem(n->offered)))
		pre = n->offered;
	field(n, N_LOGIN_NAME, bbs_cfg.new_users ? "|15Handle|07 (or |15NEW|07 to join): " :
		"|15Handle|07: ", BBS_HANDLE_MAX, false, pre);
}

static void welcome(node_t *n) {
	if (!show_text(n, "welcome")) {
		out_title(n, "Welcome");
		out_fmt(n, "  Welcome to |15%s|07.\r\n", bbs_cfg.name);
		if (bbs_cfg.sysop[0]) out_fmt(n, "  Your sysop is %s.\r\n", bbs_cfg.sysop);
		out_nl(n);
		out_fmt(n, "  |07Node %d of %d  -  %s  -  %s%s|07\r\n", node_index(n) + 1, n_nodes,
			n->ansi ? (n->charset == CS_UTF8 ? "ANSI, UTF-8" : "ANSI, CP437") : "plain text",
			n->transport, !strcmp(n->transport, "telnet") ? " (not encrypted)" : "");
		out_nl(n);
	}
	ask_handle(n);
}

static void apply_prefs(node_t *n) {
	user_t *u = &n->user;
	if (u->charset != CS_AUTO) {
		n->charset = u->charset;
		// UTF-8 or CP437 by choice implies the caller knows its
		// terminal speaks ANSI; ASCII keeps whatever was found
		if (u->charset != CS_ASCII) n->ansi = true;
	}
	if (u->color == COLOR_ON) n->color = n->ansi;
	if (u->color == COLOR_OFF) n->color = false;
	if (u->rows >= 10) n->rows = u->rows;
}

static void logged_in(node_t *n) {
	user_t *u = &n->user;
	n->logged_in = true;
	u->prev_login = u->last_login;
	u->last_login = plat_now();
	u->calls++;
	users_write(u);
	apply_prefs(n);
	lastread_load(n);
	bbs_logf("node %d: %s logged in (%s%s%s)", node_index(n) + 1, u->handle, n->transport,
		n->peer[0] ? " from " : "", n->peer);
	// Bulletins new since the last call -- all of them on the first.
	n->pg_newer = u->prev_login;
	n->pg_count = 0;
	n->pg_path[0] = 0;
	if (bulletin_list() && bulletin_next(n)) {
		n->pg_kind = PG_BULLETINS;
		n->pg_then = N_MENU;
		n->pg_off = 0;
		set_state(n, N_PAGER);
		pager_run(n);
		return;
	}
	show_menu(n);
}

static void login_name(node_t *n) {
	char key[FIELD_MAX];
	handle_key(n->field, key, sizeof(key));
	if (!n->field[0]) {
		// nothing typed, three times: nobody is logging in -- the call ends
		if (++n->empty_handles >= 3) { bye(n, "Goodbye."); return; }
		ask_handle(n);
		return;
	}
	if (!strcmp(key, "new")) {
		if (!bbs_cfg.new_users) {
			out_mci(n, "|12Sorry, this board is not taking new users.|07\r\n");
			ask_handle(n);
			return;
		}
		out_title(n, "Joining");
		out_mci(n, "  A handle is the name everyone here will know you by.\r\n"
			"  2 to 20 characters: letters, numbers, spaces, - _ . are fine.\r\n\r\n");
		field(n, N_NEW_NAME, "|15Handle|07: ", BBS_HANDLE_MAX, false, NULL);
		return;
	}
	int idx = users_find(n->field);
	if (idx < 0 || !users_read(idx, &n->user)) {
		out_mci(n, bbs_cfg.new_users ? "|12No such user.|07 Type |15NEW|07 to join.\r\n" :
			"|12No such user.|07\r\n");
		if (++n->tries >= 5) { bye(n, "Too many tries. Goodbye."); return; }
		ask_handle(n);
		return;
	}
	field(n, N_LOGIN_PASS, "|15Password|07: ", 64, true, NULL);
}

static void login_pass(node_t *n) {
	bool ok = pw_check(&n->user, n->field);
	memset(n->field, 0, sizeof(n->field));
	if (!ok) {
		bbs_logf("node %d: wrong password for %s%s%s", node_index(n) + 1, n->user.handle,
			n->peer[0] ? " from " : "", n->peer);
		out_mci(n, "|12Wrong password.|07\r\n");
		if (++n->tries >= 3) { bye(n, "Too many tries. Goodbye."); return; }
		ask_handle(n);
		return;
	}
	if (n->user.flags & USER_F_DISABLED) {
		bbs_logf("node %d: %s is disabled", node_index(n) + 1, n->user.handle);
		bye(n, "|12This account is disabled.|07 Goodbye.");
		return;
	}
	logged_in(n);
}

static void new_name(node_t *n) {
	const char *why = handle_problem(n->field);
	if (!why && users_find(n->field) >= 0) why = "somebody already has that handle";
	if (why) {
		out_fmt(n, "|12Sorry: %s.|07\r\n", why);
		field(n, N_NEW_NAME, "|15Handle|07: ", BBS_HANDLE_MAX, false, NULL);
		return;
	}
	memset(&n->user, 0, sizeof(n->user));
	utf8_copy(n->user.handle, sizeof(n->user.handle), n->field);
	field(n, N_NEW_PASS, "|15Password|07 (6 or more characters): ", 64, true, NULL);
}

static void new_pass(node_t *n) {
	if (utf8_chars(n->field) < 6) {
		out_mci(n, "|12At least 6 characters, please.|07\r\n");
		field(n, N_NEW_PASS, "|15Password|07 (6 or more characters): ", 64, true, NULL);
		return;
	}
	snprintf(n->pending_pw, sizeof(n->pending_pw), "%s", n->field);
	memset(n->field, 0, sizeof(n->field));
	field(n, N_NEW_PASS2, "|15The same again|07: ", 64, true, NULL);
}

static void new_pass2(node_t *n) {
	bool same = !strcmp(n->pending_pw, n->field);
	memset(n->field, 0, sizeof(n->field));
	if (!same) {
		memset(n->pending_pw, 0, sizeof(n->pending_pw));
		out_mci(n, "|12Those were not the same.|07\r\n");
		field(n, N_NEW_PASS, "|15Password|07 (6 or more characters): ", 64, true, NULL);
		return;
	}
	field(n, N_NEW_LOC, "|15Where are you|07 (optional): ", 40, false, NULL);
}

static void new_loc(node_t *n) {
	utf8_copy(n->user.location, sizeof(n->user.location), n->field);
	out_nl(n);
	// The caller's own text goes through out_text(), never out_mci():
	// a | in it must not become a colour code.
	out_mci(n, "  Handle: |15");
	out_text(n, n->user.handle);
	out_mci(n, "|07\r\n");
	out_mci(n, "  Where:  ");
	out_text(n, n->user.location[0] ? n->user.location : "-");
	out_nl(n);
	out_nl(n);
	set_state(n, N_NEW_CONFIRM);
	key_prompt(n, "Make this account? |15[Y/n]|07 ");
}

static void new_confirm(node_t *n, uint32_t k) {
	out_nl(n);
	if (!is_yes(k)) {
		memset(n->pending_pw, 0, sizeof(n->pending_pw));
		out_mci(n, "Not made.\r\n");
		ask_handle(n);
		return;
	}
	user_t *u = &n->user;
	// Someone may have taken it while this caller was typing.
	if (users_find(u->handle) >= 0) {
		memset(n->pending_pw, 0, sizeof(n->pending_pw));
		out_mci(n, "|12Somebody took that handle a moment ago.|07\r\n");
		field(n, N_NEW_NAME, "|15Handle|07: ", BBS_HANDLE_MAX, false, NULL);
		return;
	}
	pw_set(u, n->pending_pw, bbs_cfg.pw_iterations);
	memset(n->pending_pw, 0, sizeof(n->pending_pw));
	u->level = (uint8_t)bbs_cfg.new_level;
	u->created = plat_now();
	// The first account is the sysop's: whoever sets a board up joins
	// it first.
	if (users_count() == 0) { u->flags |= USER_F_SYSOP; u->level = LEVEL_SYSOP; }
	if (!users_add(u)) {
		bye(n, "|12The account could not be saved -- please tell the sysop.|07");
		return;
	}
	bbs_logf("node %d: new user %s (#%u)%s", node_index(n) + 1, u->handle, (unsigned)u->id,
		(u->flags & USER_F_SYSOP) ? ", the sysop" : "");
	out_fmt(n, "|10Welcome aboard!|07%s\r\n",
		(u->flags & USER_F_SYSOP) ? " You are the first user, so you are the sysop." : "");
	logged_in(n);
}

// ------------------------------------------------------------------
// the menu
// ------------------------------------------------------------------

void item(node_t *n, char key, const char *what) {
	// The key is in brackets on every terminal; colour only tints it.
	out_fmt(n, "   |07[|15%c|07]|07 %s\r\n", key, what);
}

void show_menu(node_t *n) {
	char when[20];
	set_state(n, N_MENU);
	if (!show_text(n, "menu")) {
		out_title(n, "Main menu");
		out_mci(n, "  Hello, |15");
		out_text(n, n->user.handle);
		fmt_time(n->user.prev_login, when);
		out_fmt(n, "|07. Your last call: %s%s.\r\n", when, n->user.prev_login ? " UTC" : "");
		int mail = mail_new(n);
		if (mail) out_fmt(n, "  |14You have %d new letter%s.|07\r\n", mail, mail == 1 ? "" : "s");
		out_nl(n);
		item(n, 'N', "New messages, everywhere");
		item(n, 'F', "Forums");
		item(n, 'M', "Mail");
		item(n, 'B', "Bulletins");
		item(n, 'W', "Who is online");
		item(n, 'U', "Users");
		item(n, 'P', "Profile and terminal");
		if (n->user.flags & USER_F_SYSOP) item(n, 'S', "Sysop");
		item(n, 'G', "Goodbye");
		out_nl(n);
	}
	key_prompt(n, "|15Your choice|07: ");
}

static void menu_key(node_t *n, uint32_t k) {
	switch (upper(k)) {
	case 'N':
		newscan_start(n);
		return;
	case 'F':
		forums_show(n);
		return;
	case 'M':
		area_menu(n, 0);
		return;
	case 'B':
		out_nl(n);
		n->pg_newer = 0;
		n->pg_count = 0;
		if (!bulletin_list()) { out_mci(n, "There are no bulletins.\r\n"); key_prompt(n, "|15Your choice|07: "); return; }
		pager_start(n, PG_BULLETINS, N_ANYKEY);
		return;
	case 'W':
		out_title(n, "Who is online");
		out_mci(n, "|03 Node Handle                 Via     Doing                  On for|07\r\n");
		pager_start(n, PG_WHO, N_ANYKEY);
		return;
	case 'U': {
		out_title(n, "Users");
		out_mci(n, "|03    #  Handle                Location                  Last call (UTC)|07\r\n");
		n->pg_kind = PG_USERS;
		int total = users_count();
		n->pg_then = N_ANYKEY;
		n->pg_off = 0;
		n->pg_line = 2;
		n->pg_count = total < 0 ? 0 : total;
		set_state(n, N_PAGER);
		pager_run(n);
		return;
	}
	case 'P':
		profile_show(n);
		return;
	case 'S':
		if (n->user.flags & USER_F_SYSOP) sysop_show(n);
		return;
	case 'G': case 'Q':
		out_nl(n);
		out_fmt(n, "\r\nGoodbye, |15");
		out_text(n, n->user.handle);
		out_fmt(n, "|07. Thanks for calling |15%s|07.\r\n", bbs_cfg.name);
		bye(n, NULL);
		return;
	case K_ENTER:
		show_menu(n);
		return;
	}
}

// ------------------------------------------------------------------
// the profile: the caller's own settings
// ------------------------------------------------------------------

static const char *cs_name(int c) {
	return c == CS_UTF8 ? "UTF-8" : c == CS_CP437 ? "CP437" : c == CS_ASCII ? "ASCII" : "detect";
}

static void profile_show(node_t *n) {
	user_t *u = &n->user;
	set_state(n, N_PROFILE);
	out_title(n, "Profile and terminal");
	out_mci(n, "  Handle     |15"); out_text(n, u->handle); out_mci(n, "|07\r\n");
	out_mci(n, "  Location   "); out_text(n, u->location[0] ? u->location : "-"); out_nl(n);
	out_fmt(n, "  Calls      %u\r\n\r\n", (unsigned)u->calls);
	out_fmt(n, "  Characters %s -- now %s%s\r\n", cs_name(u->charset), cs_name(n->charset),
		n->ansi ? ", ANSI" : ", no ANSI");
	out_fmt(n, "  Colour     %s -- now %s\r\n", u->color == COLOR_ON ? "on" : u->color == COLOR_OFF ?
		"off" : "detect", n->color ? "on" : "off");
	out_fmt(n, "  Rows       %s%d -- now %d\r\n", u->rows ? "" : "detect, ", u->rows, n->rows);
	out_fmt(n, "  Editor     %s%s\r\n\r\n", (u->flags & USER_F_LINE_EDITOR) ? "a line at a time" : "full-screen",
		n->ansi ? "" : " -- a line at a time here: no ANSI");
	out_mci(n, "  Box test:  \xE2\x94\x8C\xE2\x94\x80\xE2\x94\x80\xE2\x94\x90 \xE2\x95\x94\xE2\x95\x90\xE2\x95\x90\xE2\x95\x97 "
		"\xE2\x96\x91\xE2\x96\x92\xE2\x96\x93\xE2\x96\x88  |07(should be two boxes and four shades)|07\r\n\r\n");
	item(n, 'L', "Change location");
	item(n, 'W', "Change password");
	item(n, 'C', "Characters: detect, UTF-8, CP437, ASCII");
	item(n, 'O', "Colour: detect, on, off");
	item(n, 'R', "Rows on the screen");
	item(n, 'E', "Editor: full-screen, or a line at a time");
	item(n, 'Q', "Back to the menu");
	out_nl(n);
	key_prompt(n, "|15Your choice|07: ");
}

static void profile_key(node_t *n, uint32_t k) {
	user_t *u = &n->user;
	switch (upper(k)) {
	case 'L':
		out_nl(n);
		field(n, N_PROF_LOC, "|15Where are you|07: ", 40, false, u->location);
		return;
	case 'W':
		out_nl(n);
		field(n, N_PROF_PASS_OLD, "|15Your password now|07: ", 64, true, NULL);
		return;
	case 'C':
		u->charset = (uint8_t)((u->charset + 1) % 4);
		users_write(u);
		// back to what detection found, then this user's choice
		apply_prefs(n);
		if (u->charset == CS_AUTO) out_mci(n, "\r\n(detected at the next call)\r\n");
		profile_show(n);
		return;
	case 'O':
		u->color = (uint8_t)((u->color + 1) % 3);
		users_write(u);
		n->color = u->color == COLOR_OFF ? false : n->ansi;
		profile_show(n);
		return;
	case 'R':
		out_nl(n);
		field(n, N_PROF_ROWS, "|15Rows|07 (10-100, 0 to detect): ", 3, false, NULL);
		return;
	case 'E':
		u->flags ^= USER_F_LINE_EDITOR;
		users_write(u);
		profile_show(n);
		return;
	case 'Q': case K_ESC:
		show_menu(n);
		return;
	}
}

// ------------------------------------------------------------------
// the sysop
// ------------------------------------------------------------------

static void sysop_show(node_t *n) {
	int busy = 0;
	for (int i = 0; i < n_nodes; i++) busy += bbs_node[i]->state != N_FREE;
	set_state(n, N_SYSOP);
	out_title(n, "Sysop");
	out_fmt(n, "  %d users. %d of %d nodes in use.\r\n\r\n", users_count(), busy, n_nodes);
	item(n, 'U', "A user: level, password, disable");
	item(n, 'K', "Disconnect a node");
	item(n, 'Q', "Back to the menu");
	out_nl(n);
	key_prompt(n, "|15Your choice|07: ");
}

static void sys_user_show(node_t *n) {
	user_t *u = &n->edit;
	char when[20];
	set_state(n, N_SYS_USER);
	out_title(n, "A user");
	out_fmt(n, "  #%u  |15", (unsigned)u->id); out_text(n, u->handle); out_mci(n, "|07\r\n");
	out_mci(n, "  Location  "); out_text(n, u->location[0] ? u->location : "-"); out_nl(n);
	out_fmt(n, "  Level     %u%s\r\n", u->level, (u->flags & USER_F_SYSOP) ? " (sysop)" : "");
	out_fmt(n, "  Status    %s\r\n", (u->flags & USER_F_DISABLED) ? "|12disabled|07" : "active");
	fmt_time(u->created, when);  out_fmt(n, "  Joined    %s\r\n", when);
	fmt_time(u->last_login, when); out_fmt(n, "  Last call %s, %u calls\r\n\r\n", when, (unsigned)u->calls);
	item(n, 'L', "Level (255 is sysop)");
	item(n, 'P', "Set a new password");
	item(n, 'D', (u->flags & USER_F_DISABLED) ? "Enable" : "Disable");
	item(n, 'Q', "Back");
	out_nl(n);
	key_prompt(n, "|15Your choice|07: ");
}

static void sysop_key(node_t *n, uint32_t k) {
	switch (upper(k)) {
	case 'U': out_nl(n); field(n, N_SYS_PICK, "|15Handle|07: ", BBS_HANDLE_MAX, false, NULL); return;
	case 'K': out_nl(n); field(n, N_SYS_KICK, "|15Node|07: ", 2, false, NULL); return;
	case 'Q': case K_ESC: show_menu(n); return;
	}
}

// Someone else's record may have changed since it was read (they logged
// in, changed their location): the edit applies to what is there NOW.
static bool sys_reload(node_t *n) {
	user_t fresh;
	if (!users_read((int)n->edit.id - 1, &fresh)) return false;
	n->edit = fresh;
	return true;
}

static void sys_user_key(node_t *n, uint32_t k) {
	user_t *u = &n->edit;
	switch (upper(k)) {
	case 'L': out_nl(n); field(n, N_SYS_LEVEL, "|15Level|07 (0-255): ", 3, false, NULL); return;
	case 'P': out_nl(n); field(n, N_SYS_PASS, "|15New password|07: ", 64, true, NULL); return;
	case 'D':
		if (u->id == n->user.id) { out_mci(n, "\r\n|12Not yourself.|07\r\n"); key_prompt(n, "|15Your choice|07: "); return; }
		if (sys_reload(n)) {
			u->flags ^= USER_F_DISABLED;
			users_write(u);
			bbs_logf("sysop %s: %s %s", n->user.handle, (u->flags & USER_F_DISABLED) ? "disabled" : "enabled", u->handle);
		}
		sys_user_show(n);
		return;
	case 'Q': case K_ESC: sysop_show(n); return;
	}
}

// ------------------------------------------------------------------
// dispatch
// ------------------------------------------------------------------

static void on_field(node_t *n) {
	switch (n->state) {
	case N_LOGIN_NAME: login_name(n); return;
	case N_LOGIN_PASS: login_pass(n); return;
	case N_NEW_NAME: new_name(n); return;
	case N_NEW_PASS: new_pass(n); return;
	case N_NEW_PASS2: new_pass2(n); return;
	case N_NEW_LOC: new_loc(n); return;
	case N_PROF_LOC:
		utf8_copy(n->user.location, sizeof(n->user.location), n->field);
		users_write(&n->user);
		profile_show(n);
		return;
	case N_PROF_PASS_OLD:
		if (!pw_check(&n->user, n->field)) {
			memset(n->field, 0, sizeof(n->field));
			out_mci(n, "|12That is not your password.|07\r\n");
			anykey(n, N_PROFILE);
			return;
		}
		memset(n->field, 0, sizeof(n->field));
		field(n, N_PROF_PASS_NEW, "|15New password|07 (6 or more): ", 64, true, NULL);
		return;
	case N_PROF_PASS_NEW:
		if (utf8_chars(n->field) < 6) {
			out_mci(n, "|12At least 6 characters.|07\r\n");
			field(n, N_PROF_PASS_NEW, "|15New password|07 (6 or more): ", 64, true, NULL);
			return;
		}
		snprintf(n->pending_pw, sizeof(n->pending_pw), "%s", n->field);
		memset(n->field, 0, sizeof(n->field));
		field(n, N_PROF_PASS_NEW2, "|15The same again|07: ", 64, true, NULL);
		return;
	case N_PROF_PASS_NEW2:
		if (strcmp(n->pending_pw, n->field)) out_mci(n, "|12Those were not the same -- unchanged.|07\r\n");
		else {
			pw_set(&n->user, n->pending_pw, bbs_cfg.pw_iterations);
			users_write(&n->user);
			out_mci(n, "|10Password changed.|07\r\n");
			bbs_logf("node %d: %s changed their password", node_index(n) + 1, n->user.handle);
		}
		memset(n->pending_pw, 0, sizeof(n->pending_pw));
		memset(n->field, 0, sizeof(n->field));
		anykey(n, N_PROFILE);
		return;
	case N_PROF_ROWS: {
		int r = atoi(n->field);
		if (r == 0 || (r >= 10 && r <= 100)) {
			n->user.rows = (uint8_t)r;
			users_write(&n->user);
			if (r) n->rows = r;
		}
		profile_show(n);
		return;
	}
	case N_SYS_PICK: {
		int idx = users_find(n->field);
		if (idx < 0 || !users_read(idx, &n->edit)) {
			out_mci(n, "|12No such user.|07\r\n");
			sysop_show(n);
			return;
		}
		sys_user_show(n);
		return;
	}
	case N_SYS_LEVEL: {
		int lv = atoi(n->field);
		if (n->field[0] && lv >= 0 && lv <= 255 && sys_reload(n)) {
			if (n->edit.id == n->user.id && lv != LEVEL_SYSOP)
				out_mci(n, "|12Not your own sysop level -- ask another sysop.|07\r\n");
			else {
				n->edit.level = (uint8_t)lv;
				if (lv == LEVEL_SYSOP) n->edit.flags |= USER_F_SYSOP;
				else n->edit.flags &= (uint16_t)~USER_F_SYSOP;
				users_write(&n->edit);
				bbs_logf("sysop %s: %s is level %d", n->user.handle, n->edit.handle, lv);
			}
		}
		sys_user_show(n);
		return;
	}
	case N_SYS_PASS:
		if (utf8_chars(n->field) >= 6 && sys_reload(n)) {
			pw_set(&n->edit, n->field, bbs_cfg.pw_iterations);
			users_write(&n->edit);
			bbs_logf("sysop %s: set a new password for %s", n->user.handle, n->edit.handle);
		} else out_mci(n, "|12At least 6 characters -- unchanged.|07\r\n");
		memset(n->field, 0, sizeof(n->field));
		sys_user_show(n);
		return;
	case N_SYS_KICK: {
		int k = atoi(n->field) - 1;
		if (k >= 0 && k < n_nodes && bbs_node[k] != n && bbs_node[k]->state != N_FREE) {
			bbs_logf("sysop %s: disconnected node %d", n->user.handle, k + 1);
			bye(bbs_node[k], "|12The sysop has ended this call.|07");
		} else out_mci(n, "|12Not a node that can be disconnected.|07\r\n");
		sysop_show(n);
		return;
	}
	default:
		reader_field(n);
		return;
	}
}

static void on_key(node_t *n, uint32_t k) {
	if (n->field_on) { field_key(n, k); return; }
	switch (n->state) {
	case N_MENU: menu_key(n, k); return;
	case N_PAGER: pager_key(n, k); return;
	case N_ANYKEY:
		if (n->after_key == N_PROFILE) profile_show(n);
		else if (n->after_key == N_SYSOP) sysop_show(n);
		else if (n->after_key == N_READ) reader_after_body(n);
		else if (n->after_key == N_AREA_MENU) area_menu(n, n->area);
		else show_menu(n);
		return;
	case N_NEW_CONFIRM: new_confirm(n, k); return;
	case N_PROFILE: profile_key(n, k); return;
	case N_SYSOP: sysop_key(n, k); return;
	case N_SYS_USER: sys_user_key(n, k); return;
	default: reader_keyed(n, k); return;
	}
}

// ------------------------------------------------------------------
// time
// ------------------------------------------------------------------

void bbs_poll(void) {
	uint32_t now = plat_ms();
	for (int i = 0; i < n_nodes; i++) {
		node_t *n = bbs_node[i];
		if (n->state == N_FREE || n->state == N_BYE) continue;
		uint32_t in_state = now - n->since_ms;
		if (n->state == N_ZETTA) zed_tick(n);		// a lone Esc; input held after an action
		if (n->in_esc_n == 1 && now - n->esc_ms >= ESC_MS) {
			n->in_esc_n = 0;
			on_key(n, K_ESC);
		}
		if (n->state == N_DETECT) {
			if (n->detect_step == 0 && in_state >= DETECT_MS) detect_done(n);
			else if (n->detect_step == 1 && in_state >= DETECT2_MS) detect_done(n);
			continue;
		}
		if (!n->logged_in) {
			if (in_state >= (uint32_t)bbs_cfg.login_seconds * 1000u && now - n->last_input_ms >= 20000u)
				bye(n, "Too slow -- goodbye.");
			else if (now - n->connected_ms >= (uint32_t)bbs_cfg.login_seconds * 1000u * 3u)
				bye(n, "Too slow -- goodbye.");
			continue;
		}
		if (now - n->last_input_ms >= (uint32_t)bbs_cfg.idle_minutes * 60000u)
			bye(n, "|12Nothing typed for a while -- goodbye.|07");
	}
}
