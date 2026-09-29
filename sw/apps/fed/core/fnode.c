/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A zfed node, portable. docs/fed.md; the interface in fnode.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "fnode.h"
#include "fstore.h"
#include "fobj.h"
#include "fnet.h"
#include "fmail.h"
#include "fradio.h"
#include "../../../common/zsha256.h"
#include "../../../common/zjson.h"
#include "../../../common/zplat.h"
#include "../../../ext/monocypher/monocypher.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

static fcfg_t g_cfg;
static char g_dir[160];
static uint8_t g_sk[64], g_pk[32];
static fmail_keys_t g_mail;			// derived from the seed: fmail.h
// Shared, since no two of their users run at once: a stored object read
// back (node info, a list, a cancel's target), and a letter sealed or
// opened. ~33 KB, not ~99.
static uint8_t g_mobj[FOBJ_MAX];
static uint8_t g_mletter[FOBJ_PAYLOAD_MAX];
static uint32_t g_now_ms;
static uint32_t g_expire_ms;

const fcfg_t *fnode_cfg(void) { return &g_cfg; }
const uint8_t *fnode_public_key(void) { return g_pk; }

// -- configuration --

static bool hexkey(const char *s, uint8_t out[32]) {
	for (int i = 0; i < 32; i++) {
		unsigned v;
		if (!isxdigit((unsigned char)s[2*i]) || !isxdigit((unsigned char)s[2*i+1]) || sscanf(s + 2*i, "%2x", &v) != 1) return false;
		out[i] = (uint8_t)v;
	}
	return s[64] == 0 || s[64] == ' ' || s[64] == '\t';
}

// "90d", "12h", "30m", "45s" (a bare number is days)
static bool duration(const char *s, uint32_t *secs) {
	char *e;
	unsigned long v = strtoul(s, &e, 10);
	if (e == s) return false;
	unsigned long mul = 86400;
	if (*e == 'h') mul = 3600; else if (*e == 'm') mul = 60; else if (*e == 's') mul = 1;
	else if (*e && *e != 'd') return false;
	*secs = (uint32_t)(v * mul);
	return true;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	size_t n = strlen(s);
	while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
	return s;
}

bool fnode_config(const char *text, fcfg_t *c, char *err, int errlen) {
	static char buf[8192];
	int lineno = 0;
	memset(c, 0, sizeof(*c));
	c->poll_s = 15 * 60;
	c->retain_default = 90 * 86400;
	snprintf(buf, sizeof(buf), "%s", text);
	for (char *line = buf, *next; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		lineno++;
		char *h = strchr(line, '#');
		if (h) *h = 0;
		char *s = trim(line);
		if (!*s) continue;
		char *colon = strchr(s, ':');
		if (!colon) { snprintf(err, (size_t)errlen, "line %d: no ':'", lineno); return false; }
		*colon = 0;
		char *k = trim(s), *v = trim(colon + 1);
		if (!strcmp(k, "name")) snprintf(c->name, sizeof(c->name), "%s", v);
		else if (!strcmp(k, "listen")) c->listen = (uint16_t)atoi(v);
		else if (!strcmp(k, "poll_minutes")) c->poll_s = (uint32_t)atoi(v) * 60;
		else if (!strcmp(k, "poll_seconds")) c->poll_s = (uint32_t)atoi(v);			// for tests
		else if (!strcmp(k, "network")) {
			// network: <name> <publisher key>
			char *sp = strchr(v, ' ');
			if (c->nnetworks >= FNODE_NETWORKS) { snprintf(err, (size_t)errlen, "line %d: too many networks", lineno); return false; }
			if (!sp) { snprintf(err, (size_t)errlen, "line %d: network: <name> <publisher key>", lineno); return false; }
			*sp = 0;
			char *pk = trim(sp + 1);
			if (!fnet_name_ok(v)) { snprintf(err, (size_t)errlen, "line %d: a network name is 1-32 bytes of a-z 0-9 -", lineno); return false; }
			if (!hexkey(pk, c->networks[c->nnetworks].publisher)) { snprintf(err, (size_t)errlen, "line %d: not a 64-hex-digit key", lineno); return false; }
			// one network, one name: topics are named by it, so two with one
			// name would compete for the same topics -- and whichever list came
			// first would decide who is a member. Following two networks that
			// share a name is not possible; the sysop chooses one.
			for (int j = 0; j < c->nnetworks; j++)
				if (!strcmp(c->networks[j].name, v)) { snprintf(err, (size_t)errlen, "line %d: network %s is here already", lineno, v); return false; }
			snprintf(c->networks[c->nnetworks].name, sizeof(c->networks[0].name), "%s", v);
			c->nnetworks++;
		}
		else if (!strcmp(k, "radio")) {
			// radio: port=300 channel=1 max=2048 pace=2000
			c->radio = true;
			c->radio_port = 300; c->radio_channel = 0; c->radio_max = 2048; c->radio_pace_ms = 2000;
			for (char *w = strtok(v, " \t"); w; w = strtok(NULL, " \t")) {
				char *eq = strchr(w, '=');
				long n = eq ? atol(eq + 1) : -1;
				if (!eq) { snprintf(err, (size_t)errlen, "line %d: radio: key=value words", lineno); return false; }
				*eq = 0;
				if (!strcmp(w, "port") && n >= 256 && n <= 65535) c->radio_port = (uint32_t)n;
				else if (!strcmp(w, "channel") && n >= 0 && n <= 7) c->radio_channel = (uint8_t)n;
				else if (!strcmp(w, "max") && n >= 512 && n <= FRADIO_MAX) c->radio_max = (uint32_t)n;
				else if (!strcmp(w, "pace") && n >= 1 && n <= 600000) c->radio_pace_ms = (uint32_t)n;
				else { snprintf(err, (size_t)errlen, "line %d: radio: port=256-65535 channel=0-7 max=512-%d pace=ms", lineno, FRADIO_MAX); return false; }
			}
		}
		else if (!strcmp(k, "peer") || !strcmp(k, "member") || !strcmp(k, "block")) {
			uint8_t key[32];
			if (!hexkey(v, key)) { snprintf(err, (size_t)errlen, "line %d: not a 64-hex-digit key", lineno); return false; }
			if (!strcmp(k, "member")) {
				if (c->nmembers >= FNODE_MEMBERS) { snprintf(err, (size_t)errlen, "line %d: too many members", lineno); return false; }
				memcpy(c->members[c->nmembers++], key, 32);
			} else if (!strcmp(k, "block")) {
				if (c->nblocked >= FNODE_BLOCKED) { snprintf(err, (size_t)errlen, "line %d: too many blocks", lineno); return false; }
				memcpy(c->blocked[c->nblocked++], key, 32);
			} else {
				if (c->npeers >= FNODE_PEERS) { snprintf(err, (size_t)errlen, "line %d: too many peers", lineno); return false; }
				fpeer_t *p = &c->peers[c->npeers++];
				memset(p, 0, sizeof(*p));
				memcpy(p->key, key, 32);
				for (char *w = strtok(v + 64, " \t"); w; w = strtok(NULL, " \t")) {
					if (!strcmp(w, "trusted-relay")) p->trusted = true;
					else if (!strncmp(w, "max=", 4)) {
						// this link's limit: objects larger are not sent over it (fed.md, "Network profiles")
						long m = atol(w + 4);
						if (m < 512 || m > FOBJ_MAX) { snprintf(err, (size_t)errlen, "line %d: max= is 512-%d bytes", lineno, FOBJ_MAX); return false; }
						p->max_object = (uint32_t)m;
					} else {
						char *pc = strrchr(w, ':');
						if (!pc || pc == w) { snprintf(err, (size_t)errlen, "line %d: not host:port", lineno); return false; }
						*pc = 0;
						snprintf(p->host, sizeof(p->host), "%s", w);
						p->port = (uint16_t)atoi(pc + 1);
						if (!p->port) { snprintf(err, (size_t)errlen, "line %d: no port", lineno); return false; }
					}
				}
			}
		} else if (!strcmp(k, "subscribe")) {
			size_t o = strlen(c->wants);
			for (char *w = strtok(v, ";"); w; w = strtok(NULL, ";")) {
				char *t = trim(w);
				if (!*t) continue;
				if (o + strlen(t) + 2 >= sizeof(c->wants)) { snprintf(err, (size_t)errlen, "line %d: too many patterns", lineno); return false; }
				o += (size_t)snprintf(c->wants + o, sizeof(c->wants) - o, "%s%s", o ? "\n" : "", t);
			}
		} else if (!strcmp(k, "retain")) {
			for (char *w = strtok(v, ";"); w; w = strtok(NULL, ";")) {
				char *t = trim(w), *eq = strchr(t, '=');
				uint32_t secs;
				if (!eq) {
					if (!duration(t, &secs)) { snprintf(err, (size_t)errlen, "line %d: not a duration", lineno); return false; }
					c->retain_default = secs;
				} else {
					*eq = 0;
					if (c->nretain >= FNODE_RETAIN || !duration(eq + 1, &secs)) { snprintf(err, (size_t)errlen, "line %d: bad retain", lineno); return false; }
					snprintf(c->retain[c->nretain].pat, sizeof(c->retain[0].pat), "%s", trim(t));
					c->retain[c->nretain++].secs = secs;
				}
			}
		} else { snprintf(err, (size_t)errlen, "line %d: unknown key '%s'", lineno, k); return false; }
	}
	return true;
}

