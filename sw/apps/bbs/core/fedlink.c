/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the link to this node's fed: federated forums. docs/bbs.md,
 * "Federation"; the protocol is docs/fed.md, "The local interface".
 *
 * The platform connects to bbs_cfg.fed (a socket on Linux, the port fed0
 * on Zeitlos) and moves bytes: bbs_fed_up(), bbs_fed_input(),
 * bbs_fed_output(). This speaks the lines:
 *
 *   KEY                      -> OK <this node's key> <name>
 *   SUB bbs <network>/(all)  -> OK subscribed after N; then OBJ pos len + bytes,
 *                               each answered ACK pos once it is stored
 *   PUB <topic> bbs.post json log - <len> + payload   -> OK <id> <pos> | ERR ...
 *
 * A post to a federated forum is PUBlished, and enters the message base
 * when fed delivers it back, under its object id -- one way in for every
 * post, local or not. Until fed says OK it waits in <datadir>/fed-outbox,
 * and is sent again after any reconnect. Sent twice, it would be two
 * objects: so each post carries a random token, and the second is
 * ignored on arrival. Delivery is at least once: an object that is
 * already in the forum (by id, or by token) is acknowledged and skipped.
 *
 * What arrives is from other people's machines, for callers' terminals:
 * control characters are taken out -- the C1 ones too, since U+009B is a
 * whole CSI to some terminals -- and '@' out of a remote handle, which
 * could otherwise pose as someone on another node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bbs_int.h"
#include "../../../common/zjson.h"
#include "../../../common/zsha256.h"

#define FL_IN_MAX   (18000 + 64)		// a line, or an OBJ line and its object
#define FL_OUT_MAX  (17600)
#define NAMES_MAX   200
#define BACK        500					// messages looked through for a repeat or a parent

enum { Q_KEY = 1, Q_SUB, Q_PUB, Q_MAIL, Q_OPEN, Q_CANCELLED };

static bool fl_up;
static uint8_t fl_in[FL_IN_MAX];
static uint32_t fl_in_len;
static uint8_t fl_out[FL_OUT_MAX];
static uint32_t fl_out_head, fl_out_len;
static char fl_self[65];					// this node's key, from KEY
static char fl_sid[17], fl_node[33];		// ... its short id (its mail topic) and name
// a letter handed to fed to OPEN: acknowledged once it is stored
static struct { bool on; uint32_t pos, time; char id[65], origin[65]; } fl_open;
// a cancel for a message here: fed asked whether it honoured it (docs/fed.md, "Moderation")
static struct { bool on; uint32_t pos; int area, num; } fl_cx;
static uint8_t fl_q[64];					// requests awaiting OK / ERR, oldest first
static int fl_qn;
static int fl_sent;							// outbox records sent since connecting
// A node's name, by the first 16 hex digits of its key: a clash could
// only show a wrong NAME -- who is accepted is fed's business, in full.
static struct { char key[17]; char name[33]; } fl_names[NAMES_MAX];
static int fl_nnames;
// 1,024 tokens: a post, or a list of ~110 fully described nodes (beyond
// that, names come from part of it; the rest show short ids). 24 KB.
#define FL_TOKENS 1024
static zjson_tok_t fl_tok[FL_TOKENS];
// ONE work buffer, for what is never needed at once: an outbox record,
// a post being built, a post's body arriving, the names file at start.
static char fl_rec[FL_OUT_MAX];

static void bounce_remote(const char *origin, const char *to, const char *subject, const char *why);

// -- bytes out --

static bool fl_room(uint32_t n) {
	if (fl_out_head && fl_out_head + fl_out_len + n > sizeof(fl_out)) {
		memmove(fl_out, fl_out + fl_out_head, fl_out_len);
		fl_out_head = 0;
	}
	return fl_out_head + fl_out_len + n <= sizeof(fl_out);
}

static bool fl_put(const void *d, uint32_t n, int req) {
	if (!fl_up || fl_qn >= (int)sizeof(fl_q) || !fl_room(n)) return false;
	memcpy(fl_out + fl_out_head + fl_out_len, d, n);
	fl_out_len += n;
	if (req) fl_q[fl_qn++] = (uint8_t)req;
	return true;
}

// -- the outbox: <datadir>/fed-outbox, records "<len>\n<request>" --

static bool outbox_path(char *p) { return bbs_path(p, "fed-outbox"); }

// The k-th record into buf; its length, or -1 if there is none.
static int outbox_get(int k, char *buf, uint32_t cap) {
	char p[BBS_PATH_MAX], head[16];
	if (!outbox_path(p)) return -1;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return -1;
	uint32_t off = 0;
	int len = -1;
	for (int i = 0; ; i++) {
		if (!plat_seek(h, off)) break;
		int n = plat_read(h, head, sizeof(head) - 1);
		if (n <= 0) break;
		head[n] = 0;
		char *nl = strchr(head, '\n');
		if (!nl) break;
		uint32_t l = (uint32_t)strtoul(head, NULL, 10);
		uint32_t at = off + (uint32_t)(nl - head) + 1;
		if (i == k) {
			if (l < cap && plat_seek(h, at) && plat_read(h, buf, (int)l) == (int)l) len = (int)l;
			break;
		}
		off = at + l;
	}
	plat_close(h);
	return len;
}

