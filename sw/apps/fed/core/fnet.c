/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed node lists. docs/fed.md, "Networks"; the format in fnet.h.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fnet.h"
#include "fobj.h"
#include "../../../common/zjson.h"

// 200 nodes of up to four fields: ~1,800 tokens. 32 bytes each.
#define FNET_TOKENS 2048
static zjson_tok_t g_tok[FNET_TOKENS];

bool fnet_name_ok(const char *name) {
	size_t n = strlen(name);
	if (n < 1 || n > FNET_NAME_MAX) return false;
	for (size_t i = 0; i < n; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
	}
	return true;
}

bool fnet_addr_ok(const char *addr) {
	const char *colon = strrchr(addr, ':');
	size_t n = strlen(addr);
	if (!colon || colon == addr || n > FNET_ADDR_MAX) return false;
	for (const char *p = addr; p < colon; p++) {
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
	}
	const char *q = colon + 1;
	if (!*q || strlen(q) > 5) return false;
	for (const char *p = q; *p; p++) if (*p < '0' || *p > '9') return false;
	long port = strtol(q, NULL, 10);
	return port >= 1 && port <= 65535 && q[0] != '0';
}

static bool hexkey(const char *s, uint8_t out[32]) {
	if (strlen(s) != 64) return false;
	for (int i = 0; i < 64; i++) {
		char c = s[i];
		int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
		if (d < 0) return false;				// lowercase only, as everywhere in zfed
		if (i & 1) out[i / 2] |= (uint8_t)d; else out[i / 2] = (uint8_t)(d << 4);
	}
	return true;
}

#define FAIL(...) do { snprintf(err, (size_t)errlen, __VA_ARGS__); return -1; } while (0)

static bool pattern_ok(const char *p, const char *network);
static char g_names[FNET_MAX_NODES][FNET_NAME_MAX + 1];		// while a list is parsed