static uint32_t retain_for(const char *topic) {
	for (int i = 0; i < g_cfg.nretain; i++)
		if (fsess_wanted(g_cfg.retain[i].pat, topic)) return g_cfg.retain[i].secs;
	return g_cfg.retain_default;
}

// -- who --

static bool in_list(const void *l, int n, const uint8_t k[32]) {
	for (int i = 0; i < n; i++) if (!memcmp((const uint8_t *)l + 32 * i, k, 32)) return true;
	return false;
}

static int peer_of(const uint8_t k[32]) {
	for (int i = 0; i < g_cfg.npeers; i++) if (!memcmp(g_cfg.peers[i].key, k, 32)) return i;
	return -1;
}

// -- networks: each one's current list --

static fnet_list_t g_list[FNODE_NETWORKS];
static bool g_haslist[FNODE_NETWORKS];
static char g_ip[FNODE_NETWORKS][FNET_MAX_NODES][48];	// resolved addresses; "" if none
static bool (*g_resolve)(const char *host, char *ip, int cap);

void fnode_set_resolver(bool (*resolve)(const char *host, char *ip, int cap)) { g_resolve = resolve; }

static bool member_of(int n, const uint8_t k[32]) {
	return g_haslist[n] && fnet_has(&g_list[n], k, NULL);
}

static bool in_a_network(const uint8_t k[32]) {
	for (int n = 0; n < g_cfg.nnetworks; n++) if (member_of(n, k)) return true;
	return false;
}

// Configured by the sysop, or on a current list -- and not blocked.
static bool known(const uint8_t k[32]) {
	if (in_list(g_cfg.blocked, g_cfg.nblocked, k)) return false;
	return peer_of(k) >= 0 || in_list(g_cfg.members, g_cfg.nmembers, k) || in_a_network(k);
}

// Network n's list from the store: refused whole if anything in it is
// wrong, the previous one kept.
static int load_list(int n) {
	static uint8_t buf[FOBJ_MAX];
	char topic[FOBJ_TOPIC_MAX + 1], err[120], line[240];
	fnet_list_t *fresh_p = fnet_scratch();		// shared: fnet.h
#define fresh (*fresh_p)
	fobj_t o;
	snprintf(topic, sizeof(topic), "%.32s/nodes", g_cfg.networks[n].name);
	int r = fstore_state_get(topic, g_cfg.networks[n].publisher, "list", buf, sizeof(buf));
	if (r <= 0) return 0;
	if (fobj_parse(buf, (uint32_t)r, 0, &o, NULL) || strcmp(o.type, "fed.nodes") || o.format != FOBJ_JSON) {
		snprintf(line, sizeof(line), "fed: %s: the current list is not a fed.nodes JSON object -- ignored", g_cfg.networks[n].name);
		plat_log(line);
		return 0;
	}
	if (fnet_parse(g_cfg.networks[n].name, o.payload, o.len, &fresh, err, sizeof(err))) {
		snprintf(line, sizeof(line), "fed: %s: a list refused (%s); the previous one stands", g_cfg.networks[n].name, err);
		plat_log(line);
		return 0;
	}
	g_list[n] = fresh;
	g_haslist[n] = true;
	for (int i = 0; i < g_list[n].n; i++) {
		g_ip[n][i][0] = 0;
		if (!g_list[n].addr[i][0] || !g_resolve) continue;
		char host[FNET_ADDR_MAX + 1];
		snprintf(host, sizeof(host), "%s", g_list[n].addr[i]);
		*strrchr(host, ':') = 0;
		if (!g_resolve(host, g_ip[n][i], sizeof(g_ip[n][i]))) {
			g_ip[n][i][0] = 0;
			// said only where it matters: the address is checked on
			// connections this node TAKES -- a node that only connects
			// out takes none, and none come from itself
			if (g_cfg.listen && memcmp(g_list[n].key[i], g_pk, 32)) {
				snprintf(line, sizeof(line), "fed: %s: cannot resolve %s -- that node's address is not checked",
					g_cfg.networks[n].name, host);
				plat_log(line);
			}
		}
	}
	snprintf(line, sizeof(line), "fed: %s: a list of %d nodes", g_cfg.networks[n].name, g_list[n].n);
	plat_log(line);
	return 1;
}
#undef fresh

int fnode_reload_lists(void) {
	int k = 0;
	for (int n = 0; n < g_cfg.nnetworks; n++) k += load_list(n);
	return k;
}

// Which configured network a topic belongs to (its first segment), or -1.
static int network_of(const char *topic) {
	for (int n = 0; n < g_cfg.nnetworks; n++) {
		size_t l = strlen(g_cfg.networks[n].name);
		if (!strncmp(topic, g_cfg.networks[n].name, l) && (topic[l] == '/' || !topic[l])) return n;
	}
	return -1;
}

bool fnode_origin_ok(const uint8_t origin[32], const char *topic) {
	if (in_list(g_cfg.blocked, g_cfg.nblocked, origin)) return false;
	if (!strcmp(topic, "fed/join")) return false;			// join requests are never stored or relayed
	// a node's info -- its mail keys -- from that node and no other: else
	// anyone could plant a key of their own for someone, and read their mail
	if (!strncmp(topic, "fed/node/", 9)) {
		char sid[17];
		fobj_short_id(origin, sid);
		if (strcmp(topic + 9, sid)) return false;
	}
	int n = network_of(topic);
	if (n >= 0) {
		char nodes[FOBJ_TOPIC_MAX + 1];
		snprintf(nodes, sizeof(nodes), "%.32s/nodes", g_cfg.networks[n].name);
		if (!strcmp(topic, nodes)) return !memcmp(origin, g_cfg.networks[n].publisher, 32);	// its list: the publisher only
		// its other topics: its members (and whom the sysop configured)
		return !memcmp(origin, g_pk, 32) || member_of(n, origin) || peer_of(origin) >= 0 ||
			in_list(g_cfg.members, g_cfg.nmembers, origin);
	}
	return !memcmp(origin, g_pk, 32) || known(origin);
}

