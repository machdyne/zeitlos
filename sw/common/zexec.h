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
 * time spent transferring nothing, on an SPI card.
 *
 * It was also a format with no identity: a bare `.bin` says nothing
 * about itself, so the loader had to infer everything from the file
 * size and hope.
 *
 * This header fixes both. .bss becomes a NUMBER rather than a region
 * of zeros -- the loader allocates and memset()s it, which is far
 * faster than reading it -- and the magic makes the format
 * self-identifying. The same header now also says how much stack and
 * heap the program wants, so that choice is not a table of file names
 * inside the kernel.
 *
 * -- Layout --
 *
 *   offset  size  field
 *   0       4     magic    "ZEXE"
 *   4       2     version  format version (currently 1)
 *   6       2     flags    bits 3:0 the stack size code; bit 8
 *                           COMPRESSED; bit 15 DRIVER; the rest 0
 *   8       4     bss_size bytes of .bss to allocate and zero after data
 *   12      4     entry    0; COMPRESSED: the data's size, uncompressed
 *   16      ...   data     the loadable image, verbatim -- or COMPRESSED,
 *                           as an LZ4 block (lz4_Block_format.md)
 *
 * data_size is deliberately NOT stored: it is file_size - 16, and the
 * filesystem already knows the file size. One fewer field that can
 * disagree with reality. Except when compressed: then the file size
 * says nothing about it, and the `entry` word carries it.
 *
 * -- Compressed (bit 8) --
 *
 * The core apps in the flash archive are stored compressed: tools/
 * mkzar.py sets the bit, puts the image's size in `entry` and packs the
 * image as LZ4, the header itself left as it is so a loader learns the
 * sizes from the first 16 bytes as before. Every loader unpacks it
 * (sw/common/zexec_lz4.c): the kernel's from flash and from a file,
 * and the simulator's. docs/executables.md, "Compressed executables".
 *
 * `entry` was reserved for a start address other than the image's
 * first byte. Nothing ever used it: every app is linked at and started
 * from 0x80000000. Should a start address ever be needed, it is a new
 * header version -- a loader refuses a version it does not know.
 *
 * Header at the START rather than the end (both were on the table):
 * the loader wants bss_size BEFORE it allocates, and a trailing header
 * would mean seeking to the end, reading, then seeking back -- two
 * extra operations on every launch, to save nothing. 16 bytes also
 * keeps `data` 16-byte aligned in the file, which suits the chunked
 * reads the loader does. The stack size has the same constraint: the
 * loader has to know it before it reserves the block.
 *
 * -- The stack size code --
 *
 * Bits 3:0 of `flags` say how many bytes of stack and heap the
 * program wants on top of its image, as a power of two:
 *
 *   0       unspecified: the loader's default (16KB in sw/os/kernel.h)
 *   1..14   8KB << (code - 1)
 *   15      reserved
 *
 *   code  1    2     3     4     5      6      7      8    9    10
 *   size  8KB  16KB  32KB  64KB  128KB  256KB  512KB  1MB  2MB  4MB
 *
 *   code  11   12    13    14
 *   size  8MB  16MB  32MB  64MB
 *
 * An app's Makefile names a size (APP_STACK = 64K), and
 * tools/mkexec.py writes the code for it. Nobody has to remember the
 * table.
 *
 * The format can say 64MB; a kernel grants up to its own cap
 * (Z_PROC_STACK_CAP, sw/os/kernel.h). What it cannot give, it
 * refuses: code 15, a code above the cap, a bit set in 15:4, or a
 * block the memory pool cannot hold. The program does not start, and
 * the loader says what was asked for and what there is. It never
 * hands out less than the program asked for. A program that asked for
 * 32MB and runs in 4MB fails later, somewhere less obvious, and an
 * unknown request is not rounded to anything.
 *
 * The field is the old reserved flags word, still version 1. A binary
 * written before it -- flags 0 -- is "does not ask", which is what
 * that reservation always said. A kernel that does not read the word
 * still loads a binary that asks.
 *
 * -- Backward compatibility --
 *
 * A file with no "ZEXE" magic is treated as the old raw format:
 * data_size = file_size, bss_size = 0, flags = 0. That is EXACTLY
 * correct for a --pad-to binary, whose .bss is already present as
 * zeros in the data. So old and new binaries coexist on the same
 * card, and apps can be converted one at a time -- see
 * z_exec_parse() below.
 */

#define Z_EXEC_MAGIC0 'Z'
#define Z_EXEC_MAGIC1 'E'
#define Z_EXEC_MAGIC2 'X'
#define Z_EXEC_MAGIC3 'E'

#define Z_EXEC_VERSION      1
#define Z_EXEC_HEADER_SIZE  16

// Bits 3:0 of flags: the stack size code. Bit 8: compressed. Bits 14:9
// and 7:4 are reserved.
#define Z_EXEC_STACK_MASK      0x000fu

// Bit 15: the program is a DRIVER (docs/userspace_drivers.md, "Drivers").
// The loader starts it like any other program and marks the process
// Z_PROC_FLAG_DRIVER, which lets it open the grant window onto
// processes that have opted in to being written by drivers
// (z_grant_drivers(), sw/common/zeitlos.h). It declares; the client
// still decides. From the top of the word down, so the stack code can
// grow upwards into 14:4 if it ever needs to. A kernel that predates
// it refuses the binary (unknown flags) rather than run a driver
// without its powers. `APP_DRIVER = 1` in an app's Makefile sets it.
#define Z_EXEC_FLAG_DRIVER     0x8000u

