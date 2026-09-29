/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- reading and writing messages: the forum list, an area's menu,
 * the reader, the new-scan, and the editor. docs/bbs.md, "Messages".
 *
 * The mail is area 0 and goes through all of it too; what makes it mail
 * is that a caller only sees the messages addressed to them, and that
 * "read" is a flag on the message rather than a last-read number.
 *
 * The editor is a line editor, so it works on every terminal, plain
 * ASCII included: a numbered prompt per line, words wrapped as they are
 * typed, and commands on a line of their own that start with /.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbs_int.h"
#include "../../../common/zetta.h"

#define ED_LINE_CHARS 76
#define ED_LINES_MAX  200
#define QUOTE_LINES   40

static void editor_prompt(node_t *n, const char *carry);
static void zed_start(node_t *n, bool reply);
static void editor_save(node_t *n);

// A letter's To: a handle here, or handle@node on the network. NULL if
// it will do -- `norm` then the address as it is to be kept (the user's
// own spelling; @this-node dropped) -- else why not.
static const char *mail_to_check(const char *v, char *norm, size_t cap) {
	char handle[USER_HANDLE_BYTES];
	user_t u;
	if (!v[0]) return "Who to? A handle, or handle@node.";
	const char *at = strchr(v, '@');
	if (at) {
		const char *node = at + 1;
		if (at == v || !*node || strchr(node, '@') || strchr(v, ' ')) return "An address is handle@node.";
		snprintf(handle, sizeof(handle), "%.*s", (int)(at - v), v);
		if (utf8_chars(handle) > BBS_HANDLE_MAX) return "That handle is too long to be one.";
		if (!fed_node_name()[0] || strcmp(node, fed_node_name())) {
			if (!bbs_fed_network[0]) return "Mail to other nodes needs this BBS on a network.";
			if (!fed_node_known(node)) return "No node by that name on the network.";
			if (norm) snprintf(norm, cap, "%s", v);
			return NULL;
		}
		v = handle;							// @this node: a letter here
	}
	int idx = users_find(v);
	if (idx < 0 || !users_read(idx, &u) || (u.flags & USER_F_DISABLED)) return "No such user.";
	if (norm) snprintf(norm, cap, "%s", u.handle);
	return NULL;
}

// Writing full-screen (zetta): with ANSI, unless the caller chose a line
// at a time in their profile.
static bool full_screen(const node_t *n) {
	return n->ansi && !(n->user.flags & USER_F_LINE_EDITOR);
}
static void post_subject(node_t *n);

static bool can_read(node_t *n, int a) {
	return a == 0 ? n->user.level >= 1 : n->user.level >= bbs_area[a].rlevel;
}

static bool can_write(node_t *n, int a) {
	return n->user.level >= bbs_area[a].wlevel;
}

static bool is_sysop(node_t *n) {
	return (n->user.flags & USER_F_SYSOP) != 0;
}

// Is message `e` one this caller sees here?
static bool visible(node_t *n, int a, const idx_t *e) {
	if (e->flags & IDX_DELETED) return false;
	if (a != 0) return true;
	if (e->to_id != n->user.id) return false;
	return !(n->scan && (e->flags & IDX_READ));       // a new-scan: unread mail only
}

// The next message this caller sees, from `from` on (dir +1) or back
// (dir -1); 0 if there is none.
static int find_visible(node_t *n, int a, int from, int dir) {
	idx_t e[16];
	if (dir > 0) {
		for (int i = from < 1 ? 1 : from; i <= msg_count(a); ) {
			int k = msg_idx_many(a, i, e, 16);
			if (k <= 0) return 0;
			for (int j = 0; j < k; j++) if (visible(n, a, &e[j])) return i + j;
			i += k;
		}
		return 0;
	}
	for (int i = from > msg_count(a) ? msg_count(a) : from; i >= 1; i--)
		if (msg_idx(a, i, &e[0]) && visible(n, a, &e[0])) return i;
	return 0;
}

int mail_new(node_t *n) {
	return area_new(n, 0);
}

// ------------------------------------------------------------------
// the forum list
// ------------------------------------------------------------------

// Forums are numbered on screen as the caller sees them: those they
// may read, in forums.cfg's order.
static int forum_by_number(node_t *n, int want) {
	int k = 0;
	for (int a = 1; a < bbs_nareas; a++)
		if (can_read(n, a) && ++k == want) return a;
	return -1;
}