// The address check (docs/fed.md, "The handshake", step 0): where a list
// gives a member an address it resolved, the connection must come from it.
bool fnode_connect_ok(const uint8_t key[32], const char *addr) {
	if (!known(key)) return false;
	bool listed = false, match = false;
	for (int n = 0; n < g_cfg.nnetworks; n++) {
		if (!g_haslist[n]) continue;
		for (int i = 0; i < g_list[n].n; i++) {
			if (memcmp(g_list[n].key[i], key, 32) || !g_ip[n][i][0]) continue;
			listed = true;
			if (addr && !strcmp(g_ip[n][i], addr)) match = true;
		}
	}
	return !listed || match;
}

// -- rate limits: per address and per key, doubling on each failure --

typedef struct { uint32_t id; uint8_t fails; uint32_t until_ms; } limit_t;
static limit_t g_lim[FNODE_LIMITS];

static uint32_t hash(const void *d, uint32_t n) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)d)[i]) * 16777619u;
	return h ? h : 1;
}

static limit_t *lim_find(uint32_t id, bool make) {
	limit_t *oldest = &g_lim[0];
	for (int i = 0; i < FNODE_LIMITS; i++) {
		if (g_lim[i].id == id) return &g_lim[i];
		if ((int32_t)(g_lim[i].until_ms - oldest->until_ms) < 0) oldest = &g_lim[i];
	}
	if (!make) return NULL;
	memset(oldest, 0, sizeof(*oldest));
	oldest->id = id;
	return oldest;
}

static bool lim_ok(uint32_t id) {
	limit_t *l = lim_find(id, false);
	return !l || (int32_t)(g_now_ms - l->until_ms) >= 0;
}

static void lim_fail(uint32_t id) {
	limit_t *l = lim_find(id, true);
	if (l->fails < 10) l->fails++;
	uint32_t s = 1u << l->fails;
	if (s > 600) s = 600;
	l->until_ms = g_now_ms + s * 1000;
}

static void lim_clear(uint32_t id) {
	limit_t *l = lim_find(id, false);
	if (l) memset(l, 0, sizeof(*l));
}

bool fnode_admit(const char *addr, uint32_t now_ms) {
	g_now_ms = now_ms;
	return lim_ok(hash(addr, (uint32_t)strlen(addr)));
}

// -- sessions --

typedef struct {
	fsess_t s;
	bool used, initiator;
	int peer;
	char addr[64];
	uint32_t gen;					// g_member_gen when it began
} slot_t;
static slot_t g_slot[FNODE_SESSIONS];

static bool cb_allowed(const uint8_t key[32], void *ctx) {
	slot_t *sl = ctx;
	if (!fnode_connect_ok(key, sl->addr) || !lim_ok(hash(key, 32))) return false;
	int p = peer_of(key);
	if (p >= 0 && g_cfg.peers[p].trusted) sl->s.cfg.trusted = true;
	if (p >= 0) sl->s.cfg.max_object = g_cfg.peers[p].max_object;	// its link's limit, when it dials us too
	return true;
}

// -- cancels (docs/fed.md, "Moderation") --
//
// A cancel -- fed.cancel on <network>/cancel: {"id": ..., "topic": ...} --
// is honoured when it comes from its target's origin, or from a
// moderator the network's list names for the target's topic -- judged
// by the TARGET's own topic, always, never by what the cancel says, so
// a moderator gains nothing by misnaming it. The cancel's topic must
// still agree, or the cancel is malformed and ignored (a consistency
// rule: the live test's "lie" is refused on authority alone, and
// removing this check changes nothing it can see). A target held already is marked cancelled; one that
// has not arrived -- flooding brings things in any order -- is refused
// when it does: the cancel is remembered, a ring of FNODE_CANCELS.

#define FNODE_CANCELS 256
static struct { bool used; uint8_t id[32], by[32], th[8]; } g_cx[FNODE_CANCELS];
static int g_cx_next;

// The largest object a network takes on this topic (fed.md, "Network
// profiles"); 0: no limit but the protocol's. Its node list is exempt.
static uint32_t net_max(const char *topic, const char *type) {
	int n = network_of(topic);
	if (n < 0 || !g_haslist[n] || !g_list[n].max_object) return 0;
	if (!strcmp(type, "fed.nodes")) return 0;
	return g_list[n].max_object;
}

static bool may_cancel(const uint8_t by[32], const uint8_t origin[32], const char *topic) {
	if (!memcmp(by, origin, 32)) return true;
	int n = network_of(topic);
	return n >= 0 && g_haslist[n] && fnet_moderates(&g_list[n], by, topic);
}

static void topic_hash(const char *t, uint8_t h[8]) {
	uint8_t d[32];
	z_sha256(d, (const uint8_t *)t, (uint32_t)strlen(t));
	memcpy(h, d, 8);
}

static bool hex32(const char *s, uint8_t out[32]);

static void cancel_arrived(const fobj_t *o) {
	static zjson_tok_t t[16];
	char ct[FOBJ_TOPIC_MAX + 1], idh[70], topic[FOBJ_TOPIC_MAX + 2], line[200];
	uint8_t id[32];
	if (strcmp(o->type, "fed.cancel") || o->format != FOBJ_JSON) return;
	int n = network_of(o->topic);
	if (n < 0) return;
	snprintf(ct, sizeof(ct), "%.32s/cancel", g_cfg.networks[n].name);
	if (strcmp(o->topic, ct)) return;
	const char *js = (const char *)o->payload;
	int k, tp;
	if (zjson_parse(js, o->len, t, 16, NULL) < 1 || t[0].type != ZJ_OBJECT ||
			(k = zjson_get(js, t, 0, "id")) < 0 || !zjson_str(js, &t[k], idh, sizeof(idh)) || !hex32(idh, id) ||
			(tp = zjson_get(js, t, 0, "topic")) < 0 || !zjson_str(js, &t[tp], topic, sizeof(topic)) || strlen(topic) > FOBJ_TOPIC_MAX)
		return;
	// held: cancelled now, if it may be
	int r = fstore_get_id(id, g_mobj, FOBJ_MAX);
	fobj_t tg;
	if (r > 0 && !fobj_parse(g_mobj, (uint32_t)r, 0, &tg, NULL)) {
		if (!strcmp(tg.topic, topic) && may_cancel(o->origin, tg.origin, tg.topic) && fstore_cancel(id) == 1) {
			char by[17];
			fobj_short_id(o->origin, by);
			snprintf(line, sizeof(line), "fed: %.16s on %s cancelled by %s", idh, tg.topic, by);
			plat_log(line);
			fstore_sync();
		}
		return;
	}
	// not here yet: remembered, to be refused when it comes
	g_cx[g_cx_next].used = true;
	memcpy(g_cx[g_cx_next].id, id, 32);
	memcpy(g_cx[g_cx_next].by, o->origin, 32);
	topic_hash(topic, g_cx[g_cx_next].th);
	g_cx_next = (g_cx_next + 1) % FNODE_CANCELS;
}

static bool cb_admit(const fobj_t *o, void *ctx) {
	(void)ctx;
	uint32_t mx = net_max(o->topic, o->type);
	if (mx && o->size > mx) return false;			// larger than its network takes
	for (int i = 0; i < FNODE_CANCELS; i++) {
		if (!g_cx[i].used || memcmp(g_cx[i].id, o->id, 32)) continue;
		uint8_t th[8];
		topic_hash(o->topic, th);
		if (!memcmp(th, g_cx[i].th, 8) && may_cancel(g_cx[i].by, o->origin, o->topic)) {
			char line[160], idh[65];
			fobj_hex(o->id, 32, idh);
			snprintf(line, sizeof(line), "fed: %.16s on %s refused: cancelled before it came", idh, o->topic);
			plat_log(line);
			return false;
		}
	}
	return true;
}

