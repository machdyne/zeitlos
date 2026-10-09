/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Core apps, and files, in flash -- see zar.h for the design and the
 * layout (ZAR2).
 */

#include "flashapi.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zar.h"
#include "../common/zsoc.h"
#include "../common/zpaths.h"	// Z_DIR_APPS
#include "zarcopy.h"		// zar_copy(): a word per flash read

// Everything here reads the memory-mapped flash window directly.
// volatile because this is hardware, not RAM: nothing should cache a
// read across a reflash, and the compiler has no reason to know that.
#ifndef ZAR_HOST_TEST
static volatile const uint8_t *zar_base(void) {
	return (volatile const uint8_t *)z_zar_addr();
}
#else
// sw/os/tests/test_zar.c: an archive mkzar.py built, in a host buffer
extern const uint8_t *zar_test_base;
static volatile const uint8_t *zar_base(void) { return zar_test_base; }
#endif

// Little-endian 32-bit read. Spelled out byte by byte rather than
// casting to a uint32_t pointer: the archive is a byte layout produced
// by tools/mkzar.py on a host, and a struct cast would quietly depend
// on this compiler's padding and alignment choices matching python's
// struct.pack. Four byte loads cost nothing here and can't disagree.
static uint32_t zar_rd32(uint32_t off) {
	volatile const uint8_t *p = zar_base() + off;
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool z_zar_present(void) {

	volatile const uint8_t *p = zar_base();

	if (p[0] != Z_ZAR_MAGIC0 || p[1] != Z_ZAR_MAGIC1 ||
		p[2] != Z_ZAR_MAGIC2 || p[3] != Z_ZAR_MAGIC3)
		return false;

	// Erased flash reads back as 0xFF, so an unprogrammed region would
	// give a count of 0xFFFFFFFF. The magic check above already
	// rejects that, but bound it anyway -- a partially written archive
	// is the case where magic is valid and count is not.
	uint32_t count = zar_rd32(4);
	if (count == 0 || count > Z_ZAR_MAX_ENTRIES) return false;

	return true;

}

uint32_t z_zar_count(void) {
	if (!z_zar_present()) return 0;
	return zar_rd32(4);
}

// Byte offset of entry `i`'s record within the archive.
static uint32_t zar_entry_off(uint32_t i) {
	return Z_ZAR_HEADER_SIZE + (i * Z_ZAR_ENTRY_SIZE);
}

static uint32_t zar_flags(uint32_t i) { return zar_rd32(zar_entry_off(i) + 4); }
static uint32_t zar_data(uint32_t i) { return zar_rd32(zar_entry_off(i) + 8); }
static uint32_t zar_size(uint32_t i) { return zar_rd32(zar_entry_off(i) + 12); }

// Entry `i`'s name, in flash: NUL-terminated, at most Z_ZAR_NAME_MAX.
static volatile const uint8_t *zar_name_ptr(uint32_t i) {
	return zar_base() + zar_rd32(zar_entry_off(i));
}

bool z_zar_name(uint32_t i, char *out) {

	if (!out) return false;
	if (i >= z_zar_count()) return false;

	volatile const uint8_t *p = zar_name_ptr(i);
	uint32_t n;
	for (n = 0; n < Z_ZAR_NAME_MAX && p[n]; n++)
		out[n] = (char)p[n];
	out[n] = '\0';

	return true;

}

bool z_zar_is_file(uint32_t i) {
	return i < z_zar_count() && (zar_flags(i) & Z_ZAR_FILE);
}

static int zar_lower(int c) {
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

// The app called `name`: exactly, as names have always been matched.
// Files are not apps, whatever they are called.
static int zar_find(const char *name) {

	if (!name) return -1;

	uint32_t count = z_zar_count();
	size_t len = strlen(name);

	for (uint32_t i = 0; i < count; i++) {
		if (zar_flags(i) & Z_ZAR_FILE) continue;
		volatile const uint8_t *p = zar_name_ptr(i);
		size_t n = 0;
		while (n < len && p[n] == (uint8_t)name[n]) n++;
		if (n == len && p[n] == 0) return (int)i;
	}

	return -1;

}

int z_zar_exec_info(const char *name, z_exec_info_t *info) {

	if (!info) return 1;

	// Not while the flash is being written: the archive may be half
	// rewritten. flashapi.c, docs/spiflash.md.
	if (k_flash_session_active()) return 1;

	int idx = zar_find(name);
	if (idx < 0) return 1;

	uint32_t file_size = zar_size((uint32_t)idx);

	// The header is read into RAM before parsing because
	// z_exec_parse() takes a plain pointer and the flash window is
	// volatile. Sixteen bytes; nothing else is copied.
	uint8_t hdr[Z_EXEC_HEADER_SIZE];
	volatile const uint8_t *p = zar_base() + zar_data((uint32_t)idx);
	uint32_t n = (file_size < Z_EXEC_HEADER_SIZE) ?
		file_size : Z_EXEC_HEADER_SIZE;
	for (uint32_t i = 0; i < n; i++) hdr[i] = p[i];

	z_exec_parse(hdr, n, file_size, info);

	return 0;

}

int z_zar_load_exec(uint32_t dst, const char *name,
	const z_exec_info_t *info) {

	if (!info) return 1;
	if (k_flash_session_active()) return 1;     // see z_zar_exec_info()

	int idx = zar_find(name);
	if (idx < 0) return 1;

	volatile const uint8_t *src = zar_base() + zar_data((uint32_t)idx) +
		info->data_off;
	uint8_t *out = (uint8_t *)(uintptr_t)dst;

	zar_copy(out, src, info->data_size);

	if (info->bss_size)
		memset(out + info->data_size, 0, info->bss_size);

	z_icache_flush();

	return 0;

}

// -- files: the underlay --

// Entry i's path, as the filesystem would spell it without the leading
// slash, compared to `s` (n chars, no leading slash) without regard to
// case. Returns how many characters of the entry's path follow the
// first n when they match ("docs/welcome.txt" against "docs": the
// "/welcome.txt"), or -1. An app's path is "apps/<name>".
static int zar_path_cmp(uint32_t i, const char *s, size_t n, const char **rest_app,
	volatile const uint8_t **rest) {

	static const char apps[] = Z_DIR_APPS "/";	// "/apps/"
	volatile const uint8_t *p = zar_name_ptr(i);
	size_t k = 0, a = 0;

	*rest_app = NULL;
	if (!(zar_flags(i) & Z_ZAR_FILE)) {
		// the "apps/" in front of an app's name
		for (a = 1; apps[a] && k < n; a++, k++)
			if (zar_lower(apps[a]) != zar_lower(s[k])) return -1;
		if (apps[a]) {
			// s ended inside "apps/": what follows is the rest of it
			*rest_app = apps + a;
			*rest = p;
			return 1;
		}
	}
	for (; k < n; k++, p++)
		if (!*p || zar_lower(*p) != zar_lower(s[k])) return -1;
	*rest = p;
	return *p ? 1 : 0;

}

static const char *zar_skip_slash(const char *s) {
	while (*s == '/') s++;
	return s;
}

int z_zar_file(const char *path, uint32_t *off, uint32_t *size) {

	if (!path || k_flash_session_active()) return 1;
	path = zar_skip_slash(path);
	size_t n = strlen(path);
	uint32_t count = z_zar_count();

	for (uint32_t i = 0; i < count; i++) {
		const char *ra;
		volatile const uint8_t *r;
		if (zar_path_cmp(i, path, n, &ra, &r) == 0) {
			*off = zar_data(i);
			*size = zar_size(i);
			return 0;
		}
	}

	return 1;

}

void z_zar_read(uint32_t off, uint8_t *dst, uint32_t n) {
	zar_copy(dst, zar_base() + off, n);
}

// The child of `dir` that entry i is under, into name: its own name, or
// the subdirectory it is in. -1 if it is not under dir, else whether it
// is a directory.
static int zar_child_of(uint32_t i, const char *dir, size_t n, char *name) {

	const char *ra;
	volatile const uint8_t *r;
	uint32_t k = 0;

	if (n) {
		if (zar_path_cmp(i, dir, n, &ra, &r) != 1) return -1;
		if (ra) {
			if (*ra != '/') return -1;
			ra++;
		} else {
			if (*r != '/') return -1;
			r++;
		}
	} else {
		ra = (zar_flags(i) & Z_ZAR_FILE) ? NULL : Z_DIR_APPS "/" + 1;
		r = zar_name_ptr(i);
	}

	if (ra && *ra) {
		// the rest of "apps/": a directory
		while (*ra && *ra != '/' && k < Z_ZAR_NAME_MAX) name[k++] = *ra++;
		name[k] = 0;
		return 1;
	}
	while (*r && *r != '/' && k < Z_ZAR_NAME_MAX) name[k++] = (char)*r++;
	name[k] = 0;
	return *r == '/';

}

bool z_zar_child(const char *dir, uint32_t *iter, char *name,
	uint32_t *size, bool *is_dir) {

	dir = zar_skip_slash(dir ? dir : "");
	size_t n = strlen(dir);
	while (n && dir[n - 1] == '/') n--;
	uint32_t count = z_zar_count();

	for (; *iter < count; (*iter)++) {

		uint32_t i = *iter;
		int d = zar_child_of(i, dir, n, name);
		if (d < 0) continue;

		// a directory: only the first entry under it reports it
		if (d) {
			char other[Z_ZAR_NAME_MAX + 1];
			uint32_t j;
			for (j = 0; j < i; j++)
				if (zar_child_of(j, dir, n, other) == 1 && !strcmp(other, name)) break;
			if (j < i) continue;
		}

		*is_dir = d != 0;
		*size = d ? 0 : zar_size(i);
		(*iter)++;
		return true;

	}

	return false;

}

bool z_zar_is_dir(const char *dir) {
	char name[Z_ZAR_NAME_MAX + 1];
	uint32_t it = 0, size;
	bool d;
	dir = zar_skip_slash(dir ? dir : "");
	if (!*dir) return z_zar_count() != 0;
	return z_zar_child(dir, &it, name, &size, &d);
}