void forums_show(node_t *n) {
	char line[400], name[24 * 4 + 1];
	out_title(n, "Forums");
	// no dark grey (|07) for what must be read: in many terminal colour
	// schemes it IS the background -- the header vanished, and a long
	// description's wrapped half looked like an empty line
	out_mci(n, "|03  #  Forum                     New  All  |07\r\n");
	int k = 0;
	for (int a = 1; a < bbs_nareas; a++) {
		if (!can_read(n, a)) continue;
		utf8_pad(name, sizeof(name), bbs_area[a].name, 24);
		int nw = area_new(n, a);
		snprintf(line, sizeof(line), " %2d  %s %4d %4d  ", ++k, name, nw, msg_count(a));
		if (nw) out_fg(n, 15);
		out_text(n, line);
		out_fg(n, 7);
		// the description cut to the line -- never wrapping, and never
		// into the last column, which some terminals wrap on as well
		int room = (n->cols > 1 ? n->cols : 80) - 1 - 41;
		if (room >= 8) {
			char d[sizeof(bbs_area[0].desc) * 2 + 8];
			utf8_pad(d, sizeof(d), bbs_area[a].desc, room);
			out_text(n, d);
		}
		out_nl(n);
	}
	if (!k) out_mci(n, "  There are no forums you can read.\r\n");
	out_nl(n);
	field(n, N_AREAS, "|15Forum number|07 (Enter: back): ", 3, false, NULL);
}

// ------------------------------------------------------------------
// an area's menu
// ------------------------------------------------------------------

void area_menu(node_t *n, int a) {
	char s[160];
	n->area = a;
	n->scan = false;
	n->msgnum = 0;
	set_state(n, N_AREA_MENU);
	out_title(n, bbs_area[a].name);
	int nw = area_new(n, a);
	if (a == 0) snprintf(s, sizeof(s), "  %d new for you.\r\n\r\n", nw);
	else snprintf(s, sizeof(s), "  %d messages, %d new for you.\r\n\r\n", msg_count(a), nw);
	out_text(n, s);
	item(n, 'R', a == 0 ? "Read your mail" : "Read the new ones");
	if (a != 0) item(n, 'A', "Read from the start");
	if (can_write(n, a)) item(n, 'W', a == 0 ? "Write a letter" : "Write a message");
	item(n, 'Q', "Back");
	out_nl(n);
	out_mci(n, "|15Your choice|07: ");
}

static void read_from(node_t *n, int a, int from);

static bool area_menu_key(node_t *n, uint32_t k) {
	int a = n->area;
	switch (upper(k)) {
	case 'R':
		if (a == 0) read_from(n, 0, 1);
		else read_from(n, a, (int)n->lastread[a] + 1);
		return true;
	case 'A':
		if (a != 0) read_from(n, a, 1);
		return true;
	case 'W':
		if (!can_write(n, a)) return true;
		memset(&n->post, 0, sizeof(n->post));
		if (full_screen(n)) { zed_start(n, false); return true; }
		out_nl(n);
		if (a == 0) field(n, N_POST_TO, "|15To|07 (a handle, or handle@node): ", 50, false, NULL);
		else post_subject(n);
		return true;
	case 'Q': case K_ESC:
		if (a == 0) show_menu(n);
		else forums_show(n);
		return true;
	}
	return true;
}

// ------------------------------------------------------------------
// the reader
// ------------------------------------------------------------------

