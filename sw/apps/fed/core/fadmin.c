/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * `fed key`, `fed nodes`, `fed add`, `fed set`, `fed remove`: a network's
 * node list, managed from the command line -- never JSON by hand; on
 * Linux and on Zeitlos alike. docs/fed.md, "Managing a network".
 *
 * The list is read from the running fed (LIST, on its local interface)
 * and EDITED AS TEXT: an entry added before the end of "nodes", one cut
 * out, one field's value replaced. Everything else -- the profile, the
 * moderators, fields a later version adds -- stays exactly as it was.
 * Then it is checked as members will check it (fnet_parse) and
 * published through fed (PUB).
 *
 * Small on purpose: one 16 KB buffer, the list's own text, edited in
 * place; the parser's tokens and a list to check into are fnet's shared
 * ones (fnet.h), since a command never runs beside the node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "fadmin.h"
#include "fnode.h"
#include "fnet.h"
#include "fobj.h"
#include "../../../common/zjson.h"

static const fadmin_io_t *io;
static char g_js[FOBJ_PAYLOAD_MAX + 1];			// the list's text; fed.cfg's before it
static int g_len;
static zjson_tok_t *g_tok;
static int g_tokcap, g_nt;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	io->out(line, io->ctx);
}

static int request(const char *req, const uint8_t *payload, int plen, char *line, int lcap, uint8_t *data, int dcap) {
	int n = io->request(req, payload, plen, line, lcap, data, dcap, io->ctx);
	if (n < 0) say("fed: fed is not running -- start it first (%s)", io->how_to_start);
	return n;
}

// -- the list's text --

static bool parse(void) {
	g_tok = fnet_tokens(&g_tokcap);
	g_nt = zjson_parse(g_js, (uint32_t)g_len, g_tok, (uint32_t)g_tokcap, NULL);
	return g_nt >= 1 && g_tok[0].type == ZJ_OBJECT;
}

// A field of an object, copied; "" if absent.
static void field(int obj, const char *k, char *out, int cap) {
	int f = zjson_get(g_js, g_tok, obj, k);
	out[0] = 0;
	if (f >= 0) zjson_str(g_js, &g_tok[f], out, (uint32_t)cap);
}

static int load_list(const char *net) {
	char req[80], line[200];
	snprintf(req, sizeof(req), "LIST %s\n", net);
	int n = request(req, NULL, 0, line, sizeof(line), (uint8_t *)g_js, FOBJ_PAYLOAD_MAX);
	if (n < 0) return -1;
	if (!strcmp(line, "ERR ?")) {
		// the running fed does not know LIST: it started before this one was installed
		say("fed: the running fed is older than this command -- restart it (%s), then try again", io->how_to_restart);
		return -1;
	}
	if (strncmp(line, "OK", 2)) { say("fed: %s", line); return -1; }
	g_len = n;
	g_js[n] = 0;
	if (n && !parse()) { say("fed: the current list does not parse"); return -1; }
	return n;
}

// The nodes array's token, and each entry's; -1 if none.
static int nodes_arr(void) { return g_len ? zjson_get(g_js, g_tok, 0, "nodes") : -1; }

static void hexkey(int obj, uint8_t k[32]) {
	char h[65];
	field(obj, "key", h, sizeof(h));
	memset(k, 0, 32);
	for (int j = 0; j < 32 && h[2 * j] && h[2 * j + 1]; j++) {
		unsigned v;
		sscanf(h + 2 * j, "%2x", &v);
		k[j] = (uint8_t)v;
	}
}

// An entry by name, key or short id: its object's token, or -1.
static int find(const char *who) {
	int arr = nodes_arr(), t;
	if (arr < 0) return -1;
	t = arr + 1;
	for (uint32_t i = 0; i < g_tok[arr].size; i++, t = zjson_next(g_tok, t)) {
		char name[FNET_NAME_MAX + 2], key[70], sid[17];
		uint8_t k[32];
		field(t, "name", name, sizeof(name));
		field(t, "key", key, sizeof(key));
		hexkey(t, k);
		fobj_short_id(k, sid);
		if (!strcmp(name, who) || !strcmp(key, who) || !strcmp(sid, who)) return t;
	}
	return -1;
}

