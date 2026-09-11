/*
 * Zeitlos OS
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See cfg.h, sw/common/zcfg.h and docs/config.md.
 */

#include <stdio.h>
#include <string.h>

#include "../common/zeitlos.h"
#include "../common/zsoc.h"
#include "../common/zcfg.h"
#include "kernel.h"
#include "cfg.h"
#include "fs/fs.h"

// -- the store --
//
// One arena of "key\0value\0" strings and a table of offsets into it.
//
// Sized for what a config file is, not for what it could be: 48
// settings and 2KB of text is several times what this tree reads. It is
// kernel .bss, and kernel .bss is FLASH as well as RAM -- kernel.bin is
// padded to _end (docs/kernel.md, "The 256KB image budget") -- so every
// byte here is paid twice. A file that overflows it loads what fits and
// says how much it dropped.
#define K_CFG_ARENA   2048
#define K_CFG_ENTRIES 48

static char k_cfg_arena[K_CFG_ARENA];
static uint16_t k_cfg_used;

static struct { uint16_t key, val; } k_cfg_ent[K_CFG_ENTRIES];
static uint16_t k_cfg_n;

static uint32_t k_cfg_gen;

static const char *k_cfg_str(uint16_t off) { return k_cfg_arena + off; }

static bool arena_add(const char *s, uint16_t *off) {
	size_t len = strlen(s) + 1;
	if (k_cfg_used + len > K_CFG_ARENA) return false;
	memcpy(k_cfg_arena + k_cfg_used, s, len);
	*off = k_cfg_used;
	k_cfg_used = (uint16_t)(k_cfg_used + len);
	return true;
}

// Last one wins: a repeated key keeps its first position in the table
// (so `cfg` lists it where it first appeared) and takes the new value.
// The old value's bytes stay in the arena until the next load.
static bool store_set(const char *key, const char *val) {

	for (uint16_t i = 0; i < k_cfg_n; i++) {
		if (!strcmp(k_cfg_str(k_cfg_ent[i].key), key))
			return arena_add(val, &k_cfg_ent[i].val);
	}

	if (k_cfg_n >= K_CFG_ENTRIES) return false;

	uint16_t ko, vo;
	uint16_t mark = k_cfg_used;
	if (!arena_add(key, &ko) || !arena_add(val, &vo)) {
		k_cfg_used = mark;
		return false;
	}

	k_cfg_ent[k_cfg_n].key = ko;
	k_cfg_ent[k_cfg_n].val = vo;
	k_cfg_n++;

	return true;

}

const char *k_cfg_find(const char *key) {
	if (!key) return NULL;
	for (uint16_t i = 0; i < k_cfg_n; i++)
		if (!strcmp(k_cfg_str(k_cfg_ent[i].key), key))
			return k_cfg_str(k_cfg_ent[i].val);
	return NULL;
}

uint32_t k_cfg_count(void) { return k_cfg_n; }
uint32_t k_cfg_generation(void) { return k_cfg_gen; }

bool k_cfg_at(uint32_t index, const char **key, const char **val) {
	if (index >= k_cfg_n) return false;
	*key = k_cfg_str(k_cfg_ent[index].key);
	*val = k_cfg_str(k_cfg_ent[index].val);
	return true;
}

// -- settings the kernel applies itself --
//
// Only when the file SETS them. A key absent from the file leaves the
// hardware as it is rather than forcing the default back on it -- so
// removing system.video.mode and reloading does not flip a screen
// someone just changed with `color`. At power-on "as it is" is the
// default anyway.
static void apply(bool verbose) {

	const char *mode = k_cfg_find("system.video.mode");

	if (mode) {
		// Case-sensitive, as `color` is: a config file is as typed.
		uint32_t m = z_video_mode_from_name(mode);
		if (m >= Z_VIDEO_MODE_COUNT) {
			printf("cfg: system.video.mode: '%s' is not white, amber, "
				"green or paper -- ignored\n", mode);
		} else if (!z_video_mode_present()) {
			if (verbose)
				printf("cfg: system.video.mode: no video mode register in "
					"this bitstream\n");
		} else {
			z_video_set_mode(m);
		}
	}

}

// -- loading --