int fnet_parse(const char *network, const uint8_t *json, uint32_t len, fnet_list_t *out, char *err, int errlen) {
	char buf[FNET_ADDR_MAX + 2];
	uint32_t where;
	memset(out, 0, sizeof(*out));
	int nt = zjson_parse((const char *)json, len, g_tok, FNET_TOKENS, &where);
	if (nt < 0) FAIL("not strict JSON: %s at byte %lu", zjson_strerror(nt), (unsigned long)where);
	if (g_tok[0].type != ZJ_OBJECT) FAIL("not a JSON object");
	int v = zjson_get((const char *)json, g_tok, 0, "network");
	if (v < 0 || !zjson_str((const char *)json, &g_tok[v], buf, sizeof(buf))) FAIL("no \"network\"");
	if (strcmp(buf, network)) FAIL("a list for \"%s\", on %s's topic", buf, network);
	snprintf(out->network, sizeof(out->network), "%s", network);
	int arr = zjson_get((const char *)json, g_tok, 0, "nodes");
	if (arr < 0 || g_tok[arr].type != ZJ_ARRAY) FAIL("no \"nodes\" array");
	if (g_tok[arr].size > FNET_MAX_NODES) FAIL("%u nodes: at most %d", (unsigned)g_tok[arr].size, FNET_MAX_NODES);
	int t = arr + 1;
	for (uint32_t i = 0; i < g_tok[arr].size; i++, t = zjson_next(g_tok, t)) {
		if (g_tok[t].type != ZJ_OBJECT) FAIL("node %u: not an object", (unsigned)i);
		int k = zjson_get((const char *)json, g_tok, t, "key");
		if (k < 0 || !zjson_str((const char *)json, &g_tok[k], buf, sizeof(buf)) || !hexkey(buf, out->key[i]))
			FAIL("node %u: no key, or not 64 lowercase hex digits", (unsigned)i);
		for (int j = 0; j < (int)i; j++)
			if (!memcmp(out->key[j], out->key[i], 32)) FAIL("node %u: the same key as node %d", (unsigned)i, j);
		int f = zjson_get((const char *)json, g_tok, t, "name");
		g_names[i][0] = 0;
		if (f >= 0) {
			char name[FNET_NAME_MAX + 2];
			if (!zjson_str((const char *)json, &g_tok[f], name, sizeof(name)) || !fnet_name_ok(name))
				FAIL("node %u: a name is 1-%d bytes of a-z 0-9 -", (unsigned)i, FNET_NAME_MAX);
			// one name, one node: users are handle@name, and letters are
			// addressed that way -- two nodes called the same would make
			// both ambiguous
			for (int j = 0; j < (int)i; j++)
				if (!strcmp(g_names[j], name)) FAIL("node %u: the name \"%s\", as node %d has", (unsigned)i, name, j);
			memcpy(g_names[i], name, strlen(name) + 1);
		}
		f = zjson_get((const char *)json, g_tok, t, "sysop");
		if (f >= 0) {
			char sysop[FNET_SYSOP_MAX + 2];
			if (!zjson_str((const char *)json, &g_tok[f], sysop, sizeof(sysop)) || !sysop[0] || strlen(sysop) > FNET_SYSOP_MAX)
				FAIL("node %u: a sysop is a string of 1-%d bytes", (unsigned)i, FNET_SYSOP_MAX);
			for (const char *c = sysop; *c; c++)			// shown on screens: no control characters
				if ((unsigned char)*c < 0x20 || *c == 0x7f) FAIL("node %u: a control character in the sysop", (unsigned)i);
		}
		f = zjson_get((const char *)json, g_tok, t, "addr");
		if (f >= 0) {
			if (!zjson_str((const char *)json, &g_tok[f], buf, sizeof(buf)) || !fnet_addr_ok(buf))
				FAIL("node %u: an addr is host:port", (unsigned)i);
			memcpy(out->addr[i], buf, strlen(buf) + 1);		// fnet_addr_ok(): at most FNET_ADDR_MAX
		}
		out->n = (int)i + 1;
	}
	// the profile: optional
	int pr = zjson_get((const char *)json, g_tok, 0, "profile");
	if (pr >= 0) {
		if (g_tok[pr].type != ZJ_OBJECT) FAIL("\"profile\" is not an object");
		int m = zjson_get((const char *)json, g_tok, pr, "max_object");
		if (m >= 0) {
			if (g_tok[m].type != ZJ_NUMBER || g_tok[m].num < 512 || g_tok[m].num > FOBJ_MAX)
				FAIL("\"max_object\" is 512-%d bytes", FOBJ_MAX);
			out->max_object = (uint32_t)g_tok[m].num;
		}
		int su = zjson_get((const char *)json, g_tok, pr, "suite");
		if (su >= 0) {
			if (!zjson_str((const char *)json, &g_tok[su], buf, sizeof(buf)) || (strcmp(buf, "hybrid") && strcmp(buf, "classical")))
				FAIL("\"suite\" is \"hybrid\" or \"classical\"");
			out->classical = !strcmp(buf, "classical");
		}
	}
	// moderators: optional; each a key and 1-8 patterns inside this network
	int mo = zjson_get((const char *)json, g_tok, 0, "moderators");
	if (mo >= 0) {
		if (g_tok[mo].type != ZJ_ARRAY) FAIL("\"moderators\" is not an array");
		if (g_tok[mo].size > FNET_MODS) FAIL("%u moderators: at most %d", (unsigned)g_tok[mo].size, FNET_MODS);
		t = mo + 1;
		for (uint32_t i = 0; i < g_tok[mo].size; i++, t = zjson_next(g_tok, t)) {
			if (g_tok[t].type != ZJ_OBJECT) FAIL("moderator %u: not an object", (unsigned)i);
			int k = zjson_get((const char *)json, g_tok, t, "key");
			if (k < 0 || !zjson_str((const char *)json, &g_tok[k], buf, sizeof(buf)) || !hexkey(buf, out->mod_key[i]))
				FAIL("moderator %u: no key, or not 64 lowercase hex digits", (unsigned)i);
			int tp = zjson_get((const char *)json, g_tok, t, "topics");
			if (tp < 0 || g_tok[tp].type != ZJ_ARRAY || g_tok[tp].size < 1 || g_tok[tp].size > 8)
				FAIL("moderator %u: \"topics\" is 1-8 patterns", (unsigned)i);
			char *pats = out->mod_pats[i];
			size_t o = 0;
			pats[0] = 0;
			int u = tp + 1;
			for (uint32_t j = 0; j < g_tok[tp].size; j++, u = zjson_next(g_tok, u)) {
				char pat[FNET_MOD_PATS];
				if (!zjson_str((const char *)json, &g_tok[u], pat, sizeof(pat)) || !pattern_ok(pat, network))
					FAIL("moderator %u: a pattern is a topic in %s, or one ending /*", (unsigned)i, network);
				if (o + strlen(pat) + 2 > FNET_MOD_PATS) FAIL("moderator %u: patterns too long together", (unsigned)i);
				o += (size_t)snprintf(pats + o, FNET_MOD_PATS - o, "%s%s", o ? "\n" : "", pat);
			}
			out->nmods = (int)i + 1;
		}
	}
	return 0;
}

