/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the message base. docs/bbs.md, "Messages".
 *
 * An area (a forum, or the one mail area) is two files:
 *
 *   msgs/<tag>.log   the messages, appended, never rewritten
 *   msgs/<tag>.idx   32 bytes per message: where it is, and what a list
 *                    or a scan needs without reading it
 *
 * The log is written first and the index after, so a crash between the
 * two leaves a log ahead of its index -- which area_repair() notices at
 * start, and closes by reading the missing messages back out of the log.
 * One process writes (the BBS), so nothing here locks.
 *
 * A message in the log:
 *
 *   ZM1 <bytes that follow>\n
 *   id: 3f9c0a17e2b44d61:general:12
 *   from: Phil
 *   from_id: 1
 *   subject: Hello
 *   date: 1790000000
 *   \n
 *   the body, UTF-8, lines ending \n
 *
 * Plain text so a person can read a log, and so phase 5 can carry
 * messages between nodes as they are. The id is where the message was
 * written (this node), its area, and its number there: unique without
 * anyone assigning anything. Provisional until the fed spec (phase 5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbs_int.h"

area_t bbs_area[AREA_MAX];
int bbs_nareas;
char bbs_node_id[17];

// ------------------------------------------------------------------
// the node's id
// ------------------------------------------------------------------

bool node_id_load(void) {
	char p[BBS_PATH_MAX], b[32];
	if (!bbs_path(p, "node.id")) return false;
	int h = plat_open(p, PLAT_READ);
	if (h >= 0) {
		int n = plat_read(h, b, 16);
		plat_close(h);
		if (n == 16) {
			bool hex = true;
			for (int i = 0; i < 16; i++) if (!((b[i] >= '0' && b[i] <= '9') || (b[i] >= 'a' && b[i] <= 'f'))) hex = false;
			if (hex) { memcpy(bbs_node_id, b, 16); bbs_node_id[16] = 0; return true; }
		}
		bbs_logf("node.id is damaged: not replacing it -- fix or delete it");
		return false;
	}
	uint8_t r[8];
	plat_random(r, sizeof(r));
	for (int i = 0; i < 8; i++) snprintf(bbs_node_id + 2 * i, 3, "%02x", r[i]);
	h = plat_open(p, PLAT_CREATE);
	if (h < 0) return false;
	snprintf(b, sizeof(b), "%s\n", bbs_node_id);
	bool ok = plat_write(h, b, 17) == 17;
	plat_close(h);
	if (ok) bbs_logf("this BBS's node id: %s", bbs_node_id);
	return ok;
}

// ------------------------------------------------------------------
// areas: <datadir>/forums.cfg
// ------------------------------------------------------------------

static const char *default_forums =
	"# forums.cfg -- the forums (docs/bbs.md, \"Forums\").\n"
	"# tag; name; level to read; level to write; description\n"
	"# A tag is its name on the network later: a-z 0-9 - _, at most 16.\n"
	"# Add, reorder, rename freely; never reuse a tag for something else.\n"
	"general; General; 0; 10; Anything at all\n"
	"zeitlos; Zeitlos; 0; 10; The computer, the OS, the apps\n";

static bool tag_ok(const char *t) {
	size_t l = strlen(t);
	if (l < 1 || l > AREA_TAG_MAX) return false;
	for (size_t i = 0; i < l; i++)
		if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '-' || t[i] == '_')) return false;
	return true;
}

static void trim(char *s) {
	char *a = s;
	while (*a == ' ' || *a == '\t') a++;
	memmove(s, a, strlen(a) + 1);
	size_t l = strlen(s);
	while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
}

// Splits one line on ';' into at most 5 fields, in place.
static int split(char *line, char **f) {
	int k = 0;
	f[k++] = line;
	for (char *p = line; *p && k < 6; p++) if (*p == ';') { *p = 0; f[k++] = p + 1; }
	for (int i = 0; i < k; i++) trim(f[i]);
	return k;
}

char bbs_fed_network[33];

