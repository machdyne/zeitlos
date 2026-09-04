/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI memory modules -- see sw/common/zmmod.h for the interface and
 * the design, and docs/mmod.md for how to use it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zmmod.h"
#include "zeitlos.h"		// z_uptime_ticks()
#include "zsoc.h"			// Z_TICK_HZ

// MMOD pin 1..4 -> GPIO bit 0..3, on both socket sizes. Fixed by the
// spec (github.com/machdyne/mmod), so this is not configurable.
#define MMOD_SS   0
#define MMOD_MOSI 1
#define MMOD_MISO 2
#define MMOD_SCK  3

// Three-byte-address opcodes.
#define CMD_RDID   0x9f
#define CMD_READ   0x03
#define CMD_PP     0x02
#define CMD_SE     0x20
#define CMD_WREN   0x06
#define CMD_RDSR   0x05

// Four-byte-address opcodes.
//
// Used instead of EN4B (0xb7) deliberately -- see zmmod.h. EN4B leaves
// a mode set that outlives this process; these are stateless.
#define CMD_READ4  0x13
#define CMD_PP4    0x12
#define CMD_SE4    0x21

#define SR_WIP 0x01

void z_mmod_init(z_mmod_t *m, uint32_t port, uint32_t khz) {

	memset(m, 0, sizeof(*m));

	m->spi.sck_port = (uint8_t)port;  m->spi.sck_pin = MMOD_SCK;
	m->spi.mosi_port = (uint8_t)port; m->spi.mosi_pin = MMOD_MOSI;
	m->spi.miso_port = (uint8_t)port; m->spi.miso_pin = MMOD_MISO;
	m->spi.cs_port = (uint8_t)port;   m->spi.cs_pin = MMOD_SS;
	m->spi.mode = 0;					// MMOD is Pmod Type 2
	m->spi.khz = khz;

	z_spi_init(&m->spi);

	// Defaults describing the most common device, so a caller that
	// only corrects `cls` and `size` gets something workable.
	m->cls = Z_MMOD_UNKNOWN;
	m->addr_bytes = 3;
	m->page = 256;
	m->sector = 4096;
	m->needs_erase = true;
	m->has_wip = true;

	// Generous. A NOR page program is sub-millisecond typical but
	// specified far higher, and a sector erase is tens of ms typical
	// against hundreds specified. Timing out early on a slow part
	// would abandon a write half done, which is worse than waiting.
	m->write_ms = 50;
	m->erase_ms = 1000;

}

// -- primitives -------------------------------------------------

// Push the address in whatever width the profile says. Byte order is
// big-endian on every device here.
static int put_addr(const z_mmod_t *m, uint8_t *b, uint32_t addr) {

	int n = m->addr_bytes;
	int i;

	for (i = 0; i < n; i++)
		b[i] = (uint8_t)((addr >> (8 * (n - 1 - i))) & 0xff);

	return n;

}

static uint8_t opcode(const z_mmod_t *m, uint8_t three, uint8_t four) {
	return m->addr_bytes == 4 ? four : three;
}

static void cmd_only(z_mmod_t *m, uint8_t op) {
	z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, &op, NULL, 1);
	z_spi_select(&m->spi, false);
}

static uint8_t read_sr(z_mmod_t *m) {
	uint8_t b[2];
	b[0] = CMD_RDSR;
	b[1] = 0xff;
	z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, b, b, 2);
	z_spi_select(&m->spi, false);
	return b[1];
}

// Wait for the write-in-progress flag to clear.
//
// Bounded by wall-clock ticks rather than an iteration count, because
// the loop's speed depends on the SPI backend -- an iteration count
// tuned for bit-banging would time out roughly 18x too early on
// hardware SPI, which is exactly the kind of bug that only shows up
// on the machine you did not test on.
static z_mmod_rv wait_ready(z_mmod_t *m, uint32_t ms) {

	uint32_t start;
	uint32_t limit;

	if (!m->has_wip) return Z_MMOD_OK;

	start = z_uptime_ticks();
	limit = (Z_TICK_HZ * ms) / 1000u;
	if (!limit) limit = 1;

	while ((read_sr(m) & SR_WIP)) {
		if ((z_uptime_ticks() - start) > limit) return Z_MMOD_TIMEOUT;
	}

	return Z_MMOD_OK;

}

