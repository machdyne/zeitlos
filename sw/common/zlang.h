#ifndef ZLANG_H
#define ZLANG_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The language directory: what sw/apps/jfont holds for every app when
 * the system speaks a language other than English -- the translations,
 * the glyphs the hardware fonts do not have, and the Japanese font. See
 * docs/i18n.md.
 *
 * Found as the Japanese font always was (zjfont.h): no message. The
 * client finds the process registered as Z_JFONT_NAME in
 * z_proc_list(), scans its block a word at a time for a live directory
 * (both magics, and a check word made from the directory's own
 * physical address), and keeps the address. Each entry points at data
 * in jfont's arena by physical address: every byte of memory has one
 * physical address any process may read (docs/mpu.md).
 *
 * jfont writes the entries, then `check`, then magic1, then magic0 LAST,
 * so a half-written directory never matches. It runs until the machine
 * stops: an app keeps pointers into the catalog (zi18n.h caches them),
 * so the language is chosen once, at boot, and changing it means a
 * restart (settings says so).
 *
 * Entries a client does not know are skipped, so a later system can add
 * kinds -- a font for another script, right-to-left data -- without
 * breaking a program built now.
 */

#include <stdint.h>

#define Z_LANG_MAGIC0     0x474E4C5Au	// "ZLNG"
#define Z_LANG_MAGIC1     0x31524944u	// "DIR1"

// Room for the fonts and a catalog for every app that has one: the
// common one (/data/jfont/lang/<lang>.zlc, the shared library's
// strings) and each app's own (/data/<app>/lang/<lang>.zlc).
#define Z_LANG_ENTRIES    48

// What an entry holds.
#define Z_LANG_CATALOG    1	// translations: a ZLC1 bundle (zi18n.h); one per app, any number
#define Z_LANG_FONT_5X8   2	// ZFN1: glyphs z_font_5x8 lacks (Latin Extended-A, Greek, Cyrillic, ...)
#define Z_LANG_FONT_6X12  3	// ZFN1: the same for z_font_6x12
#define Z_LANG_FONT_JP12  4	// ZFN1: the Japanese font, 12x12 (two 6x12 cells)

typedef struct {
	uint32_t	kind;		// Z_LANG_*, 0 for an unused slot
	uint32_t	addr;		// physical address of the data
	uint32_t	len;		// bytes
} z_lang_entry_t;

typedef struct {
	uint32_t	magic0;
	uint32_t	magic1;
	uint32_t	check;
	uint32_t	version;	// 1
	char		lang[8];	// system.language: "de", "ja", ... NUL-padded
	z_lang_entry_t	entry[Z_LANG_ENTRIES];
} z_lang_dir_t;

static inline uint32_t z_lang_check(uint32_t phys) {
	return Z_LANG_MAGIC0 ^ Z_LANG_MAGIC1 ^ phys;
}

static inline int z_lang_dir_ok(const volatile z_lang_dir_t *d, uint32_t phys) {
	return d->magic0 == Z_LANG_MAGIC0 && d->magic1 == Z_LANG_MAGIC1 &&
		d->check == z_lang_check(phys);
}

// The entry of `kind` in a live directory, or 0 (its length in *len).
static inline uint32_t z_lang_entry(const volatile z_lang_dir_t *d, uint32_t kind, uint32_t *len) {
	for (int i = 0; i < Z_LANG_ENTRIES; i++)
		if (d->entry[i].kind == kind && d->entry[i].addr) {
			if (len) *len = d->entry[i].len;
			return d->entry[i].addr;
		}
	return 0;
}

// The data of `kind`, from the directory jfont holds (or what a host
// test handed over with z_lang_use()), or NULL. zi18n.c.
const uint8_t *z_lang_data(uint32_t kind, uint32_t *len);

// The language jfont holds -- the one the running system speaks, which
// a change in settings does not alter until a restart -- or "en" when
// jfont is not running. For what must match the windows, such as the
// voice (sw/apps/tts).
const char *z_lang_current(void);
void z_lang_use(uint32_t kind, const uint8_t *data, uint32_t len);

// Finds the directory in a process list (zproc.h's z_proc_info_t), as
// z_jfont_find() did the font. Pure, so the host tests run it on a fake
// list over a real buffer.
#include "zproc.h"
#include "zjfont.h"		// Z_JFONT_NAME

static inline const volatile z_lang_dir_t *z_lang_find(
	const z_proc_info_t *procs, uint32_t n, uint32_t *phys) {

	for (uint32_t i = 0; i < n; i++) {
		const char *a = procs[i].name, *b = Z_JFONT_NAME;
		while (*a && *a == *b) { a++; b++; }
		if (*a || *b) continue;
		uint32_t at = (procs[i].base + 3) & ~3u;
		uint32_t end = procs[i].base + procs[i].size;
		for (; at + sizeof(z_lang_dir_t) <= end; at += 4) {
			const volatile z_lang_dir_t *d =
				(const volatile z_lang_dir_t *)(uintptr_t)at;
			if (d->magic0 != Z_LANG_MAGIC0) continue;
			if (!z_lang_dir_ok(d, at)) continue;
			*phys = at;
			return d;
		}
	}
	return 0;

}

#endif