// A zfed topic (docs/fed.md): 1-96 bytes of a-z 0-9 / . _ -, no empty segment.
static bool topic_ok(const char *t) {
	size_t n = strlen(t);
	if (n < 1 || n > 96 || t[0] == '/' || t[n - 1] == '/') return false;
	for (size_t i = 0; i < n; i++) {
		char c = t[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-')) return false;
		if (c == '/' && t[i + 1] == '/') return false;
	}
	return true;
}

void areas_parse(const char *text, uint32_t len) {
	char line[256];
	const char *p = text, *end = text + len;
	memset(bbs_area, 0, sizeof(bbs_area));
	bbs_fed_network[0] = 0;
	// area 0 is always the mail
	strcpy(bbs_area[0].tag, "mail");
	strcpy(bbs_area[0].name, "Private mail");
	bbs_area[0].is_mail = true;
	bbs_area[0].wlevel = 1;
	bbs_nareas = 1;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol) eol = end;
		size_t l = (size_t)(eol - p);
		if (l >= sizeof(line)) l = sizeof(line) - 1;
		memcpy(line, p, l);
		line[l] = 0;
		p = eol + 1;
		trim(line);
		if (!line[0] || line[0] == '#') continue;
		char *f[6] = { 0 };
		int k = split(line, f);
		if (k < 2 || !tag_ok(f[0]) || !strcmp(f[0], "mail")) {
			bbs_logf("forums.cfg: '%s' is not a forum (tag: a-z 0-9 - _, not 'mail')", f[0]);
			continue;
		}
		if (area_find(f[0]) >= 0) { bbs_logf("forums.cfg: '%s' twice", f[0]); continue; }
		if (bbs_nareas >= AREA_MAX) { bbs_logf("forums.cfg: more than %d forums", AREA_MAX - 1); break; }
		area_t *a = &bbs_area[bbs_nareas++];
		strcpy(a->tag, f[0]);
		utf8_copy(a->name, sizeof(a->name), f[1][0] ? f[1] : f[0]);
		int r = k > 2 ? atoi(f[2]) : 0, w = k > 3 ? atoi(f[3]) : 10;
		a->rlevel = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
		a->wlevel = (uint8_t)(w < 0 ? 0 : w > 255 ? 255 : w);
		if (k > 4) utf8_copy(a->desc, sizeof(a->desc), f[4]);
		if (k > 5 && f[5][0]) {
			// federated: its zfed topic, in the same network as the others
			char net[33];
			const char *slash = strchr(f[5], '/');
			if (!topic_ok(f[5]) || !slash || (size_t)(slash - f[5]) >= sizeof(net)) {
				bbs_logf("forums.cfg: '%s': '%s' is not a zfed topic -- the forum stays local", a->tag, f[5]);
			} else {
				memcpy(net, f[5], (size_t)(slash - f[5]));
				net[slash - f[5]] = 0;
				if (bbs_fed_network[0] && strcmp(net, bbs_fed_network)) {
					bbs_logf("forums.cfg: '%s': network %s, but the others are in %s -- the forum stays local",
						a->tag, net, bbs_fed_network);
				} else {
					snprintf(bbs_fed_network, sizeof(bbs_fed_network), "%s", net);
					snprintf(a->topic, sizeof(a->topic), "%s", f[5]);
				}
			}
		}
	}
}

int area_find(const char *tag) {
	for (int i = 0; i < bbs_nareas; i++) if (!strcmp(bbs_area[i].tag, tag)) return i;
	return -1;
}

static bool area_path(char *out, int a, const char *ext) {
	char rel[40];
	snprintf(rel, sizeof(rel), "msgs/%s.%s", bbs_area[a].tag, ext);
	return bbs_path(out, rel);
}

bool areas_load(void) {
	char p[BBS_PATH_MAX];
	static char text[4096];
	if (bbs_path(p, "msgs")) plat_mkdir(p);
	if (!bbs_path(p, "lastread")) return false;
	plat_mkdir(p);
	if (!bbs_path(p, "forums.cfg")) return false;
	if (plat_size(p) < 0) {
		int h = plat_open(p, PLAT_CREATE);
		if (h >= 0) { plat_write(h, default_forums, (int)strlen(default_forums)); plat_close(h); }
	}
	int n = 0, h = plat_open(p, PLAT_READ);
	if (h >= 0) { n = plat_read(h, text, (int)sizeof(text) - 1); plat_close(h); }
	if (n < 0) n = 0;
	areas_parse(text, (uint32_t)n);
	for (int a = 0; a < bbs_nareas; a++) area_repair(a);
	return true;
}

