/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Core apps in flash -- see zar.h for the design and the layout.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zar.h"
#include "../common/zsoc.h"

// Everything here reads the memory-mapped flash window directly.
// volatile because this is hardware, not RAM: nothing should cache a
// read across a reflash, and the compiler has no reason to know that.
static volatile const uint8_t *zar_base(void) {
	return (volatile const uint8_t *)Z_ZAR_ADDR;
}

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

bool z_zar_name(uint32_t i, char *out) {

	if (!out) return false;
	if (i >= z_zar_count()) return false;

	volatile const uint8_t *p = zar_base() + zar_entry_off(i);

	uint32_t n;
	for (n = 0; n < Z_ZAR_NAME_MAX; n++)
		out[n] = (char)p[n];

	// names are NUL-padded, but a name of exactly Z_ZAR_NAME_MAX
	// characters has no terminator in the record itself
	out[Z_ZAR_NAME_MAX] = '\0';

	return true;

}

// Finds `name`, returning its index or -1. Compared over exactly
// Z_ZAR_NAME_MAX bytes against the NUL-padded record so that a stored
// "wm" can't be matched by a query of "wmx" or vice versa.
static int zar_find(const char *name) {

	if (!name) return -1;

	uint32_t count = z_zar_count();
	char entry[Z_ZAR_NAME_MAX + 1];

	for (uint32_t i = 0; i < count; i++) {
		if (!z_zar_name(i, entry)) continue;
		if (strncmp(entry, name, Z_ZAR_NAME_MAX) == 0 &&
			strlen(entry) == strlen(name))
			return (int)i;
	}

	return -1;

}

int z_zar_exec_info(const char *name, z_exec_info_t *info) {

	if (!info) return 1;

	int idx = zar_find(name);
	if (idx < 0) return 1;

	uint32_t rec = zar_entry_off((uint32_t)idx);
	uint32_t file_size = zar_rd32(rec + 20);

	// The stored file is a ZEXE, exactly as written by the app's own
	// Makefile -- mkzar.py concatenates them verbatim rather than
	// re-encoding, so the same parser applies to both sources and
	// there is only one format to keep working.
	uint32_t off = zar_rd32(rec + 16);
	uint8_t hdr[Z_EXEC_HEADER_SIZE];
	volatile const uint8_t *p = zar_base() + off;

	uint32_t n = (file_size < Z_EXEC_HEADER_SIZE) ?
		file_size : Z_EXEC_HEADER_SIZE;
	for (uint32_t i = 0; i < n; i++) hdr[i] = p[i];

	z_exec_parse(hdr, n, file_size, info);

	return 0;

}

int z_zar_load_exec(uint32_t dst, const char *name,
	const z_exec_info_t *info) {

	if (!info) return 1;

	int idx = zar_find(name);
	if (idx < 0) return 1;

	uint32_t rec = zar_entry_off((uint32_t)idx);
	uint32_t off = zar_rd32(rec + 16);

	volatile const uint8_t *src = zar_base() + off + info->data_off;
	uint8_t *out = (uint8_t *)dst;

	// Plain byte copy out of the flash window. No chunking or
	// buffering, unlike fs_load_exec() -- there is no filesystem in
	// the way, this is just memory.
	for (uint32_t i = 0; i < info->data_size; i++)
		out[i] = src[i];

	// Nothing else zeroes .bss on this OS -- same as fs_load_exec().
	if (info->bss_size)
		memset(out + info->data_size, 0, info->bss_size);

	// We have just written CODE through the data path; the instruction
	// cache caches fetches only and never saw those stores. Without
	// this, a process loaded into memory a previous app occupied runs
	// the previous app's instructions. See zsoc.h.
	z_icache_flush();

	return 0;

}