// Replace [a, b) with s: the text shifts in place.
static bool splice(uint32_t a, uint32_t b, const char *s) {
	int sl = (int)strlen(s), grow = sl - (int)(b - a);
	if (g_len + grow > FOBJ_PAYLOAD_MAX) { say("fed: the list would be over 16 KB"); return false; }
	memmove(g_js + b + grow, g_js + b, (size_t)(g_len - (int)b));
	memcpy(g_js + a, s, (size_t)sl);
	g_len += grow;
	g_js[g_len] = 0;
	return parse();
}

// A JSON string: what a name, sysop or address may hold needs only the
// quote and the backslash escaped (fnet refuses control characters).
static void jstr(char *o, int cap, const char *s) {
	int n = 0;
	o[n++] = '"';
	for (; *s && n < cap - 3; s++) {
		if (*s == '"' || *s == '\\') o[n++] = '\\';
		o[n++] = *s;
	}
	o[n++] = '"';
	o[n] = 0;
}

// One field of one entry set: its value replaced, or the field added.
static bool set_field(const char *who, const char *k, const char *v) {
	char q[160];
	int t = find(who), f;
	if (t < 0) return false;
	jstr(q, sizeof(q), v);
	if ((f = zjson_get(g_js, g_tok, t, k)) >= 0)
		return splice(g_tok[f].start - 1, g_tok[f].end + 1, q);		// the quotes too
	char add[200];
	snprintf(add, sizeof(add), ", \"%s\": %s", k, q);
	return splice(g_tok[t].end - 1, g_tok[t].end - 1, add);			// before its '}'
}

static bool add_entry(const char *key, const char *name) {
	char e[200];
	int arr = nodes_arr();
	if (arr < 0) return false;
	snprintf(e, sizeof(e), "%s{\"key\": \"%s\", \"name\": \"%s\"}", g_tok[arr].size ? ",\n  " : "\n  ", key, name);
	return splice(g_tok[arr].end - 1, g_tok[arr].end - 1, e);			// before its ']'
}

static bool remove_entry(int t) {
	int arr = nodes_arr(), prev = -1, next = -1, u = arr + 1;
	for (uint32_t i = 0; i < g_tok[arr].size; i++, u = zjson_next(g_tok, u)) {
		if (u == t) { if (i + 1 < g_tok[arr].size) next = zjson_next(g_tok, u); break; }
		prev = u;
	}
	// with the comma on one side of it, so the array stays well formed
	if (prev >= 0) return splice(g_tok[prev].end, g_tok[t].end, "");
	if (next >= 0) return splice(g_tok[t].start, g_tok[next].start, "");
	return splice(g_tok[t].start, g_tok[t].end, "");
}

// Checked as members will check it, then published through fed.
static int publish(const char *net) {
	char err[160], req[160], line[200];
	fnet_list_t *check = fnet_scratch();
	if (fnet_parse(net, (const uint8_t *)g_js, (uint32_t)g_len, check, err, sizeof(err))) {
		say("fed: the list would be refused: %s -- nothing published", err);
		return 1;
	}
	int n = check->n;
	snprintf(req, sizeof(req), "PUB %s/nodes fed.nodes json state list %d\n", net, g_len);
	if (request(req, (const uint8_t *)g_js, g_len, line, sizeof(line), NULL, 0) < 0) return 1;
	if (strncmp(line, "OK", 2)) { say("fed: fed refused it: %s", line); return 1; }
	say("published: %s now has %d node%s", net, n, n == 1 ? "" : "s");
	return 0;
}

// -- this node --

static bool me(uint8_t pk[32], char hex[65], fcfg_t *cfg) {
	char err[160];
	if (!io->public_key(pk, io->ctx)) { say("fed: no node key, and none could be made here"); return false; }
	fobj_hex(pk, 32, hex);
	if (!cfg) return true;
	// fed.cfg into the list's buffer: read before any list is
	if (!io->read_cfg(g_js, sizeof(g_js), io->ctx)) { say("fed: cannot read fed.cfg"); return false; }
	if (!fnode_config(g_js, cfg, err, sizeof(err))) { say("fed: fed.cfg: %s", err); return false; }
	return true;
}