// ------------------------------------------------------------------
// the index
// ------------------------------------------------------------------

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void idx_pack(const idx_t *e, uint8_t r[IDX_SIZE]) {
	put32(r, e->off); put32(r + 4, e->len); put32(r + 8, e->date); put32(r + 12, e->from_id);
	put32(r + 16, e->to_id); put32(r + 20, e->reply); put32(r + 24, e->flags); put32(r + 28, 0);
}

static void idx_unpack(idx_t *e, const uint8_t r[IDX_SIZE]) {
	e->off = get32(r); e->len = get32(r + 4); e->date = get32(r + 8); e->from_id = get32(r + 12);
	e->to_id = get32(r + 16); e->reply = get32(r + 20); e->flags = get32(r + 24);
}

int msg_count(int a) {
	return (a >= 0 && a < bbs_nareas) ? (int)bbs_area[a].count : 0;
}

bool msg_idx(int a, int num, idx_t *e) {
	char p[BBS_PATH_MAX];
	uint8_t r[IDX_SIZE];
	if (num < 1 || num > msg_count(a) || !area_path(p, a, "idx")) return false;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return false;
	bool ok = plat_seek(h, (uint32_t)(num - 1) * IDX_SIZE) && plat_read(h, r, IDX_SIZE) == IDX_SIZE;
	plat_close(h);
	if (ok) idx_unpack(e, r);
	return ok;
}

// Entries first..first+k-1 in one open, for scans (mail, skipping).
// Returns how many were read.
int msg_idx_many(int a, int first, idx_t *e, int k) {
	char p[BBS_PATH_MAX];
	uint8_t r[IDX_SIZE * 16];
	int got = 0;
	if (first < 1 || !area_path(p, a, "idx")) return 0;
	if (first + k - 1 > msg_count(a)) k = msg_count(a) - first + 1;
	if (k <= 0) return 0;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return 0;
	if (plat_seek(h, (uint32_t)(first - 1) * IDX_SIZE)) {
		while (got < k) {
			int want = k - got > 16 ? 16 : k - got;
			if (plat_read(h, r, want * IDX_SIZE) != want * IDX_SIZE) break;
			for (int i = 0; i < want; i++) idx_unpack(&e[got + i], r + i * IDX_SIZE);
			got += want;
		}
	}
	plat_close(h);
	return got;
}

bool msg_idx_write(int a, int num, const idx_t *e) {
	char p[BBS_PATH_MAX];
	uint8_t r[IDX_SIZE];
	if (num < 1 || num > msg_count(a) || !area_path(p, a, "idx")) return false;
	idx_pack(e, r);
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_seek(h, (uint32_t)(num - 1) * IDX_SIZE) && plat_write(h, r, IDX_SIZE) == IDX_SIZE;
	plat_close(h);
	return ok;
}

static bool idx_append(int a, int num, const idx_t *e) {
	char p[BBS_PATH_MAX];
	uint8_t r[IDX_SIZE];
	if (!area_path(p, a, "idx")) return false;
	if (plat_size(p) < 0) { int h = plat_open(p, PLAT_CREATE); if (h < 0) return false; plat_close(h); }
	idx_pack(e, r);
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	bool ok = plat_seek(h, (uint32_t)(num - 1) * IDX_SIZE) && plat_write(h, r, IDX_SIZE) == IDX_SIZE;
	plat_close(h);
	return ok;
}

// ------------------------------------------------------------------
// reading a message's headers
// ------------------------------------------------------------------

static char rec_buf[MSG_HEAD_MAX + 32];

static uint32_t hval_u32(const char *v) { return (uint32_t)strtoul(v, NULL, 10); }