static bool cb_origin(const uint8_t origin[32], const char *topic, void *ctx) {
	(void)ctx;
	return fnode_origin_ok(origin, topic);
}

// A new list from a network's publisher: that network's membership changes now.
// Membership changes with every new list, and what was refused while
// it was otherwise -- objects whose origin was not yet known to be a
// member -- would be lost for good: the cursor moved past them. So a
// new list clears every cursor, and the next sessions ask from the
// start; what is already here costs an id lookup, before any signature.
// A session the list arrived in records its cursor at its END, after
// that: so its peer's is cleared again when it ends (g_member_gen).
static uint32_t g_member_gen;

static void list_arrived(const fobj_t *o) {
	for (int n = 0; n < g_cfg.nnetworks; n++) {
		char nodes[FOBJ_TOPIC_MAX + 1];
		snprintf(nodes, sizeof(nodes), "%.32s/nodes", g_cfg.networks[n].name);
		if (!strcmp(o->topic, nodes) && !memcmp(o->origin, g_cfg.networks[n].publisher, 32)) {
			load_list(n);
			if (g_haslist[n]) {
				g_member_gen++;
				fstore_clear_cursors();
				plat_log("fed: a new node list -- every peer asked again from the start, for what was refused before it");
			}
		}
	}
}
static void cb_stored(const fobj_t *o, void *ctx) { (void)ctx; list_arrived(o); cancel_arrived(o); }

fsess_t *fnode_session(bool initiator, int peer, const char *addr, uint32_t now_ms) {
	g_now_ms = now_ms;
	for (int i = 0; i < FNODE_SESSIONS; i++) {
		slot_t *sl = &g_slot[i];
		if (sl->used) continue;
		// one outbound at a time (two client sockets in the whole system)
		if (initiator) for (int j = 0; j < FNODE_SESSIONS; j++) if (g_slot[j].used && g_slot[j].initiator) return NULL;
		sl->used = true;
		sl->initiator = initiator;
		sl->peer = peer;
		sl->gen = g_member_gen;
		snprintf(sl->addr, sizeof(sl->addr), "%s", addr ? addr : "");
		fsess_cfg_t c;
		memset(&c, 0, sizeof(c));
		c.secret_key = g_sk;
		c.peer = initiator ? g_cfg.peers[peer].key : NULL;
		c.allowed = cb_allowed;
		c.origin_ok = cb_origin;
		c.wants = g_cfg.wants;
		c.now = plat_now();
		c.ctx = sl;
		c.trusted = initiator && g_cfg.peers[peer].trusted;
		c.max_object = initiator ? g_cfg.peers[peer].max_object : 0;		// a responder learns it in cb_allowed
		c.stored = cb_stored;
		c.admit = cb_admit;
		fsess_init(&sl->s, &c, initiator);
		fsess_poll(&sl->s, now_ms);
		return &sl->s;
	}
	return NULL;
}

void fnode_session_end(fsess_t *s, uint32_t now_ms) {
	slot_t *sl = (slot_t *)s;			// s is a slot's first member
	char line[320], sid[17] = "-";
	g_now_ms = now_ms;
	if (!sl->used) return;
	fsess_closed(s);
	if (s->state == FS_DONE || s->phase >= 2) fobj_short_id(s->peer_key, sid);
	// the membership changed while it ran: its END recorded a cursor past
	// what it refused before -- cleared again, so the next session asks anew
	if (sl->gen != g_member_gen && (s->state == FS_DONE || s->phase >= 2)) fstore_set_cursor(s->peer_key, 0, 0);
	bool ok = s->state == FS_DONE;
	if (sl->initiator) {
		fpeer_t *p = &g_cfg.peers[sl->peer];
		if (ok) { p->backoff_s = 0; p->due_ms = now_ms + g_cfg.poll_s * 1000; }
		else {
			p->backoff_s = p->backoff_s ? p->backoff_s * 2 : 30;
			// at most ten minutes, as the other side's limit: a node refused
			// until it was added to the list is let in soon after, not in an
			// hour; a peer down for hours costs six attempts an hour
			if (p->backoff_s > 600) p->backoff_s = 600;
			p->due_ms = now_ms + p->backoff_s * 1000;
		}
		if (!p->due_ms) p->due_ms = 1;
	} else {
		uint32_t a = hash(sl->addr, (uint32_t)strlen(sl->addr)), k = hash(s->peer_key, 32);
		if (ok) { lim_clear(a); lim_clear(k); }
		else { lim_fail(a); if (s->phase >= 1) lim_fail(k); }
	}
	snprintf(line, sizeof(line), "fed: session %s %s: %s -- %u new, %u had, %u refused, %u sent%s%s",
		sl->initiator ? "to" : "from", sid, ok ? "done" : s->error,
		(unsigned)s->stats.got_new, (unsigned)s->stats.got_have, (unsigned)s->stats.got_rejected, (unsigned)s->stats.sent,
		s->stats.withheld ? ", some withheld: larger than this link takes" : "",
		s->stats.got_expired ? "; some arrived already expired -- is the sending node's clock right?" : "");
	plat_log(line);
	sl->used = false;
}

int fnode_due(uint32_t now_ms) {
	g_now_ms = now_ms;
	for (int j = 0; j < FNODE_SESSIONS; j++) if (g_slot[j].used && g_slot[j].initiator) return -1;
	for (int i = 0; i < g_cfg.npeers; i++) {
		fpeer_t *p = &g_cfg.peers[i];
		if (!p->host[0]) continue;
		if (!p->due_ms || (int32_t)(now_ms - p->due_ms) >= 0) return i;
	}
	return -1;
}

// -- publishing --

static uint64_t next_seq(void) {
	char p[200], b[32];
	uint64_t v = 0;
	snprintf(p, sizeof(p), "%s/seq.txt", g_dir);
	int h = plat_open(p, PLAT_READ);
	// by hand, never 64 bits through printf or scanf (fobj.h)
	if (h >= 0) { int n = plat_read(h, b, sizeof(b) - 1); plat_close(h); if (n > 0) { b[n] = 0; if (!fobj_dec_u64(b, &v)) v = 0; } }
	v++;
	h = plat_open(p, PLAT_CREATE);
	if (h >= 0) { int n = fobj_u64_dec(b, v); b[n++] = '\n'; plat_write(h, b, n); plat_sync(h); plat_close(h); }
	return v;
}

