/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * net.cfg, line by line. See cfgfile.h.
 */

#include <string.h>
#include <stdlib.h>
#include "cfgfile.h"

/* the key of line l, into k (cap bytes): 1, or 0 for a comment or a
 * blank line or a line without = */
static int key_of(const char *l, char *k, int cap)
{
	const char *eq;
	int n;
	while (*l == ' ' || *l == '\t') l++;
	if (!*l || *l == '#') return 0;
	eq = strchr(l, '=');
	if (!eq) return 0;
	n = (int)(eq - l);
	while (n > 0 && (l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
	if (n <= 0 || n >= cap) return 0;
	memcpy(k, l, (size_t)n);
	k[n] = 0;
	return 1;
}

int cf_load(cf_t *c, const char *t)
{
	c->n = 0;
	if ((unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
		t += 3;
	while (*t) {
		const char *nl = strchr(t, '\n');
		int len = nl ? (int)(nl - t) : (int)strlen(t);
		if (len && t[len - 1] == '\r') len--;
		if (c->n >= CF_LINES || len >= CF_LINE) return -1;
		memcpy(c->line[c->n], t, (size_t)len);
		c->line[c->n][len] = 0;
		c->n++;
		if (!nl) break;
		t = nl + 1;
	}
	return 0;
}

const char *cf_get(const cf_t *c, const char *key)
{
	static char v[CF_LINE];
	char k[64];
	int i, found = 0;
	for (i = 0; i < c->n; i++) {
		if (!key_of(c->line[i], k, sizeof(k)) || strcmp(k, key)) continue;
		const char *p = strchr(c->line[i], '=') + 1;
		int n;
		while (*p == ' ' || *p == '\t') p++;
		strcpy(v, p);
		/* a comment after the value is not the value */
		{ char *h = strstr(v, " #"); if (h) *h = 0; }
		n = (int)strlen(v);
		while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = 0;
		found = 1;
	}
	return found ? v : 0;
}

static void drop(cf_t *c, int i)
{
	memmove(c->line[i], c->line[i + 1], (size_t)(c->n - i - 1) * CF_LINE);
	c->n--;
}

int cf_set(cf_t *c, const char *key, const char *value)
{
	char k[64];
	int i, first = -1;
	if (strlen(key) + strlen(value) + 4 >= CF_LINE) return -1;
	for (i = 0; i < c->n; i++) {
		if (!key_of(c->line[i], k, sizeof(k)) || strcmp(k, key)) continue;
		if (first < 0) first = i;
		else { drop(c, i); i--; }
	}
	if (first < 0) {
		if (c->n >= CF_LINES) return -1;
		first = c->n++;
	}
	strcpy(c->line[first], key);
	strcat(c->line[first], " = ");
	strcat(c->line[first], value);
	return 0;
}

void cf_unset(cf_t *c, const char *key)
{
	char k[64];
	int i;
	for (i = 0; i < c->n; i++)
		if (key_of(c->line[i], k, sizeof(k)) && !strcmp(k, key)) { drop(c, i); i--; }
}

void cf_unset_prefix(cf_t *c, const char *prefix)
{
	char k[64];
	int i;
	size_t n = strlen(prefix);
	for (i = 0; i < c->n; i++)
		if (key_of(c->line[i], k, sizeof(k)) && !strncmp(k, prefix, n)) { drop(c, i); i--; }
}

int cf_has_prefix(const cf_t *c, const char *prefix)
{
	char k[64];
	int i;
	size_t n = strlen(prefix);
	for (i = 0; i < c->n; i++)
		if (key_of(c->line[i], k, sizeof(k)) && !strncmp(k, prefix, n)) return 1;
	return 0;
}

int cf_save(const cf_t *c, char *out, int cap)
{
	int i, n = 0;
	for (i = 0; i < c->n; i++) {
		int l = (int)strlen(c->line[i]);
		if (n + l + 1 >= cap) return -1;
		memcpy(out + n, c->line[i], (size_t)l);
		n += l;
		out[n++] = '\n';
	}
	out[n] = 0;
	return n;
}

int cf_valid_ip(const char *s, int allow_prefix)
{
	int part = 0, digits = 0, val = 0;
	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			val = val * 10 + (*s - '0');
			if (++digits > 3 || val > 255) return 0;
		} else if (*s == '.' && part < 3 && digits) {
			part++; digits = 0; val = 0;
		} else if ((*s == 0 || *s == '/') && part == 3 && digits) {
			if (*s == '/') {
				int p;
				if (!allow_prefix || !s[1]) return 0;
				for (p = 1; s[p]; p++) if (s[p] < '0' || s[p] > '9' || p > 2) return 0;
				return atoi(s + 1) <= 32;
			}
			return 1;
		} else {
			return 0;
		}
	}
}