static void show_msg(node_t *n, int num) {
	msg_t m;
	idx_t e;
	char when[20], line[300];
	int a = n->area;
	if (!msg_head(a, num, &m, &e)) {
		out_mci(n, "|12That message cannot be read.|07\r\n");
		anykey(n, N_MENU);
		return;
	}
	n->msgnum = num;
	snprintf(line, sizeof(line), "%s  #%d of %d", bbs_area[a].name, num, msg_count(a));
	out_title(n, line);
	fmt_time(m.date, when);
	out_mci(n, "  |03From|07     |15"); out_text(n, m.from); out_mci(n, "|07\r\n");
	if (m.to[0]) { out_mci(n, "  |03To|07       "); out_text(n, m.to); out_nl(n); }
	out_mci(n, "  |03Subject|07  |15"); out_text(n, m.subject); out_mci(n, "|07\r\n");
	snprintf(line, sizeof(line), "  %s UTC", when);
	out_mci(n, "  |03Date|07   "); out_text(n, line);
	if (m.reply) { snprintf(line, sizeof(line), "   (a reply to #%u)", (unsigned)m.reply); out_fg(n, 7); out_text(n, line); out_fg(n, 7); }
	out_nl(n);
	out_rule(n);

	// Read, as far as this caller goes.
	if (a == 0) {
		if (e.to_id == n->user.id && !(e.flags & IDX_READ)) { e.flags |= IDX_READ; msg_idx_write(a, num, &e); }
	} else if ((uint32_t)num > n->lastread[a]) {
		n->lastread[a] = (uint32_t)num;
		n->lastread_dirty = true;
	}

	// the body, a page at a time; then the reader's prompt
	if (!msg_log_path(a, n->pg_path)) { reader_after_body(n); return; }
	n->pg_kind = PG_MSG;
	n->pg_then = N_READ;
	n->pg_off = m.body_off;
	n->pg_end = m.body_off + m.body_len;
	n->pg_line = 7;
	set_state(n, N_PAGER);
	pager_run(n);
}

void reader_after_body(node_t *n) {
	set_state(n, N_READ);
	out_nl(n);
	out_mci(n, "|07[|15N|07]|07ext |07[|15P|07]|07rev");
	if (can_write(n, n->area)) out_mci(n, " |07[|15R|07]|07eply");
	out_mci(n, " |07[|15D|07]|07elete |07[|15Q|07]|07uit: ");
}

static void scan_next(node_t *n, int after);

static void read_from(node_t *n, int a, int from) {
	n->area = a;
	int num = find_visible(n, a, from, +1);
	if (num) { show_msg(n, num); return; }
	if (n->scan) { scan_next(n, a); return; }
	out_mci(n, "\r\nNothing new here.");
	if (a != 0 && msg_count(a)) out_mci(n, " |15A|07 reads from the start.");
	out_nl(n);
	out_mci(n, "|15Your choice|07: ");
	set_state(n, N_AREA_MENU);
}

static bool may_delete(node_t *n, const idx_t *e) {
	return is_sysop(n) || e->from_id == n->user.id || (n->area == 0 && e->to_id == n->user.id);
}

static void post_reply(node_t *n);

void reader_key(node_t *n, uint32_t k) {
	int a = n->area, num;
	idx_t e;
	switch (upper(k)) {
	case 'N': case K_ENTER: case ' ':
		num = find_visible(n, a, n->msgnum + 1, +1);
		if (num) { show_msg(n, num); return; }
		if (n->scan) { scan_next(n, a); return; }
		out_mci(n, "\r\n|07No more messages here.|07\r\n");
		anykey(n, N_AREA_MENU);
		return;
	case 'P':
		num = find_visible(n, a, n->msgnum - 1, -1);
		if (num) { show_msg(n, num); return; }
		out_mci(n, "\r\n|07That was the first.|07");
		reader_after_body(n);
		return;
	case 'R':
		if (can_write(n, a)) post_reply(n);
		return;
	case 'D':
		if (!msg_idx(a, n->msgnum, &e) || !may_delete(n, &e)) {
			out_mci(n, "\r\n|12Only its writer (or the sysop) can delete it.|07");
			reader_after_body(n);
			return;
		}
		e.flags |= IDX_DELETED;
		msg_idx_write(a, n->msgnum, &e);
		bbs_logf("node %d: %s deleted %s #%d", node_index(n) + 1, n->user.handle, bbs_area[a].tag, n->msgnum);
		{
			// federated: a cancel too -- honoured by other nodes where this
			// caller wrote it, or is a moderator of the forum (fed.md, "Moderation")
			msg_t m;
			if (bbs_area[a].topic[0] && msg_head(a, n->msgnum, &m, NULL) && strlen(m.id) == 64 && fed_cancel(a, m.id) >= 0) {
				out_mci(n, "\r\n|10Deleted here, and a cancel sent to the network|07 |07(honoured where you wrote it, or moderate the forum)|07\r\n");
				reader_key(n, 'N');
				return;
			}
		}
		out_mci(n, "\r\n|10Deleted.|07\r\n");
		reader_key(n, 'N');
		return;
	case 'Q': case K_ESC:
		lastread_save(n);
		if (n->scan) { n->scan = false; show_menu(n); return; }
		area_menu(n, a);
		return;
	}
}