static bool outbox_add(const char *req, uint32_t n) {
	char p[BBS_PATH_MAX], head[16];
	if (!outbox_path(p)) return false;
	if (plat_size(p) < 0) { int c = plat_open(p, PLAT_CREATE); if (c < 0) return false; plat_close(c); }
	int32_t end = plat_size(p);
	int h = plat_open(p, PLAT_UPDATE);
	if (h < 0) return false;
	int hl = snprintf(head, sizeof(head), "%u\n", (unsigned)n);
	bool ok = plat_seek(h, (uint32_t)end) && plat_write(h, head, hl) == hl && plat_write(h, req, (int)n) == (int)n;
	plat_close(h);
	return ok;
}

// Drops the first record: the rest rewritten, renamed into place.
static void outbox_pop(void) {
	char *rec = fl_rec;
	char p[BBS_PATH_MAX], tmp[BBS_PATH_MAX], head[16];
	if (!outbox_path(p) || !bbs_path(tmp, "fed-outbox.new")) return;
	int h = plat_open(tmp, PLAT_CREATE);
	if (h < 0) return;
	for (int k = 1; ; k++) {
		int n = outbox_get(k, rec, FL_OUT_MAX);
		if (n < 0) break;
		int hl = snprintf(head, sizeof(head), "%u\n", (unsigned)n);
		plat_write(h, head, hl);
		plat_write(h, rec, n);
	}
	plat_close(h);
	plat_rename(tmp, p);
}

static int outbox_count(void) {
	int k = 0;
	while (outbox_get(k, fl_rec, sizeof(fl_rec)) >= 0) k++;
	return k;
}

// As many unsent records as there is room for.
static void outbox_send(void) {
	for (;;) {
		int n = outbox_get(fl_sent, fl_rec, sizeof(fl_rec));
		if (n < 0 || !fl_put(fl_rec, (uint32_t)n, strncmp(fl_rec, "MAIL ", 5) ? Q_PUB : Q_MAIL)) return;
		fl_sent++;
	}
}

// -- node names: from the network's list, kept in <datadir>/fed-names --

static void names_save(void) {
	char p[BBS_PATH_MAX], tmp[BBS_PATH_MAX], line[120];
	if (!bbs_path(p, "fed-names") || !bbs_path(tmp, "fed-names.new")) return;
	int h = plat_open(tmp, PLAT_CREATE);
	if (h < 0) return;
	for (int i = 0; i < fl_nnames; i++) {
		int n = snprintf(line, sizeof(line), "%s %s\n", fl_names[i].key, fl_names[i].name);	// short: fits fl_rec
		plat_write(h, line, n);
	}
	plat_close(h);
	plat_rename(tmp, p);
}

static void names_load(void) {
	char *buf = fl_rec;						// at start: nothing else uses it
	char p[BBS_PATH_MAX];
	fl_nnames = 0;
	if (!bbs_path(p, "fed-names")) return;
	int h = plat_open(p, PLAT_READ);
	if (h < 0) return;
	int n = plat_read(h, buf, FL_OUT_MAX - 1);
	plat_close(h);
	if (n <= 0) return;
	buf[n] = 0;
	for (char *s = buf; *s && fl_nnames < NAMES_MAX; ) {
		char *e = strchr(s, '\n');
		if (e) *e = 0;
		if (strlen(s) > 18 && s[16] == ' ') {
			memcpy(fl_names[fl_nnames].key, s, 16);
			fl_names[fl_nnames].key[16] = 0;
			utf8_copy(fl_names[fl_nnames].name, sizeof(fl_names[0].name), s + 17);
			fl_nnames++;
		}
		if (!e) break;
		s = e + 1;
	}
}

static const char *name_of(const char *key) {
	for (int i = 0; i < fl_nnames; i++) if (!strncmp(fl_names[i].key, key, 16)) return fl_names[i].name;
	return NULL;
}

static void names_from_list(const char *js, uint32_t len) {
	int nt = zjson_parse(js, len, fl_tok, FL_TOKENS, NULL);
	if (nt == -ZJ_E_TOKENS) { bbs_logf("fed: a node list longer than names are kept for -- short ids shown"); return; }
	if (nt < 1 || fl_tok[0].type != ZJ_OBJECT) return;
	int arr = zjson_get(js, fl_tok, 0, "nodes");
	if (arr < 0 || fl_tok[arr].type != ZJ_ARRAY) return;
	fl_nnames = 0;
	int t = arr + 1;
	for (uint32_t i = 0; i < fl_tok[arr].size && fl_nnames < NAMES_MAX; i++, t = zjson_next(fl_tok, t)) {
		int k = zjson_get(js, fl_tok, t, "key"), nm = zjson_get(js, fl_tok, t, "name");
		char key[70], name[40];
		if (k < 0 || nm < 0 || !zjson_str(js, &fl_tok[k], key, sizeof(key)) || strlen(key) != 64 ||
				!zjson_str(js, &fl_tok[nm], name, sizeof(name))) continue;
		memcpy(fl_names[fl_nnames].key, key, 16);
		fl_names[fl_nnames].key[16] = 0;
		utf8_copy(fl_names[fl_nnames].name, sizeof(fl_names[0].name), name);
		fl_nnames++;
	}
	names_save();
	bbs_logf("fed: %d node names from the %s list", fl_nnames, bbs_fed_network);
}