// The network to work on: named, or the only one, or the one this node publishes.
static int pick(const fcfg_t *cfg, const uint8_t pk[32], const char *want) {
	int found = -1, mine = -1, count = 0;
	for (int i = 0; i < cfg->nnetworks; i++) {
		if (want && !strcmp(cfg->networks[i].name, want)) found = i;
		if (!memcmp(cfg->networks[i].publisher, pk, 32)) { mine = i; count++; }
	}
	if (want) { if (found < 0) say("fed: fed.cfg has no network called %s", want); return found; }
	if (cfg->nnetworks == 1) return 0;
	if (count == 1) return mine;
	say("fed: fed.cfg names %d networks -- say which: --network NAME", cfg->nnetworks);
	return -1;
}

static bool is_key(const char *s) {
	if (strlen(s) != 64) return false;
	for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
	return true;
}

// sysop=, addr=, name= words: each set on the entry `who`.
static bool fields(const char *who, int argc, char **argv, int from) {
	char name[FNET_NAME_MAX + 2];
	snprintf(name, sizeof(name), "%s", who);
	for (int i = from; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		char k[8];
		if (!eq || eq - argv[i] > 6) { say("fed: '%s' -- sysop=NAME, addr=HOST:PORT, name=NAME", argv[i]); return false; }
		snprintf(k, sizeof(k), "%.*s", (int)(eq - argv[i]), argv[i]);
		if (strcmp(k, "sysop") && strcmp(k, "addr") && strcmp(k, "name")) { say("fed: '%s' -- sysop=NAME, addr=HOST:PORT, name=NAME", argv[i]); return false; }
		if (!strcmp(k, "name") && !fnet_name_ok(eq + 1)) { say("fed: '%s': a name is 1-32 of a-z 0-9 -", eq + 1); return false; }
		if (!strcmp(k, "name") && find(eq + 1) >= 0) { say("fed: %s is on the list already", eq + 1); return false; }
		if (!set_field(name, k, eq + 1)) return false;
		if (!strcmp(k, "name")) snprintf(name, sizeof(name), "%s", eq + 1);	// found by its new name next
	}
	return true;
}

// -- the commands --

bool fadmin_is_command(const char *word) {
	static const char *const cmds[] = { "key", "nodes", "add", "set", "remove" };
	for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) if (!strcmp(word, cmds[i])) return true;
	return false;
}