static int publish(const char *topic, const char *type, const char *format, const char *kind, const char *key,
	const uint8_t *payload, uint32_t len, char *reply, int cap) {
	static uint8_t obj[FOBJ_MAX];
	fobj_t o;
	uint32_t pos;
	memset(&o, 0, sizeof(o));
	// refused, never cut short: a truncated topic is a DIFFERENT topic
	if (strlen(topic) > FOBJ_TOPIC_MAX || strlen(type) > FOBJ_TYPE_MAX || strlen(key) > FOBJ_KEY_MAX)
		return snprintf(reply, (size_t)cap, "ERR a field is too long\n");
	// nothing no node would take from us: a join request's topic, or a
	// network's list when this node is not its publisher
	if (!fnode_origin_ok(g_pk, topic))
		return snprintf(reply, (size_t)cap, "ERR this node may not publish on %s\n", topic);
	snprintf(o.topic, sizeof(o.topic), "%s", topic);
	snprintf(o.type, sizeof(o.type), "%s", type);
	o.format = !strcmp(format, "json") ? FOBJ_JSON : !strcmp(format, "text") ? FOBJ_TEXT : !strcmp(format, "bytes") ? FOBJ_BYTES : 0;
	o.kind = !strcmp(kind, "log") ? FOBJ_LOG : !strcmp(kind, "state") ? FOBJ_STATE : 0;
	if (strcmp(key, "-")) snprintf(o.key, sizeof(o.key), "%s", key);
	o.time = plat_now();
	// A state object must be NEWER than this node's current one for the
	// same topic and key -- or it is refused as older. Two changes in one
	// second tie on time, and the tie went to whichever id was higher: a
	// quick second `fed add` was refused half the time (found by
	// tests/live_admin.py). So it takes the current one's time plus one
	// when the clock has not moved past it.
	if (o.kind == FOBJ_STATE) {
		fobj_t cur;
		// read into obj: not made yet -- and never the caller's payload
		int r = fstore_state_get(o.topic, g_pk, o.key, obj, sizeof(obj));
		if (r > 0 && !fobj_parse(obj, (uint32_t)r, 0, &cur, NULL) && cur.time >= o.time) o.time = cur.time + 1;
	}
	o.seq = next_seq();
	o.len = len;
	int n = fobj_make(&o, payload, g_sk, obj, sizeof(obj));
	if (n < 0) return snprintf(reply, (size_t)cap, "ERR %s\n", fobj_strerror(n));
	uint32_t mx = net_max(topic, type);
	if (mx && (uint32_t)n > mx)
		return snprintf(reply, (size_t)cap, "ERR %d bytes: larger than this network takes (%u)\n", n, (unsigned)mx);
	int r = fstore_put(obj, (uint32_t)n, plat_now(), &pos);
	if (r < 0) return snprintf(reply, (size_t)cap, "ERR the store refused it\n");
	if (r != FSTORE_NEW) return snprintf(reply, (size_t)cap, "ERR %s\n", r == FSTORE_STALE ? "older than the current state" :
		r == FSTORE_EXPIRED ? "already past retention" : "already stored");
	fstore_sync();
	list_arrived(&o);			// this node publishing a network's list
	{
		fobj_t po;						// ... or a cancel: with its payload, parsed back
		if (!fobj_parse(obj, (uint32_t)n, 0, &po, NULL)) cancel_arrived(&po);
	}
	char id[65];
	fobj_hex(o.id, 32, id);
	return snprintf(reply, (size_t)cap, "OK %s %u\n", id, (unsigned)pos);
}

// -- mail between nodes (docs/fed.md, "Mail between nodes") --

static bool unhex(const char *s, uint8_t *out, uint32_t n) {
	if (strlen(s) != 2 * n) return false;
	for (uint32_t i = 0; i < n; i++) {
		int v = 0;
		for (int j = 0; j < 2; j++) {
			char c = s[2 * i + j];
			v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1000);
		}
		if (v < 0) return false;
		out[i] = (uint8_t)v;
	}
	return true;
}

static bool hex32(const char *s, uint8_t out[32]) { return unhex(s, out, 32); }

// This node's info: its name and its public mail keys, on
// fed/node/<short id> -- published when it differs from what is stored.
static void publish_node_info(void) {
	static char js[ZMLKEM_EK_BYTES * 2 + 256], eh[ZMLKEM_EK_BYTES * 2 + 1];
	uint8_t *buf = g_mobj;
	char sid[17], topic[40], xh[65], reply[200];
	fobj_short_id(g_pk, sid);
	snprintf(topic, sizeof(topic), "fed/node/%s", sid);
	fobj_hex(g_mail.xpk, 32, xh);
	fobj_hex(g_mail.ek, ZMLKEM_EK_BYTES, eh);
	bool named = g_cfg.name[0] && fnet_name_ok(g_cfg.name);
	int n = snprintf(js, sizeof(js), "{%s%s%s\"mail\":{\"x25519\":\"%s\",\"mlkem\":\"%s\"}}",
		named ? "\"name\":\"" : "", named ? g_cfg.name : "", named ? "\"," : "", xh, eh);
	// the full info, and a small one beside it -- the name and the X25519
	// key alone, ~450 bytes, to cross a link too small for 2.6 KB
	// (fed.md, "Network profiles"): each published when it changed
	for (int small = 0; small < 2; small++) {
		if (small) n = snprintf(js, sizeof(js), "{%s%s%s\"mail\":{\"x25519\":\"%s\"}}",
			named ? "\"name\":\"" : "", named ? g_cfg.name : "", named ? "\"," : "", xh);
		const char *key = small ? "x25519" : "info";
		int r = fstore_state_get(topic, g_pk, key, buf, FOBJ_MAX);
		fobj_t o;
		if (r > 0 && !fobj_parse(buf, (uint32_t)r, 0, &o, NULL) && o.len == (uint32_t)n && !memcmp(o.payload, js, (size_t)n)) continue;
		publish(topic, "fed.node", "json", "state", key, (const uint8_t *)js, (uint32_t)n, reply, sizeof(reply));
		if (!strncmp(reply, "OK", 2)) plat_log(small ? "fed: this node's info published (small: its X25519 key)" :
			"fed: this node's info published (its mail keys)");
		else { reply[strcspn(reply, "\n")] = 0; char line[240]; snprintf(line, sizeof(line), "fed: this node's info: %s", reply); plat_log(line); }
	}
}

// A node's public mail keys, from its info in the store: both, from the
// full info (ek non-NULL); or only the X25519 key, from either.
static bool keys_from(const uint8_t key[32], const char *skey, uint8_t xpk[32], uint8_t *ek);

static bool mail_keys_of(const uint8_t key[32], uint8_t xpk[32], uint8_t *ek) {
	if (ek) return keys_from(key, "info", xpk, ek);
	return keys_from(key, "info", xpk, NULL) || keys_from(key, "x25519", xpk, NULL);
}

static bool keys_from(const uint8_t key[32], const char *skey, uint8_t xpk[32], uint8_t *ek) {
	uint8_t *buf = g_mobj;
	static char eh[ZMLKEM_EK_BYTES * 2 + 2];
	static zjson_tok_t t[32];
	char sid[17], topic[40], xh[70];
	fobj_t o;
	fobj_short_id(key, sid);
	snprintf(topic, sizeof(topic), "fed/node/%s", sid);
	int r = fstore_state_get(topic, key, skey, buf, FOBJ_MAX);
	if (r <= 0 || fobj_parse(buf, (uint32_t)r, 0, &o, NULL) || o.format != FOBJ_JSON) return false;
	const char *js = (const char *)o.payload;
	if (zjson_parse(js, o.len, t, 32, NULL) < 1 || t[0].type != ZJ_OBJECT) return false;
	int m = zjson_get(js, t, 0, "mail");
	if (m < 0 || t[m].type != ZJ_OBJECT) return false;
	int x = zjson_get(js, t, m, "x25519"), e = zjson_get(js, t, m, "mlkem");
	if (x < 0 || !zjson_str(js, &t[x], xh, sizeof(xh)) || !unhex(xh, xpk, 32)) return false;
	if (!ek) return true;
	return e >= 0 && zjson_str(js, &t[e], eh, sizeof(eh)) && unhex(eh, ek, ZMLKEM_EK_BYTES) && zmlkem_ek_ok(ek);
}