// ------------------------------------------------------------------
// the new-scan: the mail, then every forum with something new
// ------------------------------------------------------------------

static void scan_next(node_t *n, int after) {
	lastread_save(n);
	for (int a = after + 1; a < bbs_nareas; a++) {
		if (!can_read(n, a) || area_new(n, a) == 0) continue;
		n->area = a;
		read_from(n, a, a == 0 ? 1 : (int)n->lastread[a] + 1);
		return;
	}
	n->scan = false;
	out_mci(n, "\r\n|10That is everything new.|07\r\n");
	anykey(n, N_MENU);
}

void newscan_start(node_t *n) {
	n->scan = true;
	scan_next(n, -1);
}

// ------------------------------------------------------------------
// writing
// ------------------------------------------------------------------

static void post_subject(node_t *n) {
	field(n, N_POST_SUBJ, "|15Subject|07: ", SUBJECT_MAX, false, n->post.subject);
}

static void post_reply(node_t *n) {
	msg_t m;
	if (!msg_head(n->area, n->msgnum, &m, NULL)) return;
	memset(&n->post, 0, sizeof(n->post));
	n->post.reply = (uint32_t)n->msgnum;
	utf8_copy(n->post.reply_id, sizeof(n->post.reply_id), m.id);
	if (n->area == 0) {
		utf8_copy(n->post.to, sizeof(n->post.to), m.from);
		n->post.to_id = m.from_id;
	}
	if (strncmp(m.subject, "Re: ", 4)) {
		char s[sizeof(m.subject) + 4];
		snprintf(s, sizeof(s), "Re: %s", m.subject);
		utf8_copy(n->post.subject, sizeof(n->post.subject), s);
	} else utf8_copy(n->post.subject, sizeof(n->post.subject), m.subject);
	if (utf8_chars(n->post.subject) > SUBJECT_MAX) {
		// cut to SUBJECT_MAX characters, at a character boundary
		const char *p = n->post.subject, *end = p + strlen(p);
		for (int i = 0; i < SUBJECT_MAX; i++) utf8_next(&p, end);
		n->post.subject[p - n->post.subject] = 0;
	}
	if (full_screen(n)) { zed_start(n, true); return; }	// with the quote window
	out_nl(n);
	out_nl(n);
	post_subject(n);
}

static const char *ed_help =
	"|07Write a line and press Enter; words wrap by themselves.\r\n"
	"On a line of its own: |15/s|07 saves  |15/a|07 abandons  |15/l|07 lists  "
	"|15/d 3|07 deletes line 3  |15/?|07 this.|07\r\n";

static bool ed_add(node_t *n, const char *line) {
	size_t l = strlen(line);
	if (n->ed_lines >= ED_LINES_MAX || n->ed_len + l + 1 >= MSG_BODY_MAX) return false;
	memcpy(n->ed + n->ed_len, line, l);
	n->ed_len += (uint32_t)l;
	n->ed[n->ed_len++] = '\n';
	n->ed[n->ed_len] = 0;
	n->ed_lines++;
	return true;
}

// Adds `text` as quoted lines, each "> " and at most ED_LINE_CHARS.
static void ed_quote(node_t *n, const char *from, const char *text, uint32_t len) {
	char line[FIELD_MAX], hdr[USER_HANDLE_BYTES + 16];
	snprintf(hdr, sizeof(hdr), "%s wrote:", from);
	ed_add(n, hdr);
	const char *p = text, *end = text + len;
	int lines = 0;
	while (p < end && lines < QUOTE_LINES) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol) eol = end;
		// "> " or ">" before text already quoted (">> ")
		int o = snprintf(line, sizeof(line), p < eol && *p == '>' ? ">" : "> ");
		const char *q = p;
		int chars = o;
		while (q < eol && chars < ED_LINE_CHARS) {
			const char *c = q;
			utf8_next(&q, eol);
			if ((size_t)(o + (q - c)) >= sizeof(line)) break;
			memcpy(line + o, c, (size_t)(q - c));
			o += (int)(q - c);
			chars++;
		}
		line[o] = 0;
		if (!ed_add(n, line)) break;
		lines++;
		p = eol + 1;
	}
	ed_add(n, "");
}

