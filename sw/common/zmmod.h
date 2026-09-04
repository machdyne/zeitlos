#ifndef ZMMOD_H
#define ZMMOD_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI memory modules -- NOR flash, FRAM and EEPROM -- over
 * sw/common/zspi.h. See docs/mmod.md.
 *
 * -- The profile is the API --
 *
 * These devices differ in ways no single code path can hide: whether
 * a write needs an erase first, whether writes stop at a page
 * boundary, whether there is a busy flag to poll, and how many bytes
 * an address takes. So a z_mmod_t carries a PROFILE, and every
 * operation reads it.
 *
 * z_mmod_detect() fills the profile in when it can. It often cannot,
 * and that is not a gap to be closed later -- see below. The caller
 * is expected to correct it, which is why every field is public and
 * writable rather than hidden behind an accessor.
 *
 * -- Detection is genuinely impossible for some devices --
 *
 * RDID (0x9F) sorts into three cases:
 *
 *   first byte 0x7f  JEDEC continuation codes; the real manufacturer
 *                    follows. Infineon/Cypress FRAM identifies this
 *                    way, so 7f 7f 7f is a positive FRAM signature
 *                    rather than garbage.
 *   0x00 or 0xff     NO RDID AT ALL.
 *   anything else    NOR-style manufacturer / type / capacity.
 *
 * The middle case is the problem: small FRAM, the 25AA/25LC EEPROMs
 * and an EMPTY SOCKET are indistinguishable. No probe fixes that --
 * telling "device that ignores 0x9F" from "nothing there" requires
 * writing, and writing is the thing being made safe.
 *
 * So a manual profile is the primary mechanism and detection is a
 * convenience on top of it.
 *
 * -- Address width is 2, 3 OR 4 --
 *
 * Not 2 or 3. A 32MB NOR (capacity byte 0x19) cannot be reached with
 * 24 bits at all: three bytes top out at 16MB, and the upper half
 * needs four. Defaulting such a part to three silently accesses the
 * bottom half while appearing to address the whole device -- a wrong
 * reading on a read, and data loss on a write.
 *
 * FOUR-BYTE ACCESS USES THE 4-BYTE OPCODES (0x13, 0x12, 0x21), NOT
 * EN4B. That is deliberate. EN4B (0xB7) puts the device into a mode
 * that PERSISTS after this process exits and after a warm reset on
 * many parts, so a bootloader or another app that assumes 3-byte
 * addressing would then read from the wrong place -- a failure caused
 * by a program that is no longer running. The 4-byte opcodes are
 * stateless and leave nothing behind.
 *
 * -- Wrong width is asymmetric --
 *
 * With 3-byte addressing on a 2-byte device the third address byte is
 * consumed as the first data byte. Harmless on a read; catastrophic on
 * a write. So z_mmod_probe_widths() exists to let a human SEE which
 * width produces sensible data, and the write path refuses to run
 * against an unconfirmed profile.
 *
 * -- Chunking is the caller's job --
 *
 * Nothing here yields. A 32MB read over bit-banged SPI is minutes, so
 * an interactive caller must break it into pieces and do something
 * else in between; z_mmod_read() moves exactly what it is asked for
 * and returns. Z_MMOD_CHUNK is a reasonable slice.
 *
 * Erase and chip-erase are the exception -- they cannot be subdivided
 * below one sector, and a chip erase is one command that takes
 * minutes. z_mmod_erase_sector() polls with a timeout; the caller
 * should erase one sector per slice rather than calling
 * z_mmod_erase_range() from an event loop.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zspi.h"

// A sensible amount to move per call from an interactive caller.
// Small enough that a slice is milliseconds even bit-banged, large
// enough that the per-call command overhead is amortised.
#define Z_MMOD_CHUNK 256

typedef enum {
	Z_MMOD_UNKNOWN = 0,
	Z_MMOD_NOR,			// erase before write, page-limited, has WIP
	Z_MMOD_FRAM,		// no erase, no page limit, write completes at once
	Z_MMOD_EEPROM		// no erase (page write overwrites), page-limited
} z_mmod_class_t;

typedef enum {
	Z_MMOD_OK = 0,
	Z_MMOD_NODEV,		// nothing answered
	Z_MMOD_BADPROFILE,	// the profile cannot describe this operation
	Z_MMOD_RANGE,		// outside the device, or crosses its end
	Z_MMOD_TIMEOUT,		// the busy flag never cleared
	Z_MMOD_VERIFY		// read-back did not match
} z_mmod_rv;