// -- text from elsewhere --

// Takes out what must not reach a terminal: C0 controls (a newline kept
// where `lines`, a tab made a space), DEL, and the C1 controls
// U+0080-U+009F (U+009B is a CSI). The text is valid UTF-8 (zjson).
static void clean(char *s, bool lines) {
	char *o = s;
	for (unsigned char *p = (unsigned char *)s; *p; ) {
		if (*p == '\n' && lines) { *o++ = (char)*p++; continue; }
		if (*p == '\t') { *o++ = ' '; p++; continue; }
		if (*p < 0x20 || *p == 0x7f) { p++; continue; }
		if (*p == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) { p += 2; continue; }
		*o++ = (char)*p++;
	}
	*o = 0;
}

// -- an object delivered --

static bool hdr(const char *h, uint32_t hl, const char *key, char *out, uint32_t cap) {
	size_t kl = strlen(key);
	for (const char *p = h; p < h + hl; ) {
		const char *e = memchr(p, '\n', (size_t)(h + hl - p));
		if (!e) break;
		if ((size_t)(e - p) > kl + 2 && !memcmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') {
			size_t n = (size_t)(e - p) - kl - 2;
			if (n >= cap) return false;
			memcpy(out, p + kl + 2, n);
			out[n] = 0;
			return true;
		}
		p = e + 1;
	}
	return false;
}

static void got_post(int a, const char *id, const char *origin, uint32_t otime, const char *js, uint32_t jl) {
	char *body = fl_rec;					// the outbox is not touched while a post is stored
	char from[64], subject[SUBJECT_MAX * 4 + 1], reply[72], post[40];
	int nt = zjson_parse(js, jl, fl_tok, FL_TOKENS, NULL);
	if (nt < 1 || fl_tok[0].type != ZJ_OBJECT) { bbs_logf("fed: a post that is not strict JSON -- skipped"); return; }
	int t;
	if ((t = zjson_get(js, fl_tok, 0, "from")) < 0 || !zjson_str(js, &fl_tok[t], from, sizeof(from)) || !from[0]) {
		bbs_logf("fed: a post with no author -- skipped"); return;
	}
	if ((t = zjson_get(js, fl_tok, 0, "body")) < 0 || !zjson_str(js, &fl_tok[t], body, FL_OUT_MAX)) {
		bbs_logf("fed: a post with no body -- skipped"); return;
	}
	subject[0] = reply[0] = post[0] = 0;
	if ((t = zjson_get(js, fl_tok, 0, "subject")) >= 0) {
		static char s[400];
		if (zjson_str(js, &fl_tok[t], s, sizeof(s))) utf8_copy(subject, sizeof(subject), s);
	}
	if ((t = zjson_get(js, fl_tok, 0, "reply")) >= 0) zjson_str(js, &fl_tok[t], reply, sizeof(reply));
	if ((t = zjson_get(js, fl_tok, 0, "post")) >= 0) zjson_str(js, &fl_tok[t], post, sizeof(post));
	if (strlen(post) > 32) post[0] = 0;		// kept whole or not at all: a cut one would never match
	uint32_t date = otime;
	if ((t = zjson_get(js, fl_tok, 0, "date")) >= 0 && fl_tok[t].type == ZJ_NUMBER && fl_tok[t].num > 0) {
		int64_t d = fl_tok[t].num;
		if (d >= (int64_t)otime - 86400 && d <= (int64_t)otime + 86400) date = (uint32_t)d;	// else the object's own time
	}
	clean(from, false); clean(subject, false); clean(body, true);
	for (char *p = from; *p; p++) if (*p == '@') *p = '_';		// no posing as someone elsewhere

	// a repeat -- the same object, or the same post sent twice -- is skipped
	if (msg_find(a, id, post, BACK)) return;

	msg_t m;
	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", id);
	memcpy(m.post, post, strlen(post) + 1);		// at most 32: above
	bool own = !strcmp(origin, fl_self);
	char handle[BBS_HANDLE_MAX * 4 + 1];
	utf8_copy(handle, sizeof(handle), from);
	if (own) {
		utf8_copy(m.from, sizeof(m.from), handle);
		int u = users_find(handle);
		m.from_id = u >= 0 ? (uint32_t)u + 1 : 0;
	} else {
		const char *nm = name_of(origin);
		char sid[17];
		if (!nm) { memcpy(sid, origin, 16); sid[16] = 0; nm = sid; }
		// made whole first, then cut at a character boundary if too long
		char both[sizeof(handle) + 40];
		snprintf(both, sizeof(both), "%s@%.32s", handle, nm);
		utf8_copy(m.from, sizeof(m.from), both);
	}
	utf8_copy(m.subject, sizeof(m.subject), subject[0] ? subject : "(no subject)");
	m.date = date;
	if (strlen(reply) == 64) {
		snprintf(m.reply_id, sizeof(m.reply_id), "%s", reply);
		int parent = msg_find(a, reply, NULL, BACK);
		m.reply = parent > 0 ? (uint32_t)parent : 0;
		if (!m.reply) m.reply_id[0] = 0;		// its parent is not here: shown as a new thread
	}
	uint32_t bl = (uint32_t)strlen(body);
	int num = msg_post(a, &m, body, bl);
	if (num < 0) bbs_logf("fed: %s: a post could not be stored", bbs_area[a].tag);
	else bbs_logf("fed: %s #%d from %s", bbs_area[a].tag, num, m.from);
}