static void ed_list(node_t *n) {
	char num[16];
	const char *p = n->ed;
	for (int i = 1; i <= n->ed_lines; i++) {
		const char *eol = strchr(p, '\n');
		if (!eol) break;
		snprintf(num, sizeof(num), "%3d> ", i);
		out_fg(n, 7); out_text(n, num); out_fg(n, 7);
		out_textn(n, p, (uint32_t)(eol - p));
		out_nl(n);
		p = eol + 1;
	}
}

static bool ed_delete(node_t *n, int which) {
	char *p = n->ed;
	if (which < 1 || which > n->ed_lines) return false;
	for (int i = 1; i < which; i++) p = strchr(p, '\n') + 1;
	char *eol = strchr(p, '\n') + 1;
	memmove(p, eol, (size_t)(n->ed + n->ed_len - eol) + 1);
	n->ed_len -= (uint32_t)(eol - p);
	n->ed_lines--;
	return true;
}

void editor_free(node_t *n) {
	free(n->zed);
	n->zed = NULL;
	free(n->ed);
	n->ed = NULL;
	n->ed_len = 0;
	n->ed_lines = 0;
}

// ------------------------------------------------------------------
// writing full-screen: zetta (docs/zetta.md), for callers with ANSI
// ------------------------------------------------------------------

#define ZED_ORIG     3072			// the message replied to, for the quote window
#define ZED_LINES    128

typedef struct {
	zetta_t z;
	node_t *n;
	bool mail;
	char clip[1024];
	char orig[ZED_ORIG];
	uint16_t line_at[ZED_LINES + 1];
	int nlines;
	char line[FIELD_MAX];
} zed_t;

enum { ZA_POST = 1, ZA_QUOTE = 2 };
enum { ZT_DISCARD = 1, ZT_QUOTE = 2 };

// zetta's output: escape sequences as they are, text in the caller's
// charset (out_textn() would drop the escapes' ESC as a control).
static void zed_write(void *ctx, const char *b, uint32_t len) {
	node_t *n = ctx;
	uint32_t i = 0;
	while (i < len) {
		uint32_t j = i;
		if (b[i] == 0x1b) {
			j = i + 1;
			if (j < len && b[j] == '[') {
				j++;
				while (j < len && !(b[j] >= 0x40 && b[j] <= 0x7e)) j++;
				if (j < len) j++;
			}
			out_raw(n, b + i, j - i);
		} else {
			while (j < len && b[j] != 0x1b) j++;
			out_textn(n, b + i, j - i);
		}
		i = j;
	}
}

static const char *zed_check_to(const char *v, void *ctx) {
	(void)ctx;
	return mail_to_check(v, NULL, 0);
}

// The quote window's line i: "> " before it, or ">" before text already
// quoted -- as the line editor quotes (ed_quote()).
static const char *zed_line(void *ctx, int i) {
	zed_t *zd = ctx;
	if (i < 0 || i >= zd->nlines) return NULL;
	uint32_t a = zd->line_at[i], e = zd->line_at[i + 1];
	if (e > a && zd->orig[e - 1] == '\n') e--;
	uint32_t n = e - a;
	if (n >= sizeof(zd->line)) n = sizeof(zd->line) - 1;
	memcpy(zd->line, zd->orig + a, n);
	zd->line[n] = 0;
	return zd->line;
}

static void zed_end(node_t *n) {
	free(n->zed);
	n->zed = NULL;
	out_cls(n);
}