static void read_id_raw(z_mmod_t *m, uint8_t *id, bool select) {

	uint8_t b[4];

	b[0] = CMD_RDID;
	b[1] = b[2] = b[3] = 0xff;

	if (select) z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, b, b, 4);
	if (select) z_spi_select(&m->spi, false);

	id[0] = b[1]; id[1] = b[2]; id[2] = b[3];

}

// -- identify ---------------------------------------------------

z_mmod_rv z_mmod_detect(z_mmod_t *m) {

	uint8_t id[3];

	m->detected = false;
	m->ss_ok = false;

	read_id_raw(m, id, true);

	m->id[0] = id[0]; m->id[1] = id[1]; m->id[2] = id[2];

	// All-00 and all-ff are the two ways of reading nothing, and they
	// point at different halves of the board. Both are NODEV here;
	// the caller distinguishes them from m->id, because only the
	// caller has somewhere to say it.
	if ((id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00)
		|| (id[0] == 0xff && id[1] == 0xff && id[2] == 0xff))
		return Z_MMOD_NODEV;

	m->detected = true;

	// 0x7f is a JEDEC continuation code: the real manufacturer is
	// further along. Infineon/Cypress FRAM identifies this way, so
	// this is a positive FRAM signature rather than a failed read --
	// but the density is in bytes this 3-byte read did not fetch, so
	// the size still has to come from the caller.
	if (id[0] == 0x7f) {
		m->cls = Z_MMOD_FRAM;
		m->needs_erase = false;
		m->has_wip = false;
		m->page = 0;			// no page limit
		m->sector = 0;			// no erase
		m->addr_bytes = 3;		// the large parts that answer RDID
		m->size = 0;			// caller must set it
		return Z_MMOD_OK;
	}

	// Capacity is log2(bytes) on essentially every SPI NOR part.
	// Bounded rather than trusted: outside this range it is a device
	// that does not follow the convention or a misread, and reporting
	// a size of 2^195 would be worse than reporting none.
	if (id[2] >= 0x10 && id[2] <= 0x1b) {

		m->cls = Z_MMOD_NOR;
		m->size = 1ul << id[2];
		m->page = 256;
		m->sector = 4096;
		m->needs_erase = true;
		m->has_wip = true;

		// Over 16MB three bytes cannot reach the top half at all, so
		// pick four rather than silently addressing the bottom 16MB
		// of a 32MB part.
		m->addr_bytes = (m->size > 0x1000000ul) ? 4 : 3;

		return Z_MMOD_OK;

	}

	// Answered, but not in a shape this understands. Left for the
	// caller to describe; the ID is still worth showing.
	m->cls = Z_MMOD_UNKNOWN;
	m->size = 0;

	return Z_MMOD_OK;

}

bool z_mmod_ss_ok(z_mmod_t *m) {

	uint8_t idle[3];

	if (!m->detected) return false;

	// The same exchange with SS deasserted. A working device is not
	// listening, so this should read back as whatever the bus floats
	// to -- anything but the real ID.
	read_id_raw(m, idle, false);

	m->ss_ok = memcmp(idle, m->id, 3) != 0;

	return m->ss_ok;

}

// -- read -------------------------------------------------------