typedef struct {

	z_spi_t			spi;

	// -- profile: detect() fills these in; the caller may correct
	//    any of them, and for most non-NOR devices must --

	z_mmod_class_t	cls;
	uint8_t			id[3];			// raw RDID, for display
	uint32_t		size;			// bytes
	uint8_t			addr_bytes;		// 2, 3 or 4
	uint32_t		page;			// program granularity, 0 = unlimited
	uint32_t		sector;			// erase granularity, 0 = no erase
	bool			needs_erase;	// a 1 bit cannot be written over a 0
	bool			has_wip;		// poll the status register after a write

	// True once detect() has run AND the chip select was proven to
	// work. The write path checks it; see z_mmod_ss_ok().
	bool			ss_ok;
	bool			detected;

	// Timeouts in milliseconds. Defaults are set by z_mmod_init() and
	// are generous: a NOR sector erase is typically tens of ms but
	// specified in the hundreds, and a page program is sub-ms but not
	// guaranteed.
	uint32_t		write_ms;
	uint32_t		erase_ms;

} z_mmod_t;

// Configure the SPI pins for an MMOD socket on `port` and set profile
// defaults. Does not touch the device.
//
// MMOD pin 1..4 map to GPIO bits 0..3 on both the 6- and 12-pin
// socket, fixed by the spec, so there is nothing to configure beyond
// which port.
void z_mmod_init(z_mmod_t *m, uint32_t port, uint32_t khz);

// Read RDID and populate whatever of the profile it can.
//
// Returns Z_MMOD_NODEV if nothing answered (all 00 or all ff), in
// which case `id` still holds what came back -- the two values mean
// different things and the caller should say which it saw.
//
// A successful return does NOT mean the profile is right. For
// anything that is not NOR flash it means "here is a starting point";
// see this file's header.
z_mmod_rv z_mmod_detect(z_mmod_t *m);

// Is the chip select actually working?
//
// Runs an RDID with SS DEASSERTED. A working device is not listening,
// so MISO stays tri-stated and reads back 0xff -- anything but the
// real ID. If the ID comes back regardless, the device is permanently
// selected, every clock edge on the bus is a command byte to it, and
// nothing may be written.
//
// Only meaningful after a successful detect(). Sets m->ss_ok.
bool z_mmod_ss_ok(z_mmod_t *m);

// Read `n` bytes from `addr`. No erase state, no page limit, no
// waiting -- reads are the safe operation on every device here.
z_mmod_rv z_mmod_read(z_mmod_t *m, uint32_t addr, void *buf, uint32_t n);

// Write `n` bytes to `addr`, splitting at page boundaries and waiting
// for each page to complete.
//
// DOES NOT ERASE. On a device with needs_erase set, writing over
// anything that is not already erased silently produces the AND of
// the old and new data -- so this refuses unless the target range
// reads as erased. Call z_mmod_erase_range() first, or
// z_mmod_blank_check() to find out.
//
// Refuses entirely unless m->ss_ok: see z_mmod_ss_ok() on what a
// stuck chip select does to a write.
z_mmod_rv z_mmod_write(z_mmod_t *m, uint32_t addr, const void *buf,
	uint32_t n);

// Is this range already erased (all 0xff)?
//
// Cheap, and worth doing before a program: the failure it prevents is
// a write that "succeeds" and verifies wrong, which is a confusing
// thing to debug from the other end.
z_mmod_rv z_mmod_blank_check(z_mmod_t *m, uint32_t addr, uint32_t n,
	uint32_t *first_bad);

// Erase one sector containing `addr`. The whole sector goes, which is
// why callers should snap ranges to sector boundaries visibly rather
// than quietly widening them.
z_mmod_rv z_mmod_erase_sector(z_mmod_t *m, uint32_t addr);

// Compare `n` bytes at `addr` against `buf`. Returns Z_MMOD_VERIFY and
// sets `*first_bad` to the offset of the first difference.
z_mmod_rv z_mmod_verify(z_mmod_t *m, uint32_t addr, const void *buf,
	uint32_t n, uint32_t *first_bad);

// Read 16 bytes from `addr` at 2, 3 and 4-byte addressing into three
// buffers, so a human can see which one produces sensible data.
//
// The reliable way to settle address width on a device with no RDID,
// and safe because it only reads. See this file's header on why
// guessing is fine for reads and not for writes.
void z_mmod_probe_widths(z_mmod_t *m, uint32_t addr,
	uint8_t out2[16], uint8_t out3[16], uint8_t out4[16]);

const char *z_mmod_strerror(z_mmod_rv rv);
const char *z_mmod_class_name(z_mmod_class_t c);

// Manufacturer name for an RDID first byte, or "unknown".
const char *z_mmod_vendor(uint8_t mfg);

#endif
