/*
 * Zeitlos
 *
 * DNS-SD browsing by legacy-unicast multicast DNS -- see mdns.h for
 * what and why, docs/networking.md ("Finding services") for the user's
 * side.
 *
 * A browse:
 *
 *   0 ms     PTR questions for each service type, to 224.0.0.251:5353
 *   300 ms   again (a lost packet), plus the follow-ups below
 *   600 ms,  follow-ups: SRV and TXT for an instance that has not said
 *   900 ms   where it is, A for a host whose address has not come
 *   1200 ms  done, if every instance found is complete
 *   1800 ms  done regardless: the complete ones are the answer
 *
 * Most responders put the SRV, TXT and A records in the same answer as
 * the PTR (RFC 6763 section 12), so the follow-ups usually ask nothing.
 *
 * Names are kept in wire form -- length-prefixed labels, uncompressed
 * -- because an instance name's first label may hold dots ("Canon
 * MG3600 series" is plain, "HP LaserJet 400 (1.2.3)" is not), and the
 * dotted text form cannot say where such a label ends.
 *
 * The answer, to the requester: Z_MAP { "ok": 1, "found": Z_STR }, a
 * line per instance:
 *
 *   SERVICE \t NAME \t A.B.C.D \t PORT [\t TXT-STRING]... \n
 *
 * or { "ok": 0, "error": Z_STR }. Tabs and newlines inside a name or a
 * TXT string are spaces, so the fields split cleanly.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mdns.h"
#include "udp.h"
#include "ip.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ
#include "../../common/znet.h"

#define MDNS_IP         0xE00000FBu		// 224.0.0.251
#define MDNS_PORT       5353

// Sized for net's RAM budget (Makefile, CORE_BUDGETS): about 3KB in all.
#define MAX_SVCS        3
#define MAX_ENTS        6
#define NAME_MAX        96		// wire form: a 63-byte instance label and "._ipp._tcp.local"
#define HOST_MAX        64		// wire form: "Canon-MG3600.local"
#define TXT_MAX         80		// the TXT strings kept (txt_keep())
#define PKT_MAX         384		// what we send: a few questions
#define FOUND_MAX       1024

#define T_A     1
#define T_PTR   12
#define T_TXT   16
#define T_SRV   33

typedef struct {
	uint8_t name[NAME_MAX];
	uint8_t len;
} wname_t;

typedef struct {
	uint8_t name[HOST_MAX];
	uint8_t len;
} whost_t;

typedef struct {
	uint8_t svc;			// which of svcs[]
	wname_t inst;			// "Name._ipp._tcp.local"
	whost_t target;			// "host.local", from SRV
	uint16_t port;
	bool have_srv, have_txt;
	uint32_t ip;
	char txt[TXT_MAX];		// the TXT strings, tab-joined
} ent_t;

static struct {
	bool busy;
	uint32_t pid, tag;
	uint32_t t0;
	uint8_t step;			// the next of the schedule's sends
	uint16_t qid, port;
	wname_t svcs[MAX_SVCS];
	int nsvcs;
	ent_t ents[MAX_ENTS];
	int nents;
} b;

// -- names --

static char lower(uint8_t c) { return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

static bool bytes_eq(const uint8_t *a, int al, const uint8_t *c, int cl) {
	if (al != cl) return false;
	for (int i = 0; i < al; i++)
		if (lower(a[i]) != lower(c[i])) return false;
	return true;
}

static bool name_eq(const wname_t *a, const wname_t *c) { return bytes_eq(a->name, a->len, c->name, c->len); }

// "_ipp._tcp" -> wire "_ipp._tcp.local". False if it does not fit.
static bool name_from_text(wname_t *w, const char *s, int n) {
	static const char local[] = "local";
	int o = 0;
	char t[NAME_MAX];
	if (n <= 0 || n + 7 > (int)sizeof(t)) return false;
	memcpy(t, s, (size_t)n);
	t[n] = '.';
	memcpy(t + n + 1, local, sizeof(local));		// with its NUL
	for (const char *p = t; *p; ) {
		const char *e = p;
		while (*e && *e != '.') e++;
		int l = (int)(e - p);
		if (l < 1 || l > 63 || o + 1 + l + 1 > NAME_MAX) return false;
		w->name[o++] = (uint8_t)l;
		memcpy(w->name + o, p, (size_t)l);
		o += l;
		p = *e ? e + 1 : e;
	}
	w->name[o++] = 0;
	w->len = (uint8_t)o;
	return true;
}

// The name at p[off], compression followed, into w. The offset just
// past the name where it began, or 0 if it is malformed or too long.
static uint16_t read_name(const uint8_t *p, uint16_t len, uint16_t off, wname_t *w) {
	uint16_t end = 0;
	int hops = 0, o = 0;
	for (;;) {
		if (off >= len) return 0;
		uint8_t l = p[off];
		if ((l & 0xC0) == 0xC0) {
			if (off + 1 >= len || ++hops > 16) return 0;
			if (!end) end = off + 2;
			off = (uint16_t)(((l & 0x3F) << 8) | p[off + 1]);
			continue;
		}
		if (l & 0xC0) return 0;
		if (o + 1 + l > NAME_MAX) return 0;
		w->name[o++] = l;
		if (!l) break;
		if ((uint32_t)off + 1 + l > len) return 0;
		memcpy(w->name + o, p + off + 1, l);
		o += l;
		off = (uint16_t)(off + 1 + l);
	}
	w->len = (uint8_t)o;
	return end ? end : (uint16_t)(off + 1);
}

// does `full` end with `suffix` (both wire form, whole labels)?
static bool name_ends(const wname_t *full, const wname_t *suffix) {
	int o = 0;
	while (o < full->len) {
		if (full->len - o == suffix->len) {
			wname_t t;
			t.len = suffix->len;
			memcpy(t.name, full->name + o, suffix->len);
			return name_eq(&t, suffix);
		}
		if (!full->name[o]) break;
		o += 1 + full->name[o];
	}
	return false;
}

// -- the answer --

static ent_t *ent_by_inst(const wname_t *n) {
	for (int i = 0; i < b.nents; i++) if (name_eq(&b.ents[i].inst, n)) return &b.ents[i];
	return NULL;
}

// The TXT strings worth their room: what a printer's listing shows and
// its address needs (zbrowse.c). The rest -- pdl=, URF=, Color= ... --
// run to hundreds of bytes and say nothing a list of printers uses.
static bool txt_keep(const uint8_t *s, int n) {
	static const char *const keys[] = { "rp=", "ty=", "product=", "note=" };
	for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		int kl = (int)strlen(keys[i]);
		if (n >= kl && bytes_eq(s, kl, (const uint8_t *)keys[i], kl)) return true;
	}
	return false;
}

static void txt_add(char *d, const uint8_t *s, int n) {
	int k = (int)strlen(d);
	if (!txt_keep(s, n)) return;
	if (k && k < TXT_MAX - 1) d[k++] = '\t';
	for (int i = 0; i < n && k < TXT_MAX - 1; i++) {
		uint8_t c = s[i];
		if (c == '\t' || c == '\n' || c == '\r') c = ' ';
		if (c < 32) continue;
		d[k++] = (char)c;
	}
	d[k] = 0;
}

// One record set of a response: PTR in pass 0, SRV and TXT in pass 1, A
// in pass 2 -- so the order a responder lists them in does not matter.
static void parse_pass(const uint8_t *p, uint16_t len, int pass) {
	uint16_t qd = (uint16_t)((p[4] << 8) | p[5]);
	uint16_t rr = (uint16_t)(((p[6] << 8) | p[7]) + ((p[8] << 8) | p[9]) + ((p[10] << 8) | p[11]));
	uint16_t off = 12;
	wname_t n, r;
	for (uint16_t q = 0; q < qd; q++) {
		off = read_name(p, len, off, &n);
		if (!off || (uint32_t)off + 4 > len) return;
		off += 4;
	}
	for (uint16_t i = 0; i < rr; i++) {
		off = read_name(p, len, off, &n);
		if (!off || (uint32_t)off + 10 > len) return;
		uint16_t type = (uint16_t)((p[off] << 8) | p[off + 1]);
		uint16_t cls = (uint16_t)(((p[off + 2] << 8) | p[off + 3]) & 0x7FFF);	// less cache-flush
		uint16_t rdlen = (uint16_t)((p[off + 8] << 8) | p[off + 9]);
		uint16_t rd = (uint16_t)(off + 10);
		if ((uint32_t)rd + rdlen > len) return;
		off = (uint16_t)(rd + rdlen);
		if (cls != 1) continue;
		if (pass == 0 && type == T_PTR) {
			for (int s = 0; s < b.nsvcs; s++) {
				if (!name_eq(&n, &b.svcs[s])) continue;
				if (!read_name(p, len, rd, &r) || !name_ends(&r, &b.svcs[s])) break;
				if (r.len == b.svcs[s].len) break;		// no instance label
				if (!ent_by_inst(&r) && b.nents < MAX_ENTS) {
					ent_t *e = &b.ents[b.nents++];
					memset(e, 0, sizeof(*e));
					e->svc = (uint8_t)s;
					e->inst = r;
				}
				break;
			}
		} else if (pass == 1 && type == T_SRV && rdlen > 6) {
			ent_t *e = ent_by_inst(&n);
			if (!e || !read_name(p, len, (uint16_t)(rd + 6), &r) || r.len > HOST_MAX) continue;
			e->port = (uint16_t)((p[rd + 4] << 8) | p[rd + 5]);
			if (!e->have_srv || !bytes_eq(e->target.name, e->target.len, r.name, r.len)) e->ip = 0;
			memcpy(e->target.name, r.name, r.len);
			e->target.len = r.len;
			e->have_srv = true;
		} else if (pass == 1 && type == T_TXT) {
			ent_t *e = ent_by_inst(&n);
			if (!e || e->have_txt) continue;
			e->txt[0] = 0;
			for (uint16_t k = 0; k < rdlen; ) {
				uint8_t l = p[rd + k];
				if (k + 1 + l > rdlen) break;
				if (l) txt_add(e->txt, p + rd + k + 1, l);
				k = (uint16_t)(k + 1 + l);
			}
			e->have_txt = true;
		} else if (pass == 2 && type == T_A && rdlen == 4) {
			uint32_t ip = ((uint32_t)p[rd] << 24) | ((uint32_t)p[rd + 1] << 16) |
				((uint32_t)p[rd + 2] << 8) | p[rd + 3];
			for (int k = 0; k < b.nents; k++)
				if (b.ents[k].have_srv && bytes_eq(b.ents[k].target.name, b.ents[k].target.len, n.name, n.len))
					b.ents[k].ip = ip;
		}
	}
}

static void on_reply(uint32_t src_ip, uint16_t src_port, const uint8_t *p, uint16_t len) {
	(void)src_ip;
	if (!b.busy || src_port != MDNS_PORT || len < 12) return;
	if (((p[0] << 8) | p[1]) != b.qid) return;	// legacy unicast answers echo the id
	if (!(p[2] & 0x80)) return;			// a query, not an answer
	for (int pass = 0; pass < 3; pass++) parse_pass(p, len, pass);
}

// -- asking --

static uint8_t pkt[PKT_MAX];
static int pkt_len, pkt_qd;

static void q_begin(void) {
	memset(pkt, 0, 12);
	pkt[0] = (uint8_t)(b.qid >> 8);
	pkt[1] = (uint8_t)b.qid;
	pkt_len = 12;
	pkt_qd = 0;
}

static void q_add(const uint8_t *name, int nlen, uint16_t type) {
	if (pkt_len + nlen + 4 > PKT_MAX) return;		// the rest at the next send
	memcpy(pkt + pkt_len, name, (size_t)nlen);
	pkt_len += nlen;
	pkt[pkt_len++] = (uint8_t)(type >> 8);
	pkt[pkt_len++] = (uint8_t)type;
	pkt[pkt_len++] = 0;
	pkt[pkt_len++] = 1;				// IN
	pkt_qd++;
}

static void q_send(void) {
	if (!pkt_qd) return;
	pkt[4] = (uint8_t)(pkt_qd >> 8);
	pkt[5] = (uint8_t)pkt_qd;
	udp_send(MDNS_IP, MDNS_PORT, b.port, pkt, (uint16_t)pkt_len);
}

static bool complete(const ent_t *e) { return e->have_srv && e->have_txt && e->ip; }

static void send_step(bool ptr) {
	q_begin();
	if (ptr) for (int s = 0; s < b.nsvcs; s++) q_add(b.svcs[s].name, b.svcs[s].len, T_PTR);
	for (int i = 0; i < b.nents; i++) {
		ent_t *e = &b.ents[i];
		if (!e->have_srv) q_add(e->inst.name, e->inst.len, T_SRV);
		if (!e->have_txt) q_add(e->inst.name, e->inst.len, T_TXT);
		if (e->have_srv && !e->ip) q_add(e->target.name, e->target.len, T_A);
	}
	q_send();
}

// -- replying --

typedef struct {
	z_obj_table_t t;
	z_obj_t k[2], v[2];
	char text[FOUND_MAX];
} reply_t;

// A reply is borrowed by its receiver (dns.c's reply_ring,
// docs/messaging.md). One is enough: the next answer with a list is
// written two seconds after the next browse begins, long after the last
// was read. A "busy" refusal has a buffer of its own.
static reply_t found_reply;
static struct { z_obj_table_t t; z_obj_t k[2], v[2]; } busy_reply;

static int cat(char *d, int k, const char *s) {
	while (*s && k < FOUND_MAX - 1) d[k++] = *s++;
	d[k] = 0;
	return k;
}

static int cat_dec(char *d, int k, uint32_t v) {
	char t[12];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n && k < FOUND_MAX - 1) d[k++] = t[--n];
	d[k] = 0;
	return k;
}

// a wire name as text: its labels joined by '.', the last ("local") left off
static int cat_name(char *d, int k, const wname_t *w, int labels) {
	int o = 0;
	for (int i = 0; i < labels && o < w->len && w->name[o]; i++) {
		int l = w->name[o];
		if (i) k = cat(d, k, ".");
		for (int j = 0; j < l && k < FOUND_MAX - 1; j++) {
			char c = (char)w->name[o + 1 + j];
			d[k++] = c == '\t' || c == '\n' || c == '\r' ? ' ' : c;
		}
		d[k] = 0;
		o += 1 + l;
	}
	return k;
}

static int labels(const wname_t *w) {
	int n = 0;
	for (int o = 0; o < w->len && w->name[o]; o += 1 + w->name[o]) n++;
	return n;
}

static void reply(uint32_t pid, uint32_t tag, const char *err) {
	reply_t *r = &found_reply;
	if (err && b.busy) {
		// refusing a second search while one runs: not the running one's buffer
		busy_reply.t.len = 2;
		busy_reply.t.a = busy_reply.k;
		busy_reply.t.b = busy_reply.v;
		busy_reply.k[0].type = Z_STR;	busy_reply.k[0].val.str = (char *)"ok";
		busy_reply.v[0].type = Z_UINT32;	busy_reply.v[0].val.uint32 = 0;
		busy_reply.k[1].type = Z_STR;	busy_reply.k[1].val.str = (char *)"error";
		busy_reply.v[1].type = Z_STR;	busy_reply.v[1].val.str = (char *)err;
		z_obj_t o;
		o.type = Z_MAP;
		o.val.ptr = &busy_reply.t;
		z_msg_new_send(pid, Z_NET_BROWSE_REPLY, tag, o);
		return;
	}
	r->t.len = 2;
	r->t.a = r->k;
	r->t.b = r->v;
	r->k[0].type = Z_STR;		r->k[0].val.str = (char *)"ok";
	r->v[0].type = Z_UINT32;	r->v[0].val.uint32 = err ? 0 : 1;
	r->k[1].type = Z_STR;		r->k[1].val.str = (char *)(err ? "error" : "found");
	r->v[1].type = Z_STR;
	if (err) {
		r->v[1].val.str = (char *)err;
	} else {
		int k = 0, n = 0;
		r->text[0] = 0;
		for (int i = 0; i < b.nents; i++) {
			ent_t *e = &b.ents[i];
			if (!e->have_srv || !e->ip) continue;		// TXT is optional in practice
			int start = k;
			k = cat_name(r->text, k, &b.svcs[e->svc], 2);
			k = cat(r->text, k, "\t");
			// the labels before the service type: one, from a careful
			// responder, but some split a name with dots in it
			k = cat_name(r->text, k, &e->inst, labels(&e->inst) - labels(&b.svcs[e->svc]));
			k = cat(r->text, k, "\t");
			for (int s = 24; s >= 0; s -= 8) {
				k = cat_dec(r->text, k, (e->ip >> s) & 0xFF);
				if (s) k = cat(r->text, k, ".");
			}
			k = cat(r->text, k, "\t");
			k = cat_dec(r->text, k, e->port);
			if (e->txt[0]) {
				k = cat(r->text, k, "\t");
				k = cat(r->text, k, e->txt);
			}
			if (k >= FOUND_MAX - 2) {				// a cut line is worse than none
				k = start;
				r->text[k] = 0;
				break;
			}
			k = cat(r->text, k, "\n");
			n++;
		}
		r->v[1].val.str = r->text;
		printf("net: mdns: %d found\n", n);
	}
	z_obj_t obj;
	obj.type = Z_MAP;
	obj.val.ptr = &r->t;
	z_msg_new_send(pid, Z_NET_BROWSE_REPLY, tag, obj);
}

static void finish(const char *err) {
	if (b.busy) udp_close(b.port);
	b.busy = false;
	reply(b.pid, b.tag, err);
}

void mdns_browse_start(const char *services, uint32_t pid, uint32_t tag) {
	if (b.busy) {
		reply(pid, tag, "busy: another search is running");
		return;
	}
	b.pid = pid;
	b.tag = tag;
	b.nsvcs = 0;
	b.nents = 0;
	for (const char *s = services ? services : ""; *s; ) {
		while (*s == ' ') s++;
		const char *e = s;
		while (*e && *e != ' ') e++;
		if (e > s && b.nsvcs < MAX_SVCS && !name_from_text(&b.svcs[b.nsvcs++], s, (int)(e - s))) {
			finish("not a service type");
			return;
		}
		s = e;
	}
	if (!b.nsvcs) { finish("no service type given"); return; }
	if (!ip_our_addr()) { finish("no network address yet"); return; }
	static uint16_t n;
	b.port = (uint16_t)(61000 + n++ % 1000);
	b.qid = (uint16_t)(z_uptime_ticks() * 2654435761u >> 8);
	b.busy = true;
	b.t0 = z_uptime_ticks();
	b.step = 1;
	udp_listen(b.port, on_reply);
	send_step(true);
}

void mdns_poll(void) {
	if (!b.busy) return;
	uint32_t ms = (z_uptime_ticks() - b.t0) * 1000u / Z_TICK_HZ;
	if (b.step <= 3 && ms >= 300u * b.step) {
		send_step(b.step == 1);
		b.step++;
	}
	bool all = true;
	for (int i = 0; i < b.nents; i++) if (!complete(&b.ents[i])) all = false;
	if ((ms >= 1200 && all) || ms >= 1800) finish(NULL);
}