z_mmod_rv z_mmod_read(z_mmod_t *m, uint32_t addr, void *buf, uint32_t n) {

	uint8_t hdr[5];
	int hn;

	if (!n) return Z_MMOD_OK;
	if (m->addr_bytes < 2 || m->addr_bytes > 4) return Z_MMOD_BADPROFILE;
	if (m->size && (addr >= m->size || n > m->size - addr))
		return Z_MMOD_RANGE;

	hdr[0] = opcode(m, CMD_READ, CMD_READ4);
	hn = 1 + put_addr(m, hdr + 1, addr);

	z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, hdr, NULL, (uint32_t)hn);
	// tx NULL sends Z_SPI_TX_IDLE (0xff), which is what a device
	// expects to see while it is talking -- see sw/common/zspi.h on
	// why that is 0xff and not 0x00.
	z_spi_xfer(&m->spi, NULL, buf, n);
	z_spi_select(&m->spi, false);

	return Z_MMOD_OK;

}

// -- write ------------------------------------------------------

// One page-bounded program. Callers go through z_mmod_write().
static z_mmod_rv program(z_mmod_t *m, uint32_t addr, const uint8_t *p,
	uint32_t n) {

	uint8_t hdr[5];
	int hn;

	cmd_only(m, CMD_WREN);

	hdr[0] = opcode(m, CMD_PP, CMD_PP4);
	hn = 1 + put_addr(m, hdr + 1, addr);

	z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, hdr, NULL, (uint32_t)hn);
	z_spi_xfer(&m->spi, p, NULL, n);
	z_spi_select(&m->spi, false);

	return wait_ready(m, m->write_ms);

}

z_mmod_rv z_mmod_write(z_mmod_t *m, uint32_t addr, const void *buf,
	uint32_t n) {

	const uint8_t *p = buf;
	z_mmod_rv rv;

	if (!n) return Z_MMOD_OK;
	if (m->addr_bytes < 2 || m->addr_bytes > 4) return Z_MMOD_BADPROFILE;
	if (m->size && (addr >= m->size || n > m->size - addr))
		return Z_MMOD_RANGE;

	// A stuck chip select means the device is permanently selected and
	// every clock edge is a command byte to it. Refusing is the whole
	// reason z_mmod_ss_ok() exists.
	if (!m->ss_ok) return Z_MMOD_BADPROFILE;

	// On a device that cannot turn a 0 back into a 1, writing over
	// anything unerased produces the AND of old and new -- a write
	// that reports success and verifies wrong. Refuse instead, and
	// let the caller erase or ask blank_check() where the problem is.
	if (m->needs_erase) {
		uint32_t bad;
		rv = z_mmod_blank_check(m, addr, n, &bad);
		if (rv != Z_MMOD_OK) return rv;
	}

	while (n) {

		uint32_t chunk = n;

		// Page program cannot cross a page boundary: the address
		// wraps within the page instead of advancing, so the tail of
		// an over-long write lands back at the start and overwrites
		// its own beginning. page == 0 means no limit (FRAM).
		if (m->page) {
			uint32_t room = m->page - (addr % m->page);
			if (chunk > room) chunk = room;
		}

		rv = program(m, addr, p, chunk);
		if (rv != Z_MMOD_OK) return rv;

		addr += chunk;
		p += chunk;
		n -= chunk;

	}

	return Z_MMOD_OK;

}

// -- erase ------------------------------------------------------

z_mmod_rv z_mmod_erase_sector(z_mmod_t *m, uint32_t addr) {

	uint8_t hdr[5];
	int hn;

	if (!m->sector) return Z_MMOD_BADPROFILE;	// FRAM has no erase
	if (!m->ss_ok) return Z_MMOD_BADPROFILE;
	if (m->size && addr >= m->size) return Z_MMOD_RANGE;

	// Snap to the sector this address is in. The whole sector goes
	// whatever the caller passed, so doing the arithmetic here means
	// the caller cannot believe it erased less than it did.
	addr -= addr % m->sector;

	cmd_only(m, CMD_WREN);

	hdr[0] = opcode(m, CMD_SE, CMD_SE4);
	hn = 1 + put_addr(m, hdr + 1, addr);

	z_spi_select(&m->spi, true);
	z_spi_xfer(&m->spi, hdr, NULL, (uint32_t)hn);
	z_spi_select(&m->spi, false);

	return wait_ready(m, m->erase_ms);

}