// Parses the record at `off` (headers only). False if it is not one.
static bool parse_rec(int h, uint32_t off, msg_t *m, uint32_t *rec_len) {
	memset(m, 0, sizeof(*m));
	if (!plat_seek(h, off)) return false;
	int got = plat_read(h, rec_buf, (int)sizeof(rec_buf) - 1);
	if (got < 8) return false;
	rec_buf[got] = 0;
	if (memcmp(rec_buf, "ZM1 ", 4)) return false;
	char *nl = memchr(rec_buf, '\n', (size_t)got);
	if (!nl) return false;
	uint32_t rest = hval_u32(rec_buf + 4);
	uint32_t hdr0 = (uint32_t)(nl + 1 - rec_buf);
	*rec_len = hdr0 + rest;
	// headers up to the blank line
	char *p = nl + 1, *end = rec_buf + got;
	while (p < end && *p != '\n') {
		char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol) return false;              // headers longer than MSG_HEAD_MAX
		*eol = 0;
		char *colon = strchr(p, ':');
		if (colon) {
			*colon = 0;
			char *v = colon + 1;
			while (*v == ' ') v++;
			if (!strcmp(p, "id")) utf8_copy(m->id, sizeof(m->id), v);
			else if (!strcmp(p, "from")) utf8_copy(m->from, sizeof(m->from), v);
			else if (!strcmp(p, "from_id")) m->from_id = hval_u32(v);
			else if (!strcmp(p, "to")) utf8_copy(m->to, sizeof(m->to), v);
			else if (!strcmp(p, "to_id")) m->to_id = hval_u32(v);
			else if (!strcmp(p, "subject")) utf8_copy(m->subject, sizeof(m->subject), v);
			else if (!strcmp(p, "date")) m->date = hval_u32(v);
			else if (!strcmp(p, "reply")) m->reply = hval_u32(v);
			else if (!strcmp(p, "reply_id")) utf8_copy(m->reply_id, sizeof(m->reply_id), v);
			else if (!strcmp(p, "post")) utf8_copy(m->post, sizeof(m->post), v);
			// anything else: a later version's, kept in the log, ignored here
		}
		p = eol + 1;
	}
	if (p >= end) return false;
	m->body_off = off + (uint32_t)(p + 1 - rec_buf);
	m->body_len = off + *rec_len > m->body_off ? off + *rec_len - m->body_off : 0;
	return true;
}

bool msg_head(int a, int num, msg_t *m, idx_t *e) {
	char p[BBS_PATH_MAX];
	idx_t ie;
	uint32_t len;
	if (!msg_idx(a, num, &ie) || !area_path(p, a, "log")) return false;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return false;
	bool ok = parse_rec(h, ie.off, m, &len);
	plat_close(h);
	if (e) *e = ie;
	m->num = num;
	return ok;
}

// ------------------------------------------------------------------
// repair: the index behind the log
// ------------------------------------------------------------------

void area_repair(int a) {
	char lp[BBS_PATH_MAX], ip[BBS_PATH_MAX];
	area_t *ar = &bbs_area[a];
	if (!area_path(lp, a, "log") || !area_path(ip, a, "idx")) return;
	int32_t lsz = plat_size(lp), isz = plat_size(ip);
	if (lsz < 0) lsz = 0;
	if (isz < 0) isz = 0;
	int n = (int)(isz / IDX_SIZE);
	ar->count = (uint32_t)n;
	// Index entries pointing past the log (a log restored from an older
	// copy): not messages. Counted off from the end.
	uint32_t end = 0;
	while (ar->count > 0) {
		idx_t e;
		if (msg_idx(a, (int)ar->count, &e) && e.off + e.len <= (uint32_t)lsz) { end = e.off + e.len; break; }
		ar->count--;
	}
	if ((int)ar->count < n)
		bbs_logf("msgs/%s.idx: %d entries past the end of the log ignored", ar->tag, n - (int)ar->count);
	if (end >= (uint32_t)lsz) return;
	// The log is ahead: index what it has.
	int h = plat_open(lp, PLAT_READ);
	if (h < 0) return;
	int added = 0;
	while (end < (uint32_t)lsz) {
		msg_t m;
		uint32_t len;
		// A whole record: it parses, it fits, it ends with its newline,
		// and what follows is the end of the log or another record. A
		// torn write can pass the first two -- its length field is
		// written before its body -- but not all four.
		char tail[5] = { 0 };
		bool whole = parse_rec(h, end, &m, &len) && end + len <= (uint32_t)lsz &&
			plat_seek(h, end + len - 1) && plat_read(h, tail, 5) >= 1 && tail[0] == '\n' &&
			(end + len == (uint32_t)lsz || !memcmp(tail + 1, "ZM1 ", 4));
		if (!whole) {
			bbs_logf("msgs/%s.log: no whole message from byte %u on -- the next one is written there",
				ar->tag, (unsigned)end);
			break;
		}
		idx_t e = { end, len, m.date, m.from_id, m.to_id, m.reply, 0 };
		plat_close(h);
		if (!idx_append(a, (int)ar->count + 1, &e)) return;
		ar->count++;
		added++;
		end += len;
		h = plat_open(lp, PLAT_READ);
		if (h < 0) return;
	}
	plat_close(h);
	if (added) bbs_logf("msgs/%s: %d messages indexed from the log", ar->tag, added);
}

