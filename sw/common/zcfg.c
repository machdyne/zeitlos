/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zcfg.h and docs/config.md.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zcfg.h"

// -- known keys --
//
// Add a row when something starts reading a new key. docs/config.md
// lists the same keys with the full story; keep the two together.

const z_cfg_known_t z_cfg_known[] = {
	{ "apps.term.auto_connect", "",
	  "what a new term window connects to, e.g. port repl0 (empty: start panel)" },
	{ "system.rtc.timezone", "UTC",
	  "clock and cal: a city (Berlin, New York -- DST included), UTC, or UTC+2" },
	{ "system.video.mode", "white",
	  "display colour at boot and on reload: white, amber, green or paper" },
};

const int z_cfg_known_count = (int)(sizeof(z_cfg_known) / sizeof(z_cfg_known[0]));

const char *z_cfg_default(const char *key) {

	if (!key) return "";

	for (int i = 0; i < z_cfg_known_count; i++)
		if (!strcmp(z_cfg_known[i].key, key)) return z_cfg_known[i].def;

	return "";

}

// -- parsing --

static bool is_key_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static bool is_blank(char c) {
	return c == ' ' || c == '\t';
}

// Dotted, with the dot inside: see zcfg.h on why.
static bool key_dotted(const char *key, size_t n) {
	for (size_t i = 1; i + 1 < n; i++)
		if (key[i] == '.') return true;
	return false;
}

bool z_cfg_key_valid(const char *key) {

	size_t n = 0;

	if (!key || !*key) return false;

	for (; key[n]; n++)
		if (!is_key_char(key[n])) return false;

	return n < Z_CFG_KEY_MAX && key_dotted(key, n);

}

z_cfg_line_t z_cfg_parse_line(const char *line, size_t len,
	char *key, size_t keylen, char *val, size_t vallen) {

	size_t i = 0, k0, k1, v0, v1;

	if (keylen) key[0] = 0;
	if (vallen) val[0] = 0;

	// The line ends at a NUL, \n or \r, whichever comes first.
	for (size_t j = 0; j < len; j++)
		if (line[j] == 0 || line[j] == '\n' || line[j] == '\r') { len = j; break; }

	while (i < len && is_blank(line[i])) i++;

	if (i == len || line[i] == '#') return Z_CFG_LINE_BLANK;

	k0 = i;
	while (i < len && is_key_char(line[i])) i++;
	k1 = i;

	if (k1 == k0 || !key_dotted(line + k0, k1 - k0)) return Z_CFG_LINE_BAD;

	// Whatever follows the key must be a separator or the end -- so
	// "foo!bar" is a bad line rather than key "foo", value "!bar".
	if (i < len && !is_blank(line[i]) && line[i] != ':' && line[i] != '=')
		return Z_CFG_LINE_BAD;

	while (i < len && is_blank(line[i])) i++;
	if (i < len && (line[i] == ':' || line[i] == '=')) i++;
	while (i < len && is_blank(line[i])) i++;

	v0 = i;
	v1 = len;
	while (v1 > v0 && is_blank(line[v1 - 1])) v1--;

	if (k1 - k0 + 1 > keylen || k1 - k0 + 1 > Z_CFG_KEY_MAX) return Z_CFG_LINE_BAD;
	if (v1 - v0 + 1 > vallen || v1 - v0 + 1 > Z_CFG_VAL_MAX) return Z_CFG_LINE_BAD;

	memcpy(key, line + k0, k1 - k0);
	key[k1 - k0] = 0;
	memcpy(val, line + v0, v1 - v0);
	val[v1 - v0] = 0;

	return Z_CFG_LINE_ENTRY;

}

// -- editing --

static bool put(char *out, size_t outcap, size_t *n, const char *s, size_t len) {
	if (*n + len + 1 > outcap) return false;
	memcpy(out + *n, s, len);
	*n += len;
	out[*n] = 0;
	return true;
}