int fadmin(const fadmin_io_t *the_io, const char *network, int argc, char **argv) {
	static fcfg_t cfg;
	uint8_t pk[32];
	char hex[65], sid[17];
	const char *cmd = argv[0];
	io = the_io;

	if (!strcmp(cmd, "key")) {
		if (!me(pk, hex, NULL)) return 1;
		fobj_short_id(pk, sid);
		say("This node's PUBLIC key -- safe to share; it is how other nodes know this one:");
		say("%s", hex);
		say("Short id: %s", sid);
		say("(Its PRIVATE key is %s -- never share it; back it up.)", io->private_key_where);
		return 0;
	}
	if (!me(pk, hex, &cfg)) return 1;
	fobj_short_id(pk, sid);
	if (!cfg.nnetworks) { say("fed: fed.cfg follows no network (a line: network: NAME PUBLISHER-KEY)"); return 1; }

	if (!strcmp(cmd, "nodes")) {
		for (int i = 0; i < cfg.nnetworks; i++) {
			const char *net = cfg.networks[i].name;
			if (network && strcmp(network, net)) continue;
			bool publisher = !memcmp(cfg.networks[i].publisher, pk, 32);
			if (load_list(net) < 0) return 1;
			int arr = nodes_arr(), n = arr >= 0 ? (int)g_tok[arr].size : 0;
			say("%s -- %d node%s; its list is published by %s", net, n, n == 1 ? "" : "s", publisher ? "this node" : "another node");
			if (!n) { say("  (no list yet%s)", publisher ? ": `fed add NAME KEY` makes one" : ""); continue; }
			say("  %-20s %-17s %-16s %s", "name", "short id", "sysop", "address");
			int t = arr + 1;
			for (int k = 0; k < n; k++, t = zjson_next(g_tok, t)) {
				char name[FNET_NAME_MAX + 2], sysop[FNET_SYSOP_MAX + 2], addr[FNET_ADDR_MAX + 2], key[70], s2[17];
				uint8_t kb[32];
				field(t, "name", name, sizeof(name));
				field(t, "sysop", sysop, sizeof(sysop));
				field(t, "addr", addr, sizeof(addr));
				field(t, "key", key, sizeof(key));
				hexkey(t, kb);
				fobj_short_id(kb, s2);
				say("  %-20s %-17s %-16s %s%s", name[0] ? name : "-", s2, sysop[0] ? sysop : "-", addr[0] ? addr : "-",
					!strcmp(key, hex) ? "   (this node)" : "");
			}
		}
		return 0;
	}

	int ni = pick(&cfg, pk, network);
	if (ni < 0) return 1;
	const char *net = cfg.networks[ni].name;
	if (memcmp(cfg.networks[ni].publisher, pk, 32)) {
		say("fed: this node does not publish %s's list -- its publisher adds and removes nodes", net);
		return 1;
	}
	if (load_list(net) < 0) return 1;

	if (!strcmp(cmd, "add")) {
		if (argc < 3) { say("usage: fed add NAME PUBLIC-KEY [sysop=NAME] [addr=HOST:PORT]"); return 2; }
		const char *name = argv[1], *key = argv[2];
		if (!fnet_name_ok(name)) { say("fed: '%s': a name is 1-32 of a-z 0-9 -", name); return 1; }
		if (!is_key(key)) { say("fed: '%s' is not a public key: 64 characters of 0-9 a-f", key); return 1; }
		if (find(name) >= 0) { say("fed: %s is on the list already -- `fed set %s ...` to change it", name, name); return 1; }
		int dup = find(key);
		if (dup >= 0) { char other[FNET_NAME_MAX + 2]; field(dup, "name", other, sizeof(other)); say("fed: that key is on the list already, as %s", other[0] ? other : "a node with no name"); return 1; }
		// a network's first list: this node on it, first
		if (!g_len) {
			const char *self = cfg.name[0] && fnet_name_ok(cfg.name) ? cfg.name : sid;
			g_len = snprintf(g_js, sizeof(g_js), "{\"network\": \"%s\",\n \"nodes\": [\n  {\"key\": \"%s\", \"name\": \"%s\"}]}\n", net, hex, self);
			if (!parse()) return 1;
			say("%s's first list: this node on it too, as %s", net, self);
		}
		if (!add_entry(key, name) || !fields(name, argc, argv, 3)) return 2;
		return publish(net);
	}
	if (!strcmp(cmd, "set")) {
		if (argc < 3) { say("usage: fed set NAME [sysop=NAME] [addr=HOST:PORT] [name=NEW]"); return 2; }
		if (find(argv[1]) < 0) { say("fed: no node %s on %s's list", argv[1], net); return 1; }
		if (!fields(argv[1], argc, argv, 2)) return 2;
		return publish(net);
	}
	if (!strcmp(cmd, "remove")) {
		if (argc < 2) { say("usage: fed remove NAME"); return 2; }
		int t = find(argv[1]);
		char key[70], name[FNET_NAME_MAX + 2];
		if (t < 0) { say("fed: no node %s on %s's list", argv[1], net); return 1; }
		field(t, "key", key, sizeof(key));
		field(t, "name", name, sizeof(name));
		if (!strcmp(key, hex)) { say("fed: this node publishes the list -- it stays on it"); return 1; }
		say("removing %s from %s", name[0] ? name : key, net);
		if (!remove_entry(t)) return 1;
		return publish(net);
	}
	say("fed: '%s'? -- key, nodes, add, set, remove", cmd);
	return 2;
}