// ------------------------------------------------------------------
// posting
// ------------------------------------------------------------------

// Header values are single lines: no newline or control gets in.
static int add_header(char *buf, int o, int cap, const char *k, const char *v) {
	if (o >= cap) return o;
	int n = snprintf(buf + o, (size_t)(cap - o), "%s: ", k);
	o += n;
	for (const char *s = v; *s && o < cap - 2; s++) {
		uint8_t c = (uint8_t)*s;
		buf[o++] = (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
	}
	if (o < cap - 1) buf[o++] = '\n';
	return o;
}

int msg_post(int a, msg_t *m, const char *body, uint32_t blen) {
	char lp[BBS_PATH_MAX], num[16], head[MSG_HEAD_MAX];
	area_t *ar = &bbs_area[a];
	if (a < 0 || a >= bbs_nareas || !area_path(lp, a, "log")) return -1;
	int n = (int)ar->count + 1;
	if (!m->date) m->date = plat_now();
	// a federated message comes with its name on the network; a local one gets ours
	if (!m->id[0]) snprintf(m->id, sizeof(m->id), "%s:%s:%d", bbs_node_id, ar->tag, n);
	int o = 0;
	o = add_header(head, o, (int)sizeof(head), "id", m->id);
	o = add_header(head, o, (int)sizeof(head), "from", m->from);
	snprintf(num, sizeof(num), "%u", (unsigned)m->from_id);
	o = add_header(head, o, (int)sizeof(head), "from_id", num);
	if (m->to[0]) {
		o = add_header(head, o, (int)sizeof(head), "to", m->to);
		snprintf(num, sizeof(num), "%u", (unsigned)m->to_id);
		o = add_header(head, o, (int)sizeof(head), "to_id", num);
	}
	o = add_header(head, o, (int)sizeof(head), "subject", m->subject);
	snprintf(num, sizeof(num), "%u", (unsigned)m->date);
	o = add_header(head, o, (int)sizeof(head), "date", num);
	if (m->reply) {
		snprintf(num, sizeof(num), "%u", (unsigned)m->reply);
		o = add_header(head, o, (int)sizeof(head), "reply", num);
		if (m->reply_id[0]) o = add_header(head, o, (int)sizeof(head), "reply_id", m->reply_id);
	}
	if (m->post[0]) o = add_header(head, o, (int)sizeof(head), "post", m->post);
	if (o >= (int)sizeof(head) - 1) return -1;
	head[o++] = '\n';                             // the blank line
	// the body, ending in a newline
	bool nl = blen && body[blen - 1] == '\n';
	uint32_t rest = (uint32_t)o + blen + (nl ? 0 : 1);
	char pre[24];
	int pl = snprintf(pre, sizeof(pre), "ZM1 %u\n", (unsigned)rest);

	if (plat_size(lp) < 0) { int h = plat_open(lp, PLAT_CREATE); if (h < 0) return -1; plat_close(h); }
	int32_t at = plat_size(lp);
	// The index says where the log ends; anything after that is a torn
	// write from before, overwritten rather than left in the middle.
	if (ar->count) {
		idx_t last;
		if (msg_idx(a, (int)ar->count, &last)) at = (int32_t)(last.off + last.len);
	} else at = 0;
	int h = plat_open(lp, PLAT_UPDATE);
	if (h < 0) return -1;
	bool ok = plat_seek(h, (uint32_t)at) &&
		plat_write(h, pre, pl) == pl &&
		plat_write(h, head, o) == o &&
		(blen == 0 || plat_write(h, body, (int)blen) == (int)blen) &&
		(nl || plat_write(h, "\n", 1) == 1);
	plat_close(h);
	if (!ok) { bbs_logf("msgs/%s.log: could not write", ar->tag); return -1; }
	idx_t e = { (uint32_t)at, (uint32_t)pl + rest, m->date, m->from_id, m->to_id, m->reply, 0 };
	if (!idx_append(a, n, &e)) { bbs_logf("msgs/%s.idx: could not write", ar->tag); return -1; }
	ar->count = (uint32_t)n;
	m->num = n;
	return n;
}

int msg_find(int a, const char *id, const char *post, int back) {
	if (a < 0 || a >= bbs_nareas) return 0;
	int last = (int)bbs_area[a].count;
	for (int num = last; num >= 1 && num > last - back; num--) {
		msg_t m;
		idx_t e;
		if (!msg_head(a, num, &m, &e)) continue;
		if (id && id[0] && !strcmp(m.id, id)) return num;
		if (post && post[0] && !strcmp(m.post, post)) return num;
	}
	return 0;
}

// Reads up to cap-1 bytes of a message's body.
uint32_t msg_body(int a, const msg_t *m, char *buf, uint32_t cap) {
	char p[BBS_PATH_MAX];
	if (!cap || !area_path(p, a, "log")) return 0;
	uint32_t want = m->body_len < cap - 1 ? m->body_len : cap - 1;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) { buf[0] = 0; return 0; }
	int got = plat_seek(h, m->body_off) ? plat_read(h, buf, (int)want) : 0;
	plat_close(h);
	if (got < 0) got = 0;
	buf[got] = 0;
	return (uint32_t)got;
}