static void zed_start(node_t *n, bool reply) {
	editor_free(n);
	zed_t *zd = malloc(sizeof(zed_t));
	n->ed = malloc(MSG_BODY_MAX);
	if (!zd || !n->ed) {
		free(zd);
		editor_free(n);
		out_mci(n, "|12Not enough memory to write just now -- try again in a moment.|07\r\n");
		bbs_logf("node %d: no memory for the editor", node_index(n) + 1);
		anykey(n, N_MENU);
		return;
	}
	memset(zd, 0, sizeof(*zd));
	n->zed = zd;
	n->ed[0] = 0;
	zd->n = n;
	zd->mail = n->area == 0;
	int a = n->area;

	// replying: the original's lines, for the quote window
	if (reply) {
		msg_t m;
		if (msg_head(a, (int)n->post.reply, &m, NULL)) {
			uint32_t bl = msg_body(a, &m, zd->orig, sizeof(zd->orig));
			uint32_t p = 0;
			while (p < bl && zd->nlines < ZED_LINES) {
				zd->line_at[zd->nlines++] = (uint16_t)p;
				while (p < bl && zd->orig[p] != '\n') p++;
				if (p < bl) p++;
			}
			zd->line_at[zd->nlines] = (uint16_t)p;
		}
	}

	static char title[120];
	snprintf(title, sizeof(title), "%s -- %s%s", bbs_cfg.name, zd->mail ? "a letter" : bbs_area[a].name,
		reply ? " (a reply)" : "");
	zetta_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.rows = n->rows > 0 ? n->rows : 25;
	c.cols = n->cols > 0 && n->cols < 80 ? n->cols : 80;
	c.title = title;
	if (zd->mail) {
		c.fields[c.nfields].label = "To";
		c.fields[c.nfields].buf = n->post.to;
		c.fields[c.nfields].cap = sizeof(n->post.to);
		c.fields[c.nfields].check = zed_check_to;
		c.nfields++;
	}
	c.fields[c.nfields].label = "Subject";
	c.fields[c.nfields].buf = n->post.subject;
	c.fields[c.nfields].cap = sizeof(n->post.subject);
	c.nfields++;
	c.actions[c.nactions].key = ZK_CTRL('S');
	c.actions[c.nactions].label = zd->mail ? "Send" : "Post";
	c.actions[c.nactions].id = ZA_POST;
	c.nactions++;
	if (zd->nlines) {
		c.actions[c.nactions].key = ZK_CTRL('Q');
		c.actions[c.nactions].label = "Quote";
		c.actions[c.nactions].id = ZA_QUOTE;
		c.nactions++;
	}
	c.main_action = ZA_POST;
	c.clip = zd->clip;
	c.clip_cap = sizeof(zd->clip);
	c.write = zed_write;
	c.ctx = n;
	set_state(n, N_ZETTA);
	// room kept after the text: wrapping at 79 can add a line end where a
	// word is cut
	zetta_init(&zd->z, &c, n->ed, MSG_BODY_MAX - 256);
}

static void zed_abandon(node_t *n) {
	zed_end(n);
	editor_free(n);
	out_mci(n, "|07Not written.|07\r\n");
	anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
}

static void zed_post(node_t *n) {
	zed_t *zd = n->zed;
	static char wrapped[MSG_BODY_MAX];
	if (zd->mail) {
		char norm[sizeof(n->post.to)];
		const char *why = mail_to_check(n->post.to, norm, sizeof(norm));
		if (why) { zetta_error(&zd->z, why); return; }
		utf8_copy(n->post.to, sizeof(n->post.to), norm);
		n->post.to_id = 0;
		if (!strchr(norm, '@')) {
			int idx = users_find(norm);
			user_t u;
			if (idx >= 0 && users_read(idx, &u)) n->post.to_id = u.id;
		}
	}
	if (!n->post.subject[0]) { zetta_error(&zd->z, "A subject, please."); return; }
	uint32_t wl = zetta_text(&zd->z, wrapped, sizeof(wrapped), 79);
	bool any = false;
	for (uint32_t i = 0; i < wl; i++) if (wrapped[i] != ' ' && wrapped[i] != '\n') any = true;
	if (!any) { zetta_error(&zd->z, "Nothing written yet."); return; }
	memcpy(n->ed, wrapped, wl);
	n->ed_len = wl;
	n->ed[wl] = 0;
	zed_end(n);
	editor_save(n);				// the same way on as the line editor's /s: forums, mail, federation
}

static void zed_event(node_t *n, int r) {
	zed_t *zd = n->zed;
	if (!zd) return;
	switch (r) {
	case ZE_ACTION:
		if (zd->z.action == ZA_POST) zed_post(n);
		else if (zd->z.action == ZA_QUOTE) zetta_pick(&zd->z, "Quote", zed_line, zd, "> ", ZT_QUOTE);
		return;
	case ZE_EXIT:
		if (zd->z.len || zd->z.modified) zetta_confirm(&zd->z, "Discard this message?", ZT_DISCARD);
		else zed_abandon(n);
		return;
	case ZE_ANSWER:
		if (zd->z.answer_tag == ZT_DISCARD && zd->z.answer == 'y') zed_abandon(n);
		return;
	}
}