// A pattern a moderator may have: a topic of this network, or one ending /*.
static bool pattern_ok(const char *p, const char *network) {
	size_t nl = strlen(network), pl = strlen(p);
	if (pl < nl + 2 || pl > 96 || strncmp(p, network, nl) || p[nl] != '/') return false;
	for (size_t i = 0; i < pl; i++) {
		char c = p[i];
		if (c == '*') { if (i != pl - 1 || p[i - 1] != '/') return false; continue; }
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-')) return false;
		if (c == '/' && (p[i + 1] == '/' || i == pl - 1)) return false;
	}
	return true;
}

static bool pat_match(const char *pat, size_t pl, const char *topic) {
	if (pl >= 2 && pat[pl - 1] == '*') return strlen(topic) >= pl && !strncmp(topic, pat, pl - 1);	// "a/b/*": below a/b
	return strlen(topic) == pl && !strncmp(topic, pat, pl);
}

bool fnet_moderates(const fnet_list_t *l, const uint8_t key[32], const char *topic) {
	for (int i = 0; i < l->nmods; i++) {
		if (memcmp(l->mod_key[i], key, 32)) continue;
		for (const char *p = l->mod_pats[i]; *p; ) {
			const char *e = strchr(p, '\n');
			size_t pl = e ? (size_t)(e - p) : strlen(p);
			if (pat_match(p, pl, topic)) return true;
			if (!e) break;
			p = e + 1;
		}
	}
	return false;
}

bool fnet_has(const fnet_list_t *l, const uint8_t key[32], const char **addr) {
	for (int i = 0; i < l->n; i++)
		if (!memcmp(l->key[i], key, 32)) { if (addr) *addr = l->addr[i]; return true; }
	return false;
}

bool fnet_find_name(const uint8_t *json, uint32_t len, const char *name, uint8_t key[32]) {
	char buf[FNET_ADDR_MAX + 2];
	if (zjson_parse((const char *)json, len, g_tok, FNET_TOKENS, NULL) < 1 || g_tok[0].type != ZJ_OBJECT) return false;
	int arr = zjson_get((const char *)json, g_tok, 0, "nodes");
	if (arr < 0 || g_tok[arr].type != ZJ_ARRAY) return false;
	int t = arr + 1;
	for (uint32_t i = 0; i < g_tok[arr].size; i++, t = zjson_next(g_tok, t)) {
		int f = zjson_get((const char *)json, g_tok, t, "name"), k = zjson_get((const char *)json, g_tok, t, "key");
		if (f < 0 || k < 0 || !zjson_str((const char *)json, &g_tok[f], buf, sizeof(buf)) || strcmp(buf, name)) continue;
		return zjson_str((const char *)json, &g_tok[k], buf, sizeof(buf)) && hexkey(buf, key);
	}
	return false;
}