// A letter to this node, opened by fed: into its addressee's mailbox --
// or, with no such user here, back to its sender's node as not delivered.
static void got_letter(const char *js, uint32_t jl) {
	char *body = fl_rec;			// the work buffer: a bounce below does not need the body
	char from[64], to[64], subject[SUBJECT_MAX * 4 + 1], reply[72], post[40];
	int nt = zjson_parse(js, jl, fl_tok, FL_TOKENS, NULL);
	int t;
	if (nt < 1 || fl_tok[0].type != ZJ_OBJECT ||
			(t = zjson_get(js, fl_tok, 0, "from")) < 0 || !zjson_str(js, &fl_tok[t], from, sizeof(from)) || !from[0] ||
			(t = zjson_get(js, fl_tok, 0, "to")) < 0 || !zjson_str(js, &fl_tok[t], to, sizeof(to)) || !to[0] ||
			(t = zjson_get(js, fl_tok, 0, "body")) < 0 || !zjson_str(js, &fl_tok[t], body, FL_OUT_MAX)) {
		bbs_logf("fed: a letter that is not one -- skipped");
		return;
	}
	subject[0] = reply[0] = post[0] = 0;
	if ((t = zjson_get(js, fl_tok, 0, "subject")) >= 0) {
		static char s[400];
		if (zjson_str(js, &fl_tok[t], s, sizeof(s))) utf8_copy(subject, sizeof(subject), s);
	}
	if ((t = zjson_get(js, fl_tok, 0, "reply")) >= 0) zjson_str(js, &fl_tok[t], reply, sizeof(reply));
	if ((t = zjson_get(js, fl_tok, 0, "post")) >= 0) zjson_str(js, &fl_tok[t], post, sizeof(post));
	if (strlen(post) > 32) post[0] = 0;
	uint32_t date = fl_open.time;
	if ((t = zjson_get(js, fl_tok, 0, "date")) >= 0 && fl_tok[t].type == ZJ_NUMBER && fl_tok[t].num > 0) {
		int64_t d = fl_tok[t].num;
		if (d >= (int64_t)fl_open.time - 86400 && d <= (int64_t)fl_open.time + 86400) date = (uint32_t)d;
	}
	clean(from, false); clean(to, false); clean(subject, false); clean(body, true);
	for (char *p = from; *p; p++) if (*p == '@') *p = '_';

	user_t u;
	int idx = users_find(to);
	if (idx < 0 || !users_read(idx, &u) || (u.flags & USER_F_DISABLED)) {
		// not for anyone here: said back -- unless it is itself such a
		// notice, or two nodes could answer each other for ever
		bbs_logf("fed: a letter for %s, who is not here", to);
		if (strcmp(from, "postmaster")) {
			char why[200];
			snprintf(why, sizeof(why), "There is no user called %s at %s.", to, fl_node[0] ? fl_node : "this node");
			bounce_remote(fl_open.origin, from, subject[0] ? subject : "(no subject)", why);
		}
		return;
	}
	if (msg_find(0, fl_open.id, post, BACK)) return;		// had it already

	msg_t m;
	memset(&m, 0, sizeof(m));
	snprintf(m.id, sizeof(m.id), "%s", fl_open.id);
	memcpy(m.post, post, strlen(post) + 1);
	const char *nm = name_of(fl_open.origin);
	char sid[17], both[sizeof(m.from) + 40];
	if (!nm) { memcpy(sid, fl_open.origin, 16); sid[16] = 0; nm = sid; }
	snprintf(both, sizeof(both), "%.40s@%.32s", from, nm);
	utf8_copy(m.from, sizeof(m.from), both);
	utf8_copy(m.to, sizeof(m.to), u.handle);
	m.to_id = u.id;
	utf8_copy(m.subject, sizeof(m.subject), subject[0] ? subject : "(no subject)");
	m.date = date;
	if (strlen(reply) == 64) {
		int parent = msg_find(0, reply, NULL, BACK);
		if (parent > 0) { m.reply = (uint32_t)parent; snprintf(m.reply_id, sizeof(m.reply_id), "%s", reply); }
	}
	int num = msg_post(0, &m, body, (uint32_t)strlen(body));
	if (num < 0) bbs_logf("fed: a letter for %s could not be stored", u.handle);
	else bbs_logf("fed: a letter for %s from %s", u.handle, m.from);
}