void zed_input(node_t *n, const uint8_t *d, uint32_t len) {
	if (!n->zed) return;
	zed_event(n, zetta_key(&((zed_t *)n->zed)->z, d, len, plat_ms()));
}

void zed_tick(node_t *n) {
	if (!n->zed) return;
	zed_event(n, zetta_tick(&((zed_t *)n->zed)->z, plat_ms()));
}

static void editor_start(node_t *n, bool quote) {
	editor_free(n);
	n->ed = malloc(MSG_BODY_MAX);
	if (!n->ed) {
		out_mci(n, "|12Not enough memory to write just now -- try again in a moment.|07\r\n");
		bbs_logf("node %d: no memory for the editor", node_index(n) + 1);
		anykey(n, N_MENU);
		return;
	}
	n->ed[0] = 0;
	out_nl(n);
	out_mci(n, ed_help);
	out_nl(n);
	if (quote) {
		msg_t m;
		static char body[4096];
		if (msg_head(n->area, (int)n->post.reply, &m, NULL)) {
			uint32_t bl = msg_body(n->area, &m, body, sizeof(body));
			ed_quote(n, m.from, body, bl);
			ed_list(n);
		}
	}
	editor_prompt(n, NULL);
}

static void editor_prompt(node_t *n, const char *carry) {
	char p[24];
	snprintf(p, sizeof(p), "|03%3d>|07 ", n->ed_lines + 1);
	field(n, N_EDIT, p, ED_LINE_CHARS, false, carry);
}

// A character past the end of the line: the last word moves down.
bool editor_wrap(node_t *n, uint32_t k) {
	char carry[FIELD_MAX], one[5];
	int kl = utf8_put(k, one);
	one[kl] = 0;
	carry[0] = 0;
	if (k != ' ') {
		char *sp = strrchr(n->field, ' ');
		if (sp && sp != n->field) {
			// erase the word on screen, keep it for the next line
			for (int i = utf8_chars(sp + 1); i > 0; i--) OUT_LIT(n, "\b \b");
			snprintf(carry, sizeof(carry), "%s", sp + 1);
			*sp = 0;
		}
		size_t cl = strlen(carry);
		if (cl + (size_t)kl < sizeof(carry)) memcpy(carry + cl, one, (size_t)kl + 1);
	}
	n->field_on = false;
	out_nl(n);
	if (!ed_add(n, n->field)) {
		out_mci(n, "|12The message is full: /s to save it.|07\r\n");
		editor_prompt(n, NULL);
		return true;
	}
	editor_prompt(n, carry);
	return true;
}

static void editor_save(node_t *n) {
	int a = n->area;
	// no trailing blank lines
	while (n->ed_len >= 2 && n->ed[n->ed_len - 1] == '\n' && n->ed[n->ed_len - 2] == '\n') {
		n->ed[--n->ed_len] = 0;
		n->ed_lines--;
	}
	if (n->ed_len == 0 || (n->ed_len == 1 && n->ed[0] == '\n')) {
		out_mci(n, "|12Nothing written -- not saved.|07\r\n");
		editor_free(n);
		anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
		return;
	}
	msg_t *m = &n->post;
	utf8_copy(m->from, sizeof(m->from), n->user.handle);
	m->from_id = n->user.id;
	if (a == 0 && strchr(m->to, '@')) {
		// a letter to another node: sealed to it by fed (docs/fed.md)
		int r = fed_mail(m, n->ed, n->ed_len);
		editor_free(n);
		if (r < 0) {
			out_mci(n, "|12It could not be sent -- please tell the sysop.|07\r\n");
			anykey(n, N_MENU);
			return;
		}
		bbs_logf("node %d: %s wrote to %s", node_index(n) + 1, n->user.handle, m->to);
		out_fg(n, 10);
		out_text(n, r ? "Sent over the network, sealed: only that node can read it." :
			"Waiting for this node's fed: it is sent when it can be.");
		out_fg(n, 7); out_nl(n);
		anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
		return;
	}
	if (bbs_area[a].topic[0]) {
		// federated: published, and in the forum when fed delivers it back
		int r = fed_post(a, m, n->ed, n->ed_len);
		editor_free(n);
		if (r < 0) {
			out_mci(n, "|12It could not be sent -- please tell the sysop.|07\r\n");
			anykey(n, N_MENU);
			return;
		}
		bbs_logf("node %d: %s wrote to %s (%s)", node_index(n) + 1, n->user.handle, bbs_area[a].tag, bbs_area[a].topic);
		out_fg(n, 10);
		out_text(n, r ? "Sent to the network: it appears here when the network has it." :
			"Waiting for this node's fed: it is sent when it can be.");
		out_fg(n, 7); out_nl(n);
		anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
		return;
	}
	m->id[0] = 0;						// a local message: msg_post() names it
	int num = msg_post(a, m, n->ed, n->ed_len);
	editor_free(n);
	if (num < 0) {
		out_mci(n, "|12It could not be saved -- please tell the sysop.|07\r\n");
		anykey(n, N_MENU);
		return;
	}
	// your own message is not news to you
	if (a != 0 && n->lastread[a] == (uint32_t)num - 1) { n->lastread[a] = (uint32_t)num; n->lastread_dirty = true; }
	bbs_logf("node %d: %s wrote %s #%d", node_index(n) + 1, n->user.handle, bbs_area[a].tag, num);
	char s[80];
	snprintf(s, sizeof(s), "Saved, #%d.", num);
	out_fg(n, 10); out_text(n, s); out_fg(n, 7); out_nl(n);
	anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
}