// A node by its key (64 hex), short id (16 hex) or name in a list: its
// key and the network whose list has it.
// A node by its key (64 hex), short id (16 hex), or name in a network's
// list -- "name@network" to say which. 1: found, its key and network;
// 0: not in any list here; -1: a bare name two networks' lists give to
// different nodes. Each list has one publisher, but a node may follow
// several, and a name in one could otherwise be taken for the same name
// in another -- a letter sealed to the wrong node. So never a guess.
static int find_node(const char *who_in, uint8_t key[32], int *net) {
	uint8_t *buf = g_mobj;
	uint8_t k[32], got[32];
	char who[80];
	const char *only = NULL;
	int found = 0;
	snprintf(who, sizeof(who), "%s", who_in);
	char *at = strchr(who, '@');
	if (at) { *at = 0; only = at + 1; }
	bool bykey = strlen(who) == 64 && hexkey(who, k);
	for (int n = 0; n < g_cfg.nnetworks; n++) {
		if (!g_haslist[n] || (only && strcmp(g_cfg.networks[n].name, only))) continue;
		bool here = false;
		for (int i = 0; i < g_list[n].n && !here; i++) {
			char sid[17];
			fobj_short_id(g_list[n].key[i], sid);
			if ((bykey && !memcmp(g_list[n].key[i], k, 32)) || (!bykey && !strcmp(sid, who))) {
				memcpy(got, g_list[n].key[i], 32);
				here = true;
			}
		}
		if (!here && !bykey && fnet_name_ok(who)) {
			char topic[FOBJ_TOPIC_MAX + 1];
			fobj_t o;
			snprintf(topic, sizeof(topic), "%.32s/nodes", g_cfg.networks[n].name);
			int r = fstore_state_get(topic, g_cfg.networks[n].publisher, "list", buf, FOBJ_MAX);
			here = r > 0 && !fobj_parse(buf, (uint32_t)r, 0, &o, NULL) && fnet_find_name(o.payload, o.len, who, got);
		}
		if (!here) continue;
		if (found && memcmp(got, key, 32)) return -1;		// the same name, another node
		if (!found) { memcpy(key, got, 32); *net = n; found = 1; }
	}
	return found;
}

// MAIL: a letter sealed to a node, published on <network>/mail/<its short id>.
static int send_mail(const char *who, const char *type, const uint8_t *letter, uint32_t len, char *reply, int cap) {
	uint8_t *sealed = g_mletter;
	static uint8_t ek[ZMLKEM_EK_BYTES];
	uint8_t key[32], xpk[32], rnd[FMAIL_RANDOM];
	int net;
	if (len > FOBJ_PAYLOAD_MAX - FMAIL_OVERHEAD) return snprintf(reply, (size_t)cap, "ERR a letter is at most %d bytes\n", FOBJ_PAYLOAD_MAX - FMAIL_OVERHEAD);
	int f = find_node(who, key, &net);
	if (f < 0) return snprintf(reply, (size_t)cap, "ERR %s is a name in more than one network's list: say which (name@network)\n", who);
	if (!f) return snprintf(reply, (size_t)cap, "ERR no node %s in any network's list\n", who);
	// sealed as the letter's network chooses: hybrid, or X25519 alone
	bool classical = g_haslist[net] && g_list[net].classical;
	if (!mail_keys_of(key, xpk, classical ? NULL : ek))
		return snprintf(reply, (size_t)cap, "ERR no mail key for %s yet (its node info has not arrived)\n", who);
	plat_random(rnd, sizeof(rnd));
	int n = classical ? fmail_seal_classical(sealed, FOBJ_PAYLOAD_MAX, letter, len, xpk, rnd) :
		fmail_seal(sealed, FOBJ_PAYLOAD_MAX, letter, len, xpk, ek, rnd);
	crypto_wipe(rnd, sizeof(rnd));
	if (n < 0) return snprintf(reply, (size_t)cap, "ERR %s's mail key is not usable\n", who);
	char sid[17], topic[FOBJ_TOPIC_MAX + 1];
	fobj_short_id(key, sid);
	snprintf(topic, sizeof(topic), "%.32s/mail/%s", g_cfg.networks[net].name, sid);
	return publish(topic, type, "bytes", "log", "-", sealed, (uint32_t)n, reply, cap);
}

// -- clients: the local interface --

// A client is a program (its lines end in \n: bbs, nc) or a person at a
// terminal (`port fed0` in term: Enter sends \r) -- decided by how its
// FIRST line ends, once: a program's payload may hold a \r later, and
// must never flip it. A terminal gets its typing echoed, Backspace, and
// \r\n line ends; a program gets the bytes, exactly.
enum { CM_UNKNOWN = 0, CM_PROGRAM, CM_TERMINAL };

typedef struct {
	bool used, sub, inflight;
	uint8_t mode;
	bool last_cr;
	char name[32], pattern[100];
	uint32_t acked, scan, inflight_pos;
	// SUB's history (a forum newly carried): older objects on these topics,
	// up to where the subscription had got, sent first -- then HIST END
	char hist[100];
	uint32_t hist_scan, hist_until;
	bool hist_on;
	uint8_t in[FOBJ_PAYLOAD_MAX + 512];
	uint32_t in_len;
	uint8_t out[FOBJ_MAX + 64];
	uint32_t out_head, out_len;
} client_t;
static client_t g_cl[FNODE_CLIENTS];

int fnode_client_open(void) {
	for (int i = 0; i < FNODE_CLIENTS; i++)
		if (!g_cl[i].used) { memset(&g_cl[i], 0, sizeof(g_cl[i])); g_cl[i].used = true; return i; }
	return -1;
}

void fnode_client_close(int id) {
	if (id >= 0 && id < FNODE_CLIENTS) g_cl[id].used = false;
}

static void say_raw(client_t *c, const char *s, int n) {
	if (c->out_head + c->out_len + (uint32_t)n > sizeof(c->out)) {
		memmove(c->out, c->out + c->out_head, c->out_len);
		c->out_head = 0;
	}
	if (c->out_len + (uint32_t)n > sizeof(c->out)) return;
	memcpy(c->out + c->out_head + c->out_len, s, (size_t)n);
	c->out_len += (uint32_t)n;
}

// Replies: to a terminal, each \n as \r\n.
static void say(client_t *c, const char *s, int n) {
	if (c->mode != CM_TERMINAL) { say_raw(c, s, n); return; }
	for (int i = 0; i < n; i++) {
		if (s[i] == '\n') say_raw(c, "\r\n", 2);
		else say_raw(c, s + i, 1);
	}
}

// A delivered object's bytes, for a terminal: \n as \r\n in place, as
// far as the buffer has room (past that, the rest shows as it is).
static void term_lines(client_t *c) {
	uint8_t *b = c->out + c->out_head;
	uint32_t n = c->out_len, room = (uint32_t)sizeof(c->out) - c->out_head - c->out_len;
	for (uint32_t i = 0; i < n && room; i++) {
		if (b[i] != '\n') continue;
		memmove(b + i + 1, b + i, n - i);
		b[i] = '\r';
		n++; i++; room--;
	}
	c->out_len = n;
}

// The next object on the client's pattern, if none is in flight.
static void deliver(client_t *c) {
	if (!c->sub || c->inflight || c->out_len) return;
	for (int budget = 64; budget > 0; budget--) {
		uint32_t p;
		bool history = c->hist_on;
		if (history) {
			p = fstore_next(c->hist_scan);
			if (!p || p > c->hist_until) {			// the history sent: said, and the stream goes on
				c->hist_on = false;
				say(c, "HIST END\n", 9);
				return;
			}
			c->hist_scan = p;
		} else {
			p = fstore_next(c->scan);
			if (!p) return;
			c->scan = p;
		}
		uint8_t *b = c->out + 32;
		int n = fstore_get(p, b, FOBJ_MAX);
		fobj_t o;
		if (n <= 0 || fobj_parse(b, (uint32_t)n, 0, &o, NULL) || !fsess_wanted(history ? c->hist : c->pattern, o.topic)) continue;
		char h[32];
		int hl = snprintf(h, sizeof(h), "OBJ %u %d\n", (unsigned)p, n);
		memcpy(c->out + 32 - hl, h, (size_t)hl);
		c->out_head = 32 - (uint32_t)hl;
		c->out_len = (uint32_t)(hl + n);
		if (c->mode == CM_TERMINAL) {
			memmove(c->out, c->out + c->out_head, c->out_len);		// room to grow at the end
			c->out_head = 0;
			term_lines(c);
		}
		c->inflight = true;
		c->inflight_pos = p;
		return;
	}
}