int k_cfg_load(bool verbose, uint32_t *ignored) {

	static char line[Z_CFG_LINE_MAX];
	char chunk[256];
	char key[Z_CFG_KEY_MAX], val[Z_CFG_VAL_MAX];
	uint32_t bad = 0, dropped = 0, lineno = 0;
	size_t ll = 0;
	bool overlong = false;
	FIL f;

	// No preemption for the whole load, not just around each FatFs
	// call: an app reading the store between "cleared" and "filled"
	// would see every setting at its default for a moment. Capped by
	// K_NO_PREEMPT_MAX_TICKS (kernel.c), and a config file is far
	// smaller than that cap is generous.
	k_fs_enter();

	k_cfg_used = 0;
	k_cfg_n = 0;
	k_cfg_gen++;

	// fs_size() first, because fs_open_read() prints an error for a
	// missing file -- and a missing config file is the normal state of
	// a fresh card, not something to alarm anyone about at boot.
	if (fs_size(Z_CFG_PATH) == 0 || !fs_open_read(&f, Z_CFG_PATH)) {
		k_fs_leave();
		if (verbose)
			printf("cfg: no %s -- using defaults\n", Z_CFG_PATH);
		if (ignored) *ignored = 0;
		return 0;
	}

	for (;;) {

		int32_t n = fs_read_chunk(&f, chunk, sizeof(chunk));
		bool eof = (n <= 0);

		for (int32_t i = 0; i <= (eof ? 0 : n - 1); i++) {

			char c;

			if (eof) {
				if (ll == 0 && !overlong) break;
				c = '\n';		// a last line with no newline
			} else {
				c = chunk[i];
			}

			if (c != '\n') {
				if (ll < sizeof(line)) line[ll++] = c;
				else overlong = true;
				continue;
			}

			lineno++;

			if (overlong) {
				bad++;
				printf("cfg: %s:%lu: line longer than %d bytes -- ignored\n",
					Z_CFG_PATH, (unsigned long)lineno, Z_CFG_LINE_MAX);
			} else {
				z_cfg_line_t t = z_cfg_parse_line(line, ll, key, sizeof(key),
					val, sizeof(val));
				if (t == Z_CFG_LINE_ENTRY) {
					if (!store_set(key, val)) dropped++;
				} else if (t == Z_CFG_LINE_BAD) {
					bad++;
					printf("cfg: %s:%lu: not a setting -- ignored\n",
						Z_CFG_PATH, (unsigned long)lineno);
				}
			}

			ll = 0;
			overlong = false;

		}

		if (eof) break;

	}

	fs_close_read(&f);

	apply(verbose);

	k_fs_leave();

	if (dropped)
		printf("cfg: store full -- %lu setting(s) not loaded (limit %d, %dB)\n",
			(unsigned long)dropped, K_CFG_ENTRIES, K_CFG_ARENA);

	if (verbose)
		printf("cfg: %s: %u setting(s)%s\n", Z_CFG_PATH, (unsigned)k_cfg_n,
			bad ? ", some lines ignored" : "");

	if (ignored) *ignored = bad;
	return (int)k_cfg_n;

}

// -- syscalls --

static void copy_out(char *dst, uint32_t dstlen, const char *src) {
	uint32_t i = 0;
	if (!dst || !dstlen) return;
	for (; src && src[i] && i < dstlen - 1; i++) dst[i] = src[i];
	dst[i] = 0;
}

z_obj_t *k_cfg_get(z_obj_t *args) {

	z_cfg_get_args_t *a = (z_cfg_get_args_t *)args;
	if (!a) return &z_fail;

	a->generation = k_cfg_gen;
	a->found = 0;

	if (!a->key) return &z_ok;

	const char *v = k_cfg_find(a->key);
	if (v) {
		copy_out(a->val, a->vallen, v);
		a->found = 1;
	}

	return &z_ok;

}

z_obj_t *k_cfg_entry(z_obj_t *args) {

	z_cfg_entry_args_t *a = (z_cfg_entry_args_t *)args;
	const char *k, *v;

	if (!a) return &z_fail;

	a->found = 0;
	if (!k_cfg_at(a->index, &k, &v)) return &z_ok;

	copy_out(a->key, a->keylen, k);
	copy_out(a->val, a->vallen, v);
	a->found = 1;

	return &z_ok;

}

z_obj_t *k_cfg_reload(z_obj_t *args) {

	z_cfg_reload_args_t *a = (z_cfg_reload_args_t *)args;
	uint32_t bad = 0;

	int n = k_cfg_load(false, &bad);

	if (a) {
		a->count = n;
		a->ignored = bad;
		a->generation = k_cfg_gen;
		a->done = 1;
	}

	printf("cfg: reloaded by pid %lu -- %d setting(s)\n",
		(unsigned long)z_pid, n);

	return &z_ok;

}

// -- the shell command --

void k_cfg_shell(const char *sub, const char *arg) {

	if (sub && !strcmp(sub, "reload")) {
		uint32_t bad;
		k_cfg_load(true, &bad);
		return;
	}

	if (sub && !strcmp(sub, "get")) {
		if (!arg) { printf("usage: cfg get <key>\n"); return; }
		const char *v = k_cfg_find(arg);
		if (v) printf("%s\n", v);
		else printf("%s (default -- not set in %s)\n", z_cfg_default(arg),
			Z_CFG_PATH);
		return;
	}

	if (sub) {
		printf("usage: cfg | cfg reload | cfg get <key>\n");
		return;
	}

	printf("%s: %u setting(s), generation %lu\n", Z_CFG_PATH,
		(unsigned)k_cfg_n, (unsigned long)k_cfg_gen);

	// Known keys first, with their effective value and where it came
	// from; then anything the file sets that nothing here reads, which
	// is kept rather than rejected (an app from outside this tree may
	// read it, or a typo may be worth spotting).
	for (int i = 0; i < z_cfg_known_count; i++) {
		const char *v = k_cfg_find(z_cfg_known[i].key);
		printf("  %-26s %s%s\n", z_cfg_known[i].key,
			v ? v : z_cfg_known[i].def, v ? "" : "  (default)");
	}

	for (uint16_t i = 0; i < k_cfg_n; i++) {
		const char *k = k_cfg_str(k_cfg_ent[i].key);
		bool known = false;
		for (int j = 0; j < z_cfg_known_count; j++)
			if (!strcmp(z_cfg_known[j].key, k)) known = true;
		if (!known)
			printf("  %-26s %s  (not read by anything built in)\n", k,
				k_cfg_str(k_cfg_ent[i].val));
	}

}