bool msg_log_path(int a, char *out) {
	return area_path(out, a, "log");
}

// ------------------------------------------------------------------
// last read: lastread/<user id>.txt, "tag number" lines
// ------------------------------------------------------------------

void lastread_load(node_t *n) {
	char rel[40], p[BBS_PATH_MAX], text[1024];
	memset(n->lastread, 0, sizeof(n->lastread));
	snprintf(rel, sizeof(rel), "lastread/%u.txt", (unsigned)n->user.id);
	if (!bbs_path(p, rel)) return;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return;
	int got = plat_read(h, text, (int)sizeof(text) - 1);
	plat_close(h);
	if (got <= 0) return;
	text[got] = 0;
	// "tag number" lines. Parsed by hand: sscanf would bring newlib's
	// whole scanf, floating point included, into the board's binary --
	// some 70KB for one line of work.
	for (char *line = text; *line; ) {
		char *eol = strchr(line, '\n');
		if (eol) *eol = 0;
		char *sp = strchr(line, ' ');
		if (sp && sp - line <= AREA_TAG_MAX) {
			*sp = 0;
			int a = area_find(line);
			if (a > 0) n->lastread[a] = (uint32_t)strtoul(sp + 1, NULL, 10);
		}
		if (!eol) break;
		line = eol + 1;
	}
}

// Written to a new file and renamed over the old: a crash leaves one or
// the other, never half of each.
void lastread_save(node_t *n) {
	char rel[40], p[BBS_PATH_MAX], tmp[BBS_PATH_MAX], text[1024];
	if (!n->logged_in || !n->lastread_dirty) return;
	snprintf(rel, sizeof(rel), "lastread/%u.txt", (unsigned)n->user.id);
	if (!bbs_path(p, rel)) return;
	snprintf(rel, sizeof(rel), "lastread/%u.new", (unsigned)n->user.id);
	if (!bbs_path(tmp, rel)) return;
	int o = 0;
	for (int a = 1; a < bbs_nareas; a++)
		if (n->lastread[a]) o += snprintf(text + o, sizeof(text) - (size_t)o, "%s %u\n", bbs_area[a].tag, (unsigned)n->lastread[a]);
	int h = plat_open(tmp, PLAT_CREATE);
	if (h < 0) return;
	bool ok = plat_write(h, text, o) == o;
	plat_close(h);
	if (ok && plat_rename(tmp, p)) n->lastread_dirty = false;
}

// New for this user: forums by last read; mail by its read flag.
int area_new(node_t *n, int a) {
	if (a == 0) {
		idx_t e[32];
		int c = 0;
		for (int i = 1; i <= msg_count(0); ) {
			int k = msg_idx_many(0, i, e, 32);
			if (k <= 0) break;
			for (int j = 0; j < k; j++)
				if (e[j].to_id == n->user.id && !(e[j].flags & (IDX_READ | IDX_DELETED))) c++;
			i += k;
		}
		return c;
	}
	int c = msg_count(a) - (int)n->lastread[a];
	return c > 0 ? c : 0;
}
