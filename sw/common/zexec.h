#ifndef ZEXEC_H
#define ZEXEC_H

#include <stdint.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The Zeitlos executable format.
 *
 * -- What this replaces, and why --
 *
 * App binaries used to be a raw `objcopy -O binary --pad-to=_end`
 * dump: the loadable image followed by .bss written out as literal
 * zeros. That works -- nothing zeroes .bss at startup on this OS, so
 * zeros-in-the-file IS the mechanism -- but it means every process
 * launch reads its whole .bss off the SD card. For `repl` that is
 * ~110KB of zeros out of a 293KB file, roughly a third of its load
 * time spent transferring nothing, on a bit-banged SPI card.
 *
 * It was also a format with no identity: a bare `.bin` says nothing
 * about itself, so the loader had to infer everything from the file
 * size and hope.
 *
 * This header fixes both. .bss becomes a NUMBER rather than a region
 * of zeros -- the loader allocates and memset()s it, which is far
 * faster than reading it -- and the magic makes the format
 * self-identifying.
 *
 * -- Layout --
 *
 *   offset  size  field
 *   0       4     magic    "ZEXE"
 *   4       2     version  format version (currently 1)
 *   6       2     flags    reserved, must be 0
 *   8       4     bss_size bytes of .bss to allocate and zero after data
 *   12      4     entry    reserved; 0 means "base address"
 *   16      ...   data     the loadable image, verbatim
 *
 * data_size is deliberately NOT stored: it is file_size - 16, and the
 * filesystem already knows the file size. One fewer field that can
 * disagree with reality.
 *
 * Header at the START rather than the end (both were on the table):
 * the loader wants bss_size BEFORE it allocates, and a trailing header
 * would mean seeking to the end, reading, then seeking back -- two
 * extra operations on every launch, to save nothing. 16 bytes also
 * keeps `data` 16-byte aligned in the file, which suits the chunked
 * reads the loader does.
 *
 * -- Backward compatibility --
 *
 * A file with no "ZEXE" magic is treated as the old raw format:
 * data_size = file_size, bss_size = 0. That is EXACTLY correct for a
 * --pad-to binary, whose .bss is already present as zeros in the data.
 * So old and new binaries coexist on the same card, and apps can be
 * converted one at a time -- see z_exec_parse() below.
 */

#define Z_EXEC_MAGIC0 'Z'
#define Z_EXEC_MAGIC1 'E'
#define Z_EXEC_MAGIC2 'X'
#define Z_EXEC_MAGIC3 'E'

#define Z_EXEC_VERSION      1
#define Z_EXEC_HEADER_SIZE  16

// On-disk header. Every field is little-endian, matching the CPU, so
// this maps directly onto the first 16 bytes read with no unpacking.
typedef struct {
	uint8_t		magic[4];
	uint16_t	version;
	uint16_t	flags;
	uint32_t	bss_size;
	uint32_t	entry;
} z_exec_header_t;

// What a loader actually needs, after parsing.
typedef struct {
	uint32_t	data_off;	// where the loadable image starts in the file
	uint32_t	data_size;	// bytes to read from the file
	uint32_t	bss_size;	// bytes to zero immediately after it
	uint32_t	total;		// data_size + bss_size -- the process image size
	int			is_zexe;	// 1 = real header, 0 = legacy raw binary
} z_exec_info_t;

// Fills `info` from the first Z_EXEC_HEADER_SIZE bytes of a file plus
// its total size. `hdr` may be shorter than a full header (or the file
// smaller than one) -- that is simply a legacy binary, not an error.
//
// Returns 0 on success, non-zero only for a header that IS a ZEXE
// header but one this loader can't handle (unknown version, or a
// bss/data size that doesn't fit the file). Refusing an unknown version
// rather than guessing matters: a future format change that silently
// half-loaded would corrupt memory instead of failing.
static inline int z_exec_parse(const void *hdr, uint32_t hdr_len,
	uint32_t file_size, z_exec_info_t *info) {

	const uint8_t *h = (const uint8_t *)hdr;

	info->data_off = 0;
	info->data_size = file_size;
	info->bss_size = 0;
	info->total = file_size;
	info->is_zexe = 0;

	if (!h || hdr_len < Z_EXEC_HEADER_SIZE || file_size < Z_EXEC_HEADER_SIZE)
		return 0;	// too small to be one -- legacy

	if (h[0] != Z_EXEC_MAGIC0 || h[1] != Z_EXEC_MAGIC1 ||
		h[2] != Z_EXEC_MAGIC2 || h[3] != Z_EXEC_MAGIC3)
		return 0;	// no magic -- legacy

	uint16_t version = (uint16_t)(h[4] | (h[5] << 8));
	if (version != Z_EXEC_VERSION) return 1;

	uint32_t bss = (uint32_t)h[8] | ((uint32_t)h[9] << 8) |
		((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);

	info->data_off = Z_EXEC_HEADER_SIZE;
	info->data_size = file_size - Z_EXEC_HEADER_SIZE;
	info->bss_size = bss;
	info->total = info->data_size + bss;
	info->is_zexe = 1;

	// a bss size that overflows the total is a corrupt or hostile file
	if (info->total < info->data_size) return 1;

	return 0;

}

#endif