// Every flag bit this loader understands; any other is refused.
// The data is an LZ4 block, and `entry` is its size unpacked. A kernel
// from before this refuses the binary (unknown flags), rather than run
// compressed bytes.
#define Z_EXEC_FLAG_COMPRESSED 0x0100u

#define Z_EXEC_FLAGS_KNOWN     (Z_EXEC_STACK_MASK | Z_EXEC_FLAG_COMPRESSED | \
	Z_EXEC_FLAG_DRIVER)
#define Z_EXEC_STACK_UNSPEC    0
#define Z_EXEC_STACK_RESERVED  15
#define Z_EXEC_STACK_MAX_CODE  14
#define Z_EXEC_STACK_UNIT      (8 * 1024)	// code 1

// Bytes for a size code: 8KB << (code - 1) for 1-14, 0 otherwise
// (0 is "unspecified", 15 is reserved).
static inline uint32_t z_exec_stack_bytes(uint32_t code) {
	if (code < 1 || code > Z_EXEC_STACK_MAX_CODE) return 0;
	return (uint32_t)Z_EXEC_STACK_UNIT << (code - 1);
}

// The answer to "how much stack does this program get", before any
// memory is looked at. `def` is the loader's default for a program
// that does not ask; `cap` is the most it grants.
typedef enum {
	Z_EXEC_STACK_OK = 0,
	Z_EXEC_STACK_BAD_FLAGS,	// a reserved bit (14:4) is set
	Z_EXEC_STACK_IS_RESERVED,	// code 15
	Z_EXEC_STACK_OVER_CAP,	// a real size, above `cap`
} z_exec_stack_rv;

static inline z_exec_stack_rv z_exec_stack(uint16_t flags, uint32_t def,
	uint32_t cap, uint32_t *bytes) {

	uint32_t code = flags & Z_EXEC_STACK_MASK;

	*bytes = 0;
	if (flags & ~Z_EXEC_FLAGS_KNOWN) return Z_EXEC_STACK_BAD_FLAGS;
	if (code == Z_EXEC_STACK_RESERVED) return Z_EXEC_STACK_IS_RESERVED;
	if (code == Z_EXEC_STACK_UNSPEC) {
		*bytes = def;
		return Z_EXEC_STACK_OK;
	}
	*bytes = z_exec_stack_bytes(code);
	if (*bytes > cap) return Z_EXEC_STACK_OVER_CAP;
	return Z_EXEC_STACK_OK;

}

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
	uint32_t	data_size;	// bytes of image (unpacked, when compressed)
	uint32_t	stored_size;	// bytes of it in the file: data_size unless compressed
	int			compressed;	// Z_EXEC_FLAG_COMPRESSED: unpack stored_size bytes
	uint32_t	bss_size;	// bytes to zero immediately after it
	uint32_t	total;		// data_size + bss_size -- the process image size
	int			is_zexe;	// 1 = real header, 0 = legacy raw binary
	uint16_t	flags;		// header flags; 0 for a legacy binary
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
//
// The stack size code is NOT checked here. The flags word is stored
// and the loader decides; see z_exec_stack() above and
// k_proc_create_exec() in sw/os/kernel.c.
static inline int z_exec_parse(const void *hdr, uint32_t hdr_len,
	uint32_t file_size, z_exec_info_t *info) {

	const uint8_t *h = (const uint8_t *)hdr;

	info->data_off = 0;
	info->data_size = file_size;
	info->stored_size = file_size;
	info->compressed = 0;
	info->bss_size = 0;
	info->total = file_size;
	info->is_zexe = 0;
	info->flags = 0;

	if (!h || hdr_len < Z_EXEC_HEADER_SIZE || file_size < Z_EXEC_HEADER_SIZE)
		return 0;	// too small to be one -- legacy

	if (h[0] != Z_EXEC_MAGIC0 || h[1] != Z_EXEC_MAGIC1 ||
		h[2] != Z_EXEC_MAGIC2 || h[3] != Z_EXEC_MAGIC3)
		return 0;	// no magic -- legacy

	uint16_t version = (uint16_t)(h[4] | (h[5] << 8));
	if (version != Z_EXEC_VERSION) return 1;

	uint32_t bss = (uint32_t)h[8] | ((uint32_t)h[9] << 8) |
		((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
	uint16_t flags = (uint16_t)(h[6] | (h[7] << 8));

	info->data_off = Z_EXEC_HEADER_SIZE;
	info->data_size = file_size - Z_EXEC_HEADER_SIZE;
	info->stored_size = info->data_size;
	if (flags & Z_EXEC_FLAG_COMPRESSED) {
		uint32_t n = (uint32_t)h[12] | ((uint32_t)h[13] << 8) |
			((uint32_t)h[14] << 16) | ((uint32_t)h[15] << 24);
		if (!n) return 1;		// compressed, and nothing to unpack to
		info->data_size = n;
		info->compressed = 1;
	}
	info->bss_size = bss;
	info->total = info->data_size + bss;
	info->is_zexe = 1;
	info->flags = flags;

	// a bss size that overflows the total is a corrupt or hostile file
	if (info->total < info->data_size) return 1;

	return 0;

}

#endif