// A terminal's bytes, as they come: echoed, Enter a line end, Backspace
// taking the last byte of the line back.
static void term_input(client_t *c, const uint8_t *d, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		uint8_t b = d[i];
		if (b == '\n' && c->last_cr) { c->last_cr = false; continue; }		// \r\n: one line end
		c->last_cr = b == '\r';
		if (b == '\r') b = '\n';
		if (b == 8 || b == 127) {
			if (c->in_len && c->in[c->in_len - 1] != '\n') { c->in_len--; say_raw(c, "\b \b", 3); }
			continue;
		}
		if (c->in_len >= sizeof(c->in)) continue;
		c->in[c->in_len++] = b;
		if (b == '\n') say_raw(c, "\r\n", 2);
		else say_raw(c, (const char *)&b, 1);
	}
}

void fnode_client_input(int id, const uint8_t *d, uint32_t n) {
	client_t *c = &g_cl[id];
	char reply[160];
	// who is it: decided, once, by how the first line ends
	if (c->mode == CM_UNKNOWN) {
		for (uint32_t i = 0; i < n; i++) {
			if (d[i] == '\n') { c->mode = CM_PROGRAM; break; }
			if (d[i] == '\r') {
				// A terminal. What was typed before we knew was not echoed:
				// shown now, kept as it is, and the rest handled as typing.
				c->mode = CM_TERMINAL;
				say_raw(c, (const char *)c->in, (int)c->in_len);
				say_raw(c, (const char *)d, (int)i);
				if (c->in_len + i > sizeof(c->in)) i = (uint32_t)sizeof(c->in) - c->in_len;
				memcpy(c->in + c->in_len, d, i);
				c->in_len += i;
				d += i;
				n -= i;
				break;
			}
		}
	}
	if (c->mode == CM_TERMINAL) {
		term_input(c, d, n);
	} else {
		if (c->in_len + n > sizeof(c->in)) { say(c, "ERR too long\n", 13); c->in_len = 0; return; }
		memcpy(c->in + c->in_len, d, n);
		c->in_len += n;
	}
	while (c->in_len) {
		uint8_t *nl = memchr(c->in, '\n', c->in_len);
		if (!nl) return;
		uint32_t ll = (uint32_t)(nl - c->in);
		char line[256];
		if (ll >= sizeof(line)) { say(c, "ERR line too long\n", 18); c->in_len = 0; return; }
		memcpy(line, c->in, ll);
		line[ll] = 0;
		uint32_t used = ll + 1;
		// Split on spaces with no length cap: a field too long for its use
		// is refused by that use, AFTER its payload has been consumed --
		// splitting a long field in two would shift every field after it
		// and lose the stream's place.
		char *a[8];
		int k = 0;
		for (char *t = strtok(line, " "); t && k < 8; t = strtok(NULL, " ")) a[k++] = t;
		int r;
		if (k == 7 && !strcmp(a[0], "PUB")) {
			char *end;
			unsigned long len = strtoul(a[6], &end, 10);
			if (*end || len > FOBJ_PAYLOAD_MAX) { say(c, "ERR bad payload length\n", 23); c->in_len = 0; return; }
			if (c->in_len < used + len) return;			// the rest of the payload is still coming
			r = publish(a[1], a[2], a[3], a[4], a[5], c->in + used, (uint32_t)len, reply, sizeof(reply));
			used += (uint32_t)len;
			say(c, reply, r);
		} else if ((k == 3 || k == 4) && !strcmp(a[0], "SUB") && (strlen(a[1]) >= sizeof(c->name) || strlen(a[2]) >= sizeof(c->pattern))) {
			say(c, "ERR name or pattern too long\n", 29);
		} else if ((k == 3 || k == 4) && !strcmp(a[0], "SUB") && k == 4 && strlen(a[3]) >= sizeof(c->hist)) {
			say(c, "ERR history topics too long\n", 28);
		} else if ((k == 3 || k == 4) && !strcmp(a[0], "SUB")) {
			snprintf(c->name, sizeof(c->name), "%s", a[1]);
			snprintf(c->pattern, sizeof(c->pattern), "%s", a[2]);
			c->acked = c->scan = fstore_consumer(c->name);
			c->sub = true;
			c->hist_on = false;
			if (k == 4) {
				// SUB NAME PATTERN T1,T2: the history of those topics first
				snprintf(c->hist, sizeof(c->hist), "%s", a[3]);
				for (char *q = c->hist; *q; q++) if (*q == ',') *q = '\n';
				c->hist_scan = 0;
				c->hist_until = c->acked;
				c->hist_on = true;
			}
			r = snprintf(reply, sizeof(reply), "OK subscribed after %u\n", (unsigned)c->acked);
			say(c, reply, r);
		} else if (k == 4 && !strcmp(a[0], "MAIL")) {
			char *end;
			unsigned long len = strtoul(a[3], &end, 10);
			if (*end || len > FOBJ_PAYLOAD_MAX) { say(c, "ERR bad payload length\n", 23); c->in_len = 0; return; }
			if (c->in_len < used + len) return;
			r = send_mail(a[1], a[2], c->in + used, (uint32_t)len, reply, sizeof(reply));
			used += (uint32_t)len;
			say(c, reply, r);
		} else if (k == 2 && !strcmp(a[0], "OPEN")) {
			uint8_t *letter = g_mletter;
			char *end;
			unsigned long len = strtoul(a[1], &end, 10);
			if (*end || len > FOBJ_PAYLOAD_MAX) { say(c, "ERR bad payload length\n", 23); c->in_len = 0; return; }
			if (c->in_len < used + len) return;
			int n = fmail_open(letter, FOBJ_PAYLOAD_MAX, c->in + used, (uint32_t)len, &g_mail);
			used += (uint32_t)len;
			if (n < 0) say(c, "ERR not a letter to this node, or tampered with\n", 48);
			else {
				r = snprintf(reply, sizeof(reply), "OK %d\n", n);
				say(c, reply, r);
				say_raw(c, (const char *)letter, n);
				crypto_wipe(letter, (size_t)n);
			}
		} else if (k == 2 && !strcmp(a[0], "LIST")) {
			// a network's current list, as published: for `fed nodes`, `fed add`
			int n = -1;
			for (int i = 0; i < g_cfg.nnetworks; i++) if (!strcmp(g_cfg.networks[i].name, a[1])) n = i;
			if (n < 0) { r = snprintf(reply, sizeof(reply), "ERR this node does not follow a network called %s\n", a[1]); say(c, reply, r); }
			else {
				char topic[FOBJ_TOPIC_MAX + 1];
				fobj_t o;
				snprintf(topic, sizeof(topic), "%.32s/nodes", g_cfg.networks[n].name);
				int got = fstore_state_get(topic, g_cfg.networks[n].publisher, "list", g_mobj, FOBJ_MAX);
				if (got <= 0 || fobj_parse(g_mobj, (uint32_t)got, 0, &o, NULL)) say(c, "OK 0\n", 5);	// none yet
				else {
					r = snprintf(reply, sizeof(reply), "OK %u\n", (unsigned)o.len);
					say(c, reply, r);
					say_raw(c, (const char *)o.payload, (int)o.len);
				}
			}
		} else if (k == 2 && !strcmp(a[0], "CANCELLED")) {
			// was this object cancelled here? (a client that has it already asks)
			uint8_t id[32];
			if (!hex32(a[1], id)) say(c, "ERR not an object id\n", 21);
			else { r = snprintf(reply, sizeof(reply), "OK %s\n", fstore_cancelled(id) ? "yes" : "no"); say(c, reply, r); }
		} else if (k == 1 && !strcmp(a[0], "KEY")) {
			// this node's key, short id and name: a client tells its own
			// objects from others', and knows its mail topic
			char h[65], sid[17];
			fobj_hex(g_pk, 32, h);
			fobj_short_id(g_pk, sid);
			r = snprintf(reply, sizeof(reply), "OK %s %s %s\n", h, sid, g_cfg.name[0] ? g_cfg.name : "-");
			say(c, reply, r);
		} else if (k == 2 && !strcmp(a[0], "ACK")) {
			uint32_t p = (uint32_t)strtoul(a[1], NULL, 10);
			if (c->inflight && p == c->inflight_pos) {
				c->inflight = false;
				// a history object is behind where the subscription is: its
				// ack does not move it back
				if (p > c->acked) { c->acked = p; fstore_set_consumer(c->name, p); }
			}
		} else {
			say(c, "ERR ?\n", 6);
		}
		memmove(c->in, c->in + used, c->in_len - used);
		c->in_len -= used;
	}
	deliver(c);
}