// ------------------------------------------------------------------
// fields and keys, from session.c's dispatch
// ------------------------------------------------------------------

bool reader_field(node_t *n) {
	switch (n->state) {
	case N_AREAS: {
		if (!n->field[0]) { show_menu(n); return true; }
		int a = forum_by_number(n, atoi(n->field));
		if (a < 0) { forums_show(n); return true; }
		area_menu(n, a);
		return true;
	}
	case N_POST_TO: {
		if (!n->field[0]) { area_menu(n, n->area); return true; }
		char norm[sizeof(n->post.to)];
		const char *why = mail_to_check(n->field, norm, sizeof(norm));
		if (why) {
			out_mci(n, "|12"); out_text(n, why); out_mci(n, "|07\r\n");
			field(n, N_POST_TO, "|15To|07 (a handle, or handle@node): ", 50, false, NULL);
			return true;
		}
		utf8_copy(n->post.to, sizeof(n->post.to), norm);
		n->post.to_id = 0;
		if (!strchr(norm, '@')) {
			user_t u;
			int idx = users_find(norm);
			if (idx >= 0 && users_read(idx, &u)) n->post.to_id = u.id;
		}
		post_subject(n);
		return true;
	}
	case N_POST_SUBJ:
		if (!n->field[0]) {
			out_mci(n, "|07Not written.|07\r\n");
			if (n->msgnum) reader_after_body(n); else area_menu(n, n->area);
			return true;
		}
		utf8_copy(n->post.subject, sizeof(n->post.subject), n->field);
		if (n->post.reply) {
			set_state(n, N_QUOTE_ASK);
			out_mci(n, "Quote the message? |15[Y/n]|07 ");
			return true;
		}
		editor_start(n, false);
		return true;
	case N_EDIT: {
		const char *f = n->field;
		if (f[0] == '/' && f[1] && (f[2] == 0 || f[2] == ' ')) {
			switch (f[1]) {
			case 's': case 'S': editor_save(n); return true;
			case 'a': case 'A':
				editor_free(n);
				out_mci(n, "|07Abandoned.|07\r\n");
				anykey(n, n->msgnum ? N_READ : N_AREA_MENU);
				return true;
			case 'l': case 'L': ed_list(n); editor_prompt(n, NULL); return true;
			case 'd': case 'D':
				if (!ed_delete(n, atoi(f + 2))) out_mci(n, "|12No such line.|07\r\n");
				else ed_list(n);
				editor_prompt(n, NULL);
				return true;
			case '?': out_mci(n, ed_help); editor_prompt(n, NULL); return true;
			}
		}
		if (!ed_add(n, f)) out_mci(n, "|12The message is full: /s to save it.|07\r\n");
		editor_prompt(n, NULL);
		return true;
	}
	default:
		return false;
	}
}

bool reader_keyed(node_t *n, uint32_t k) {
	switch (n->state) {
	case N_AREA_MENU: return area_menu_key(n, k);
	case N_READ: reader_key(n, k); return true;
	case N_QUOTE_ASK:
		out_nl(n);
		editor_start(n, k == 'y' || k == 'Y' || k == K_ENTER);
		return true;
	default:
		return false;
	}
}