// -- checks -----------------------------------------------------

z_mmod_rv z_mmod_blank_check(z_mmod_t *m, uint32_t addr, uint32_t n,
	uint32_t *first_bad) {

	uint8_t b[Z_MMOD_CHUNK];
	uint32_t off = 0;

	while (off < n) {

		uint32_t chunk = n - off;
		uint32_t i;
		z_mmod_rv rv;

		if (chunk > sizeof(b)) chunk = sizeof(b);

		rv = z_mmod_read(m, addr + off, b, chunk);
		if (rv != Z_MMOD_OK) return rv;

		for (i = 0; i < chunk; i++)
			if (b[i] != 0xff) {
				if (first_bad) *first_bad = off + i;
				return Z_MMOD_VERIFY;
			}

		off += chunk;

	}

	return Z_MMOD_OK;

}

z_mmod_rv z_mmod_verify(z_mmod_t *m, uint32_t addr, const void *buf,
	uint32_t n, uint32_t *first_bad) {

	const uint8_t *p = buf;
	uint8_t b[Z_MMOD_CHUNK];
	uint32_t off = 0;

	while (off < n) {

		uint32_t chunk = n - off;
		uint32_t i;
		z_mmod_rv rv;

		if (chunk > sizeof(b)) chunk = sizeof(b);

		rv = z_mmod_read(m, addr + off, b, chunk);
		if (rv != Z_MMOD_OK) return rv;

		for (i = 0; i < chunk; i++)
			if (b[i] != p[off + i]) {
				if (first_bad) *first_bad = off + i;
				return Z_MMOD_VERIFY;
			}

		off += chunk;

	}

	return Z_MMOD_OK;

}

void z_mmod_probe_widths(z_mmod_t *m, uint32_t addr,
	uint8_t out2[16], uint8_t out3[16], uint8_t out4[16]) {

	uint8_t saved = m->addr_bytes;

	// Only reads, so guessing is free here -- see zmmod.h on why the
	// same is emphatically not true of writes.
	m->addr_bytes = 2; z_mmod_read(m, addr, out2, 16);
	m->addr_bytes = 3; z_mmod_read(m, addr, out3, 16);
	m->addr_bytes = 4; z_mmod_read(m, addr, out4, 16);

	m->addr_bytes = saved;

}

// -- names ------------------------------------------------------

const char *z_mmod_strerror(z_mmod_rv rv) {
	switch (rv) {
	case Z_MMOD_OK:         return "ok";
	case Z_MMOD_NODEV:      return "no device";
	case Z_MMOD_BADPROFILE: return "profile does not allow this";
	case Z_MMOD_RANGE:      return "outside the device";
	case Z_MMOD_TIMEOUT:    return "timed out waiting for the device";
	case Z_MMOD_VERIFY:     return "data did not match";
	default:                return "unknown";
	}
}

const char *z_mmod_class_name(z_mmod_class_t c) {
	switch (c) {
	case Z_MMOD_NOR:    return "NOR";
	case Z_MMOD_FRAM:   return "FRAM";
	case Z_MMOD_EEPROM: return "EEPROM";
	default:            return "?";
	}
}

const char *z_mmod_vendor(uint8_t mfg) {
	switch (mfg) {
	case 0x01: return "Cypress/Spansion";
	case 0x04: return "Fujitsu";
	case 0x1f: return "Adesto/Atmel";
	case 0x20: return "Micron/ST";
	case 0x7f: return "JEDEC continuation";
	case 0x9d: return "ISSI";
	case 0xbf: return "SST";
	case 0xc2: return "Macronix";
	case 0xc8: return "GigaDevice";
	case 0xef: return "Winbond";
	default:   return "unknown";
	}
}