int z_cfg_text_set(const char *in, size_t inlen, const char *key,
	const char *val, char *out, size_t outcap) {

	char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX];
	size_t n = 0, pos = 0;
	bool written = false;

	if (!out || outcap == 0 || !z_cfg_key_valid(key)) return -1;
	if (val && strlen(val) + 1 > Z_CFG_VAL_MAX) return -1;
	for (const char *p = val; p && *p; p++)
		if (*p == '\n' || *p == '\r') return -1;

	out[0] = 0;
	if (!in) inlen = 0;

	while (pos < inlen) {

		// One line, INCLUDING its line ending, so an untouched line is
		// copied back exactly -- CRLF files stay CRLF.
		size_t start = pos;
		while (pos < inlen && in[pos] != '\n') pos++;
		if (pos < inlen) pos++;		// the \n

		size_t linelen = pos - start;

		if (z_cfg_parse_line(in + start, linelen, k, sizeof(k), v, sizeof(v))
			== Z_CFG_LINE_ENTRY && !strcmp(k, key)) {

			if (val && !written) {
				if (!put(out, outcap, &n, key, strlen(key)) ||
					!put(out, outcap, &n, ": ", 2) ||
					!put(out, outcap, &n, val, strlen(val)) ||
					!put(out, outcap, &n, "\n", 1))
					return -1;
				written = true;
			}
			continue;		// replaced, or a duplicate dropped

		}

		if (!put(out, outcap, &n, in + start, linelen)) return -1;

	}

	if (val && !written) {
		if (n > 0 && out[n - 1] != '\n' && !put(out, outcap, &n, "\n", 1))
			return -1;
		if (!put(out, outcap, &n, key, strlen(key)) ||
			!put(out, outcap, &n, ": ", 2) ||
			!put(out, outcap, &n, val, strlen(val)) ||
			!put(out, outcap, &n, "\n", 1))
			return -1;
	}

	return (int)n;

}

// -- app API --
//
// Left out of the kernel's build (-DZCFG_KERNEL): these call through
// reg_kernel, and the kernel reads its own store directly.

#ifndef ZCFG_KERNEL

#include "zobj.h"
#include "zeitlos.h"

static void copy_str(char *out, size_t outlen, const char *s) {
	size_t i = 0;
	if (!outlen) return;
	for (; s && s[i] && i < outlen - 1; i++) out[i] = s[i];
	out[i] = 0;
}

bool z_cfg_get(const char *key, char *out, size_t outlen) {

	z_cfg_get_args_t a;

	if (!out || !outlen) return false;
	out[0] = 0;

	memset(&a, 0, sizeof(a));
	a.key = key;
	a.val = out;
	a.vallen = (uint32_t)outlen;

	// found is only ever set by the kernel. An old kernel, or the
	// simulator, returns "ok" without touching it -- which reads,
	// correctly, as "not set".
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_CFG_GET, (uint32_t *)&a, 0);

	if (key && rv && rv->val.uint32 == Z_OK && a.found) return true;

	copy_str(out, outlen, z_cfg_default(key));
	return false;

}

bool z_cfg_get_bool(const char *key, bool def) {

	char v[16];
	z_cfg_get(key, v, sizeof(v));

	for (char *p = v; *p; p++)
		if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") ||
		!strcmp(v, "1"))
		return true;
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") ||
		!strcmp(v, "0"))
		return false;

	return def;

}

int32_t z_cfg_get_int(const char *key, int32_t def) {

	char v[24];
	const char *p = v;
	bool neg = false;
	int64_t n = 0;

	z_cfg_get(key, v, sizeof(v));

	if (*p == '-' || *p == '+') neg = (*p++ == '-');
	if (!*p) return def;

	for (; *p; p++) {
		if (*p < '0' || *p > '9') return def;
		n = n * 10 + (*p - '0');
		if (n > 0x7fffffffLL) return def;
	}

	return (int32_t)(neg ? -n : n);

}

uint32_t z_cfg_generation(void) {

	z_cfg_get_args_t a;
	memset(&a, 0, sizeof(a));

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_kernel_ptr(Z_SYS_CFG_GET, (uint32_t *)&a, 0);

	return a.generation;

}

bool z_cfg_entry(uint32_t index, char *key, size_t keylen,
	char *val, size_t vallen) {

	z_cfg_entry_args_t a;

	if (!key || !keylen || !val || !vallen) return false;
	key[0] = 0;
	val[0] = 0;

	memset(&a, 0, sizeof(a));
	a.index = index;
	a.key = key;
	a.keylen = (uint32_t)keylen;
	a.val = val;
	a.vallen = (uint32_t)vallen;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_CFG_ENTRY, (uint32_t *)&a, 0);

	return rv && rv->val.uint32 == Z_OK && a.found;

}

int z_cfg_reload(uint32_t *ignored) {

	z_cfg_reload_args_t a;
	memset(&a, 0, sizeof(a));

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_kernel_ptr(Z_SYS_CFG_RELOAD, (uint32_t *)&a, 0);

	if (ignored) *ignored = a.ignored;
	if (!a.done) return -1;

	return a.count;

}

#endif
