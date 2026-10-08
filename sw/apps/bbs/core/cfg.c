/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- <datadir>/bbs.cfg: "key: value" lines, # comments, the same
 * shape as /sys/zeitlos.cfg. The BBS keeps its own file rather than keys in
 * /sys/zeitlos.cfg because the same data directory must work on a Linux
 * server, which has no /sys/zeitlos.cfg. docs/bbs.md, "Configuration".
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "bbs_int.h"

bbs_cfg_t bbs_cfg;
char bbs_dir[BBS_PATH_MAX];

void cfg_defaults(bbs_cfg_t *c) {
	memset(c, 0, sizeof(*c));
	strcpy(c->name, "Zeitlos BBS");
	c->nodes = 4;
	c->new_users = true;
	c->idle_minutes = 15;
	c->login_seconds = 120;
	c->pw_iterations = 2000;
	c->new_level = 10;
}

static void copy_trim(char *dst, uint32_t cap, const char *s, uint32_t n) {
	while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
	if (n >= cap) n = cap - 1;
	memcpy(dst, s, n);
	dst[n] = 0;
}

static int clamp(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

void cfg_parse(bbs_cfg_t *c, const char *text, uint32_t len) {
	const char *p = text, *end = text + len;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol) eol = end;
		const char *colon = memchr(p, ':', (size_t)(eol - p));
		const char *hash = memchr(p, '#', (size_t)(eol - p));
		if (colon && (!hash || hash > colon)) {
			char key[32], val[128];
			copy_trim(key, sizeof(key), p, (uint32_t)(colon - p));
			// A comment after the value is a # with blanks on both
			// sides: "name: Board #1   # ours" is "Board #1".
			const char *vend = eol;
			for (const char *q = colon + 1; q < eol; q++)
				if (*q == '#' && (q[-1] == ' ' || q[-1] == '\t') &&
						(q + 1 == eol || q[1] == ' ' || q[1] == '\t' || q[1] == '\r')) { vend = q; break; }
			copy_trim(val, sizeof(val), colon + 1, (uint32_t)(vend - colon - 1));
			bool yes = !strcmp(val, "yes") || !strcmp(val, "on") || !strcmp(val, "true") || !strcmp(val, "1");
			if (!strcmp(key, "name") && val[0]) utf8_copy(c->name, sizeof(c->name), val);
			else if (!strcmp(key, "sysop")) utf8_copy(c->sysop, sizeof(c->sysop), val);
			else if (!strcmp(key, "nodes")) c->nodes = clamp(atoi(val), 1, BBS_NODES_MAX);
			else if (!strcmp(key, "new_users")) c->new_users = yes;
			else if (!strcmp(key, "idle_minutes")) c->idle_minutes = clamp(atoi(val), 1, 1440);
			else if (!strcmp(key, "login_seconds")) c->login_seconds = clamp(atoi(val), 20, 3600);
			else if (!strcmp(key, "pw_iterations")) c->pw_iterations = clamp(atoi(val), 1, 1000000);
			else if (!strcmp(key, "new_level")) c->new_level = clamp(atoi(val), 0, 254);
			else if (!strcmp(key, "fed")) snprintf(c->fed, sizeof(c->fed), "%s", val);
			else if (key[0]) bbs_logf("bbs.cfg: unknown key '%s' ignored", key);
		}
		p = eol + 1;
	}
}

bool bbs_path(char *out, const char *rel) {
	int k = snprintf(out, BBS_PATH_MAX, "%s/%s", bbs_dir, rel);
	return k > 0 && k < BBS_PATH_MAX;
}

bool cfg_load(void) {
	char path[BBS_PATH_MAX], text[2048];
	cfg_defaults(&bbs_cfg);
	if (!bbs_path(path, "bbs.cfg")) return false;
	int h = plat_open(path, PLAT_READ);
	if (h < 0) {
		bbs_logf("no %s: using the defaults", path);
		return true;
	}
	int n = plat_read(h, text, (int)sizeof(text) - 1);
	plat_close(h);
	if (n < 0) return false;
	cfg_parse(&bbs_cfg, text, (uint32_t)n);
	return true;
}

void bbs_logf(const char *fmt, ...) {
	char line[200];
	va_list ap;
	va_start(ap, fmt);
	int k = snprintf(line, sizeof(line), "bbs: ");
	vsnprintf(line + k, sizeof(line) - (size_t)k, fmt, ap);
	va_end(ap);
	plat_log(line);
}