static int got_obj(uint32_t pos, const uint8_t *b, uint32_t n) {
	char topic[100], type[70], format[8], origin[70], tm[24], lenstr[12];
	const char *h = (const char *)b;
	const uint8_t *sep = NULL;
	for (uint32_t i = 0; i + 1 < n; i++) if (b[i] == '\n' && b[i + 1] == '\n') { sep = b + i; break; }
	if (!sep) return 1;
	uint32_t hl = (uint32_t)(sep - b) + 1, hdr_end = hl + 1;
	if (!hdr(h, hl, "topic", topic, sizeof(topic)) || !hdr(h, hl, "type", type, sizeof(type)) ||
			!hdr(h, hl, "format", format, sizeof(format)) || !hdr(h, hl, "origin", origin, sizeof(origin)) ||
			!hdr(h, hl, "time", tm, sizeof(tm)) || !hdr(h, hl, "len", lenstr, sizeof(lenstr))) return 1;
	uint32_t plen = (uint32_t)strtoul(lenstr, NULL, 10);
	if (hdr_end + plen > n) return 1;
	// the object's id: SHA-256 of it up to the end of its payload (docs/fed.md)
	uint8_t idb[32];
	char id[65];
	z_sha256(idb, b, hdr_end + plen);
	for (int i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02x", idb[i]);
	const char *payload = h + hdr_end;

	char nodes[40], mail[64];
	snprintf(nodes, sizeof(nodes), "%s/nodes", bbs_fed_network);
	if (!strcmp(topic, nodes) && !strcmp(type, "fed.nodes")) { names_from_list(payload, plen); return 1; }
	// a letter to this node: fed opens it; acknowledged once it is stored
	snprintf(mail, sizeof(mail), "%s/mail/%s", bbs_fed_network, fl_sid);
	if (fl_sid[0] && !strcmp(topic, mail)) {
		if (strcmp(type, "bbs.mail") || strcmp(format, "bytes")) return 1;
		if (msg_find(0, id, NULL, BACK)) return 1;					// had it already
		char l[32];
		int ll = snprintf(l, sizeof(l), "OPEN %u\n", (unsigned)plen);
		if (!fl_room((uint32_t)ll + plen) || fl_qn >= (int)sizeof(fl_q)) return -1;	// when there is room
		fl_put(l, (uint32_t)ll, Q_OPEN);
		fl_put(payload, plen, 0);
		fl_open.on = true;
		fl_open.pos = pos;
		fl_open.time = (uint32_t)strtoul(tm, NULL, 10);
		snprintf(fl_open.id, sizeof(fl_open.id), "%s", id);
		snprintf(fl_open.origin, sizeof(fl_open.origin), "%.64s", origin);	// 64 hex: fed checked it
		return 0;
	}
	// a cancel: for a message here, fed says whether it honoured it
	char cancels[48];
	snprintf(cancels, sizeof(cancels), "%s/cancel", bbs_fed_network);
	if (!strcmp(topic, cancels) && !strcmp(type, "fed.cancel") && !strcmp(format, "json")) {
		char cid[70], ctopic[100];
		int t, k;
		if (zjson_parse(payload, plen, fl_tok, FL_TOKENS, NULL) < 1 || fl_tok[0].type != ZJ_OBJECT ||
				(k = zjson_get(payload, fl_tok, 0, "id")) < 0 || !zjson_str(payload, &fl_tok[k], cid, sizeof(cid)) || strlen(cid) != 64 ||
				(t = zjson_get(payload, fl_tok, 0, "topic")) < 0 || !zjson_str(payload, &fl_tok[t], ctopic, sizeof(ctopic)))
			return 1;
		for (int a = 1; a < bbs_nareas; a++) {
			if (!bbs_area[a].topic[0] || strcmp(bbs_area[a].topic, ctopic)) continue;
			int num = msg_find(a, cid, NULL, BACK);
			if (!num) return 1;								// not here: nothing to do
			char l[90];
			int ll = snprintf(l, sizeof(l), "CANCELLED %s\n", cid);
			if (!fl_room((uint32_t)ll) || fl_qn >= (int)sizeof(fl_q)) return -1;
			fl_put(l, (uint32_t)ll, Q_CANCELLED);
			fl_cx.on = true; fl_cx.pos = pos; fl_cx.area = a; fl_cx.num = num;
			return 0;
		}
		return 1;
	}
	for (int a = 1; a < bbs_nareas; a++) {
		if (!bbs_area[a].topic[0] || strcmp(bbs_area[a].topic, topic)) continue;
		if (strcmp(type, "bbs.post") || strcmp(format, "json")) return 1;
		got_post(a, id, origin, (uint32_t)strtoul(tm, NULL, 10), payload, plen);
		return 1;
	}
	return 1;
}

static void ack(uint32_t pos) {
	char a[32];
	int n = snprintf(a, sizeof(a), "ACK %lu\n", (unsigned long)pos);
	fl_put(a, (uint32_t)n, 0);
}

// A letter or a post fed would not take: said to its writer, in their
// own mailbox -- a post too large for its network, say (fed.md,
// "Network profiles"). A refused cancel has no writer to tell.
static void bounce_local(const char *why) {
	// the record as a string: outbox_get() gives its length, not a NUL
	int rl = outbox_get(0, fl_rec, sizeof(fl_rec) - 1);
	if (rl < 0) return;
	fl_rec[rl] = 0;
	char node[100], from[64], to[64], subject[SUBJECT_MAX * 4 + 1];
	// "MAIL <node> ..." or "PUB <topic> ...": the second word (no sscanf
	// -- on newlib it brings in the whole scanf family, 60 KB of it)
	bool post = !strncmp(fl_rec, "PUB ", 4);
	if (!post && strncmp(fl_rec, "MAIL ", 5)) return;
	int k = 0;
	for (const char *p = fl_rec + (post ? 4 : 5); *p && *p != ' ' && *p != '\n' && k < (int)sizeof(node) - 1; p++) node[k++] = *p;
	node[k] = 0;
	if (!k) return;
	const char *js = strchr(fl_rec, '\n');
	if (!js) return;
	js++;
	int nt = zjson_parse(js, (uint32_t)strlen(js), fl_tok, FL_TOKENS, NULL), t;
	to[0] = 0;
	if (nt < 1 || (t = zjson_get(js, fl_tok, 0, "from")) < 0 || !zjson_str(js, &fl_tok[t], from, sizeof(from))) return;
	if (!post && ((t = zjson_get(js, fl_tok, 0, "to")) < 0 || !zjson_str(js, &fl_tok[t], to, sizeof(to)))) return;
	subject[0] = 0;
	if ((t = zjson_get(js, fl_tok, 0, "subject")) >= 0) zjson_str(js, &fl_tok[t], subject, sizeof(subject));
	user_t u;
	int idx = users_find(from);
	if (idx < 0 || !users_read(idx, &u)) return;
	msg_t m;
	char body[400];
	memset(&m, 0, sizeof(m));
	snprintf(m.from, sizeof(m.from), "postmaster");
	utf8_copy(m.to, sizeof(m.to), u.handle);
	m.to_id = u.id;
	char subj[sizeof(m.subject) + 20];
	snprintf(subj, sizeof(subj), post ? "Not posted: %s" : "Not sent: %s", subject);
	utf8_copy(m.subject, sizeof(m.subject), subj);
	int bl;
	if (post) {
		const char *forum = node;
		for (int a = 1; a < bbs_nareas; a++) if (!strcmp(bbs_area[a].topic, node)) forum = bbs_area[a].name;
		bl = snprintf(body, sizeof(body), "Your post to %s could not be sent to the network:\n%s\n", forum, why);
	} else bl = snprintf(body, sizeof(body), "Your letter to %s@%s could not be sent:\n%s\n", to, node, why);
	msg_post(0, &m, body, (uint32_t)bl);
}

// -- bytes in --

static void reply_line(const char *l) {
	int req = fl_qn ? fl_q[0] : 0;
	if (fl_qn) memmove(fl_q, fl_q + 1, (size_t)--fl_qn);
	bool ok = !strncmp(l, "OK", 2);
	if (req == Q_KEY && ok && strlen(l) >= 67) {
		// OK <key> <short id> <name>
		memcpy(fl_self, l + 3, 64); fl_self[64] = 0;
		if (strlen(l) >= 84 && l[67] == ' ') {
			memcpy(fl_sid, l + 68, 16); fl_sid[16] = 0;
			// this node's name: whole, or not at all -- a cut name is another
			// name, and it decides what is a letter here (mail_to_check())
			const char *nm = l[84] == ' ' && strcmp(l + 85, "-") ? l + 85 : "";
			size_t nl = strlen(nm);
			if (nl < sizeof(fl_node)) memcpy(fl_node, nm, nl + 1); else fl_node[0] = 0;
		}
	}
	else if (req == Q_SUB && !ok) bbs_logf("fed: the subscription was refused: %s", l);
	else if (req == Q_PUB || req == Q_MAIL) {
		if (!ok) {
			bbs_logf("fed: %s was refused: %s", req == Q_MAIL ? "a letter" : "a post", l);
			bounce_local(strncmp(l, "ERR ", 4) ? l : l + 4);
		}
		outbox_pop();						// sent, or refused for good
		if (fl_sent > 0) fl_sent--;
	} else if (req == Q_CANCELLED) {
		if (fl_cx.on) {
			idx_t e;
			if (!strcmp(l, "OK yes") && msg_idx(fl_cx.area, fl_cx.num, &e) && !(e.flags & IDX_DELETED)) {
				e.flags |= IDX_DELETED;
				msg_idx_write(fl_cx.area, fl_cx.num, &e);
				bbs_logf("fed: %s #%d cancelled on the network -- deleted here", bbs_area[fl_cx.area].tag, fl_cx.num);
			}
			ack(fl_cx.pos);
			fl_cx.on = false;
		}
	} else if (req == Q_OPEN) {
		bbs_logf("fed: a letter fed would not open: %s", l);
		if (fl_open.on) { ack(fl_open.pos); fl_open.on = false; }
	}
}

// -- the interface the platform uses --

const char *bbs_fed_target(void) {
	return bbs_fed_network[0] ? bbs_cfg.fed : "";
}

void bbs_fed_up(bool up) {
	fl_up = up;
	fl_in_len = 0;
	fl_out_head = fl_out_len = 0;
	fl_qn = 0;
	fl_sent = 0;
	fl_open.on = false;				// fed sends it again: it was never acknowledged
	fl_cx.on = false;
	if (!up) return;
	char sub[80];
	fl_put("KEY\n", 4, Q_KEY);
	int n = snprintf(sub, sizeof(sub), "SUB bbs %s/*\n", bbs_fed_network);
	fl_put(sub, (uint32_t)n, Q_SUB);
	outbox_send();
	bbs_logf("fed: connected (%d posts waiting)", outbox_count());
}

void bbs_fed_input(const uint8_t *d, uint32_t n) {
	if (fl_in_len + n > sizeof(fl_in)) { bbs_logf("fed: more than an object at once -- the link is reset"); bbs_fed_up(false); return; }
	if (n) memcpy(fl_in + fl_in_len, d, n);
	fl_in_len += n;
	for (;;) {
		uint8_t *nl = memchr(fl_in, '\n', fl_in_len);
		if (!nl) return;
		uint32_t ll = (uint32_t)(nl - fl_in);
		char line[160];
		if (ll >= sizeof(line)) { bbs_logf("fed: a line too long -- the link is reset"); bbs_fed_up(false); return; }
		memcpy(line, fl_in, ll);
		line[ll] = 0;
		uint32_t used = ll + 1;
		if (!strncmp(line, "OBJ ", 4)) {
			char *e;
			unsigned long pos = strtoul(line + 4, &e, 10), len = strtoul(e, NULL, 10);
			if (len > FL_IN_MAX - 200) { bbs_logf("fed: an object too big -- the link is reset"); bbs_fed_up(false); return; }
			if (fl_in_len < used + len) return;			// the rest is on its way
			int r = got_obj((uint32_t)pos, fl_in + used, (uint32_t)len);
			if (r < 0) return;					// no room to OPEN it yet: kept, tried again as output drains
			used += (uint32_t)len;
			if (r > 0) ack((uint32_t)pos);
		} else if (fl_qn && fl_q[0] == Q_OPEN && !strncmp(line, "OK ", 3)) {
			// the opened letter follows its line
			uint32_t ln = (uint32_t)strtoul(line + 3, NULL, 10);
			if (ln > FL_IN_MAX - 200) { bbs_logf("fed: an opened letter too big -- the link is reset"); bbs_fed_up(false); return; }
			if (fl_in_len < used + ln) return;
			memmove(fl_q, fl_q + 1, (size_t)--fl_qn);
			got_letter((const char *)fl_in + used, ln);
			used += ln;
			if (fl_open.on) { ack(fl_open.pos); fl_open.on = false; }
		} else {
			reply_line(line);
		}
		memmove(fl_in, fl_in + used, fl_in_len - used);
		fl_in_len -= used;
		outbox_send();
	}
}

uint32_t bbs_fed_output(const uint8_t **p) {
	*p = fl_out + fl_out_head;
	return fl_out_len;
}

void bbs_fed_consumed(uint32_t n) {
	if (n > fl_out_len) n = fl_out_len;
	fl_out_head += n;
	fl_out_len -= n;
	if (!fl_out_len) fl_out_head = 0;
	if (fl_in_len && n) bbs_fed_input(NULL, 0);		// a letter kept for want of room
}

// -- from the core: a post to a federated forum --

// JSON string escaping of up to `len` bytes: the quotes, the backslash,
// controls as \n or \u00XX.
static uint32_t jstr(char *o, uint32_t cap, const char *s, uint32_t len) {
	uint32_t n = 0;
	if (cap < 3) return 0;
	o[n++] = '"';
	for (uint32_t i = 0; i < len && s[i] && n + 8 < cap; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
		else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
		else if (c < 0x20) n += (uint32_t)snprintf(o + n, cap - n, "\\u%04x", c);
		else o[n++] = (char)c;
	}
	o[n++] = '"';
	return n;
}

// A post's or a letter's JSON into js: its length, or 0 if it does not fit.
static uint32_t build_json(char *js, uint32_t cap, const char *from, const char *to, const char *subject,
		const char *reply_id, const char *body, uint32_t blen) {
	uint8_t r[12];
	char token[25];
	uint32_t o = 0;
	plat_random(r, sizeof(r));
	for (int i = 0; i < 12; i++) snprintf(token + 2 * i, 3, "%02x", r[i]);
	o += (uint32_t)snprintf(js + o, cap - o, "{\"from\":");
	o += jstr(js + o, cap - o, from, (uint32_t)strlen(from));
	if (to) { o += (uint32_t)snprintf(js + o, cap - o, ",\"to\":"); o += jstr(js + o, cap - o, to, (uint32_t)strlen(to)); }
	o += (uint32_t)snprintf(js + o, cap - o, ",\"subject\":");
	o += jstr(js + o, cap - o, subject, (uint32_t)strlen(subject));
	o += (uint32_t)snprintf(js + o, cap - o, ",\"date\":%lu", (unsigned long)plat_now());
	if (reply_id && strlen(reply_id) == 64) o += (uint32_t)snprintf(js + o, cap - o, ",\"reply\":\"%s\"", reply_id);
	o += (uint32_t)snprintf(js + o, cap - o, ",\"post\":\"%s\",\"body\":", token);
	o += jstr(js + o, cap - o, body, blen);
	if (o + 2 >= cap) return 0;
	js[o++] = '}';
	return o;
}

// "<line>\n<json>" into fl_rec, into the outbox, and sent if it can be.
static int queue_request(const char *line_fmt, const char *arg, uint32_t jl) {
	enum { HEAD = 200 };
	char head[HEAD];
	int hl = snprintf(head, sizeof(head), line_fmt, arg, (unsigned)jl);
	if (hl <= 0 || hl >= HEAD) return -1;
	memmove(fl_rec + hl, fl_rec + HEAD, jl);
	memcpy(fl_rec, head, (size_t)hl);
	if (!outbox_add(fl_rec, (uint32_t)hl + jl)) return -1;
	outbox_send();
	return fl_up ? 1 : 0;				// 1: on its way; 0: waiting for fed
}

int fed_post(int a, const msg_t *m, const char *body, uint32_t blen) {
	enum { HEAD = 200 };
	if (blen > MSG_BODY_MAX) blen = MSG_BODY_MAX;
	uint32_t jl = build_json(fl_rec + HEAD, FL_OUT_MAX - HEAD - 1, m->from, NULL, m->subject, m->reply_id, body, blen);
	if (!jl) return -1;
	return queue_request("PUB %s bbs.post json log - %u\n", bbs_area[a].topic, jl);
}

// A letter to handle@node: fed seals it to that node (docs/fed.md, "Mail
// between nodes") -- MAIL, through the outbox like a post.
int fed_mail(const msg_t *m, const char *body, uint32_t blen) {
	enum { HEAD = 200 };
	char handle[USER_HANDLE_BYTES], node[40];
	const char *at = strchr(m->to, '@');
	if (!at) return -1;
	snprintf(handle, sizeof(handle), "%.*s", (int)(at - m->to), m->to);
	snprintf(node, sizeof(node), "%s", at + 1);
	if (blen > MSG_BODY_MAX) blen = MSG_BODY_MAX;
	uint32_t jl = build_json(fl_rec + HEAD, FL_OUT_MAX - HEAD - 1, m->from, handle, m->subject, m->reply_id, body, blen);
	if (!jl) return -1;
	// name@network: the name as THIS network's list gives it -- never a
	// same name in another list fed follows (fed.md, "Names")
	char addr[80];
	snprintf(addr, sizeof(addr), "%s@%s", node, bbs_fed_network);
	return queue_request("MAIL %s bbs.mail %u\n", addr, jl);
}

// A cancel for a message in federated forum a, by its object id.
int fed_cancel(int a, const char *id) {
	enum { HEAD = 200 };
	char topic[48];
	if (!bbs_area[a].topic[0] || strlen(id) != 64) return -1;
	int jl = snprintf(fl_rec + HEAD, FL_OUT_MAX - HEAD, "{\"id\":\"%s\",\"topic\":\"%s\"}", id, bbs_area[a].topic);
	snprintf(topic, sizeof(topic), "%s/cancel", bbs_fed_network);
	return queue_request("PUB %s fed.cancel json log - %u\n", topic, (uint32_t)jl);
}

// A letter from the postmaster, to a node by key: what could not be delivered.
static void bounce_remote(const char *origin, const char *to, const char *subject, const char *why) {
	enum { HEAD = 200 };
	char subj[SUBJECT_MAX * 4 + 20];
	snprintf(subj, sizeof(subj), "Not delivered: %s", subject);
	uint32_t jl = build_json(fl_rec + HEAD, FL_OUT_MAX - HEAD - 1, "postmaster", to, subj, NULL, why, (uint32_t)strlen(why));
	if (jl) queue_request("MAIL %s bbs.mail %u\n", origin, jl);
}

bool fed_node_known(const char *name) {
	for (int i = 0; i < fl_nnames; i++) if (!strcmp(fl_names[i].name, name)) return true;
	return false;
}

const char *fed_node_name(void) { return fl_node; }

void fed_init(void) {
	names_load();
	fl_self[0] = 0;
	fl_up = false;
}