uint32_t fnode_client_output(int id, const uint8_t **p) {
	client_t *c = &g_cl[id];
	deliver(c);
	*p = c->out + c->out_head;
	return c->out_len;
}

void fnode_client_consumed(int id, uint32_t n) {
	client_t *c = &g_cl[id];
	if (n > c->out_len) n = c->out_len;
	c->out_head += n;
	c->out_len -= n;
	if (!c->out_len) c->out_head = 0;
}

// -- start, stop, tick --

// -- the radio link (fradio.c) --

static bool (*g_radio_send)(const uint8_t *p, uint32_t n, void *ctx);
static void *g_radio_ctx;
static bool g_radio_on;

void fnode_radio_attach(bool (*send)(const uint8_t *p, uint32_t n, void *ctx), void *ctx) {
	g_radio_send = send;
	g_radio_ctx = ctx;
}

static bool radio_send(const uint8_t *p, uint32_t n, void *ctx) {
	(void)ctx;
	return g_radio_send && g_radio_send(p, n, g_radio_ctx);
}
static uint32_t radio_clock(void *ctx) { (void)ctx; return plat_now(); }

static void cb_stored(const fobj_t *o, void *ctx);
static bool cb_admit(const fobj_t *o, void *ctx);
static bool cb_origin(const uint8_t origin[32], const char *topic, void *ctx);

static void radio_start(void) {
	g_radio_on = false;
	if (!g_cfg.radio) return;
	if (!g_radio_send) { plat_log("fed: radio: in fed.cfg, but this platform has none (LoRa is Zeitlos only, through mesh0)"); return; }
	fradio_cfg_t c;
	memset(&c, 0, sizeof(c));
	c.max_object = g_cfg.radio_max;
	c.pace_ms = g_cfg.radio_pace_ms;
	c.send = radio_send;
	c.clock = radio_clock;
	c.wants = g_cfg.wants;
	c.origin_ok = cb_origin;
	c.admit = cb_admit;
	c.stored = cb_stored;
	fradio_init(&c, g_now_ms);
	g_radio_on = true;
}

void fnode_radio_up(bool up) {
	if (!g_radio_on || up == fradio_is_up()) return;
	fradio_up(up);
	plat_log(up ? "fed: radio: the link is up" : "fed: radio: the link is down");
}

void fnode_radio_packet(const uint8_t *p, uint32_t n, uint32_t now_ms) {
	if (g_radio_on) fradio_packet(p, n, now_ms);
}

int fnode_start(const char *dir, const uint8_t seed[32], char *err, int errlen) {
	char p[200], text[8192];
	uint8_t s[32];
	snprintf(g_dir, sizeof(g_dir), "%s", dir);
	snprintf(p, sizeof(p), "%s/fed.cfg", dir);
	int h = plat_open(p, PLAT_READ);
	if (h < 0) { snprintf(err, (size_t)errlen, "no %s", p); return -1; }
	int n = plat_read(h, text, sizeof(text) - 1);
	plat_close(h);
	text[n > 0 ? n : 0] = 0;
	if (!fnode_config(text, &g_cfg, err, errlen)) return -1;
	fmail_keys(&g_mail, seed);
	// Wanted whatever subscribe: says, since nothing works without them:
	// every node's info (the mail keys), and in each network this node
	// follows, its list (membership) and this node's own mail. Following
	// a network without subscribing to its topics would otherwise break
	// membership without a word.
	{
		uint8_t pk[32], sk64[64], sd[32];
		char sid[17], need[FNODE_NETWORKS * 2 + 1][FOBJ_TOPIC_MAX + 2];
		int nn = 0;
		memcpy(sd, seed, 32);
		crypto_ed25519_key_pair(sk64, pk, sd);
		crypto_wipe(sk64, sizeof(sk64));
		fobj_short_id(pk, sid);
		if (g_cfg.nnetworks) snprintf(need[nn++], sizeof(need[0]), "fed/node/*");
		for (int n = 0; n < g_cfg.nnetworks; n++) {
			snprintf(need[nn++], sizeof(need[0]), "%.32s/nodes", g_cfg.networks[n].name);
			snprintf(need[nn++], sizeof(need[0]), "%.32s/mail/%s", g_cfg.networks[n].name, sid);
		}
		for (int i = 0; i < nn; i++) {
			char probe[sizeof(need[0])];
			memcpy(probe, need[i], sizeof(probe));		// the same size: the whole entry, its NUL with it
			size_t pl = strlen(probe);
			if (pl && probe[pl - 1] == '*') snprintf(probe + pl - 1, sizeof(probe) - pl + 1, "0000000000000000");
			if (fsess_wanted(g_cfg.wants, probe)) continue;
			size_t o = strlen(g_cfg.wants);
			if (o + strlen(need[i]) + 2 < sizeof(g_cfg.wants))
				snprintf(g_cfg.wants + o, sizeof(g_cfg.wants) - o, "%s%s", o ? "\n" : "", need[i]);
		}
	}
	memcpy(s, seed, 32);
	crypto_ed25519_key_pair(g_sk, g_pk, s);		// wipes s
	snprintf(p, sizeof(p), "%s/store", dir);
	int so = fstore_open(p, plat_now(), retain_for, 0);
	if (so == -FSTORE_E_FORMAT) {
		snprintf(err, (size_t)errlen, "the store in %s is from an older fed -- remove that directory and start fed again "
			"(this node's key is kept; its peers send everything again)", p);
		return -1;
	}
	if (so < 0) { snprintf(err, (size_t)errlen, "the store in %s would not open", p); return -1; }
	memset(g_slot, 0, sizeof(g_slot));
	memset(g_cl, 0, sizeof(g_cl));
	memset(g_lim, 0, sizeof(g_lim));
	memset(g_haslist, 0, sizeof(g_haslist));
	fnode_reload_lists();
	publish_node_info();
	radio_start();
	return 0;
}

void fnode_stop(void) {
	fstore_close();
	crypto_wipe(g_sk, sizeof(g_sk));
	crypto_wipe(&g_mail, sizeof(g_mail));
}

void fnode_tick(uint32_t now_ms) {
	g_now_ms = now_ms;
	if (g_radio_on) fradio_poll(now_ms);
	for (int i = 0; i < FNODE_CLIENTS; i++) if (g_cl[i].used) deliver(&g_cl[i]);
	if (!g_expire_ms || (int32_t)(now_ms - g_expire_ms) >= 0) {
		g_expire_ms = now_ms + 3600 * 1000;
		if (plat_now()) fstore_expire(plat_now());
	}
}
