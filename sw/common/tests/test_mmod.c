/*
 * Host test for sw/common/zmmod.c.
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/t \
 *      sw/common/tests/test_mmod.c sw/common/zmmod.c && /tmp/t
 *
 * -- What is simulated, and at which level --
 *
 * The SPI LAYER, not the pins. z_spi_select() and z_spi_xfer() are
 * replaced by a byte-level model of an SPI flash: it decodes opcodes,
 * consumes an address of whatever width it is configured for, enforces
 * page-program wrap, honours write-enable, and reports busy for a
 * settable number of status reads.
 *
 * That is the right level for this file, and it is a different choice
 * from sw/common/tests/test_bitbang.c, which models actual pins
 * because what it was testing was pin wiggling. What zmmod.c does is
 * emit correct command sequences -- opcodes, address widths, page
 * splits, WREN before every program, waiting for WIP. A pin-level
 * model would test all of that too, plus zspi.c a second time, more
 * slowly and with more to go wrong in the harness.
 *
 * -- What this therefore cannot catch --
 *
 * Anything below z_spi_xfer(): clock modes, bit order, chip-select
 * timing. test_bitbang.c covers those. And nothing here is
 * electrical.
 *
 * -- The model is deliberately strict --
 *
 * It faults on a program that crosses a page boundary, on a program
 * without a preceding WREN, and on writing a 1 over a 0 without an
 * erase. Those are the mistakes worth catching, and a permissive
 * model would let all three through -- real flash does not fault on
 * them either, it just silently does something else, which is exactly
 * why they are hard to find on hardware.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "zmmod.h"

// -- harness ----------------------------------------------------

static int fails;

#define CK(what, got, want) do { \
	long _g = (long)(got), _w = (long)(want); \
	if (_g != _w) { \
		printf("FAIL %-40s got %ld want %ld\n", what, _g, _w); fails++; \
	} else printf("ok   %-40s %ld\n", what, _g); \
} while (0)

// -- the simulated device ---------------------------------------

#define DEV_SIZE   (1u << 16)		// 64KB is enough to exercise everything
#define DEV_PAGE   256
#define DEV_SECTOR 4096

static struct {
	uint8_t  mem[DEV_SIZE];
	uint8_t  id[3];
	int      addr_bytes;		// what the DEVICE expects
	bool     needs_erase;
	bool     selected;
	bool     wren;
	int      busy;				// status reads remaining before ready
	// current transaction
	int      phase;				// 0 opcode, 1 address, 2 data
	uint8_t  op;
	int      addr_got;
	uint32_t addr;
	uint32_t data_n;
	uint32_t page_base;
	// faults the model refuses to paper over
	int      fault_page_cross;
	int      fault_no_wren;
	int      fault_unerased;
	int      programs;
	int      erases;
	int      wren_count;
} dev;

static void dev_reset(int addr_bytes, bool needs_erase) {
	memset(&dev, 0, sizeof(dev));
	memset(dev.mem, 0xff, sizeof(dev.mem));
	dev.id[0] = 0xef; dev.id[1] = 0x40; dev.id[2] = 0x10;	// 64KB NOR
	dev.addr_bytes = addr_bytes;
	dev.needs_erase = needs_erase;
}

// One byte in, one byte out -- which is what SPI is.
static uint8_t dev_byte(uint8_t in) {

	uint8_t out = 0xff;

	// Deselected devices do not listen and do not drive. Modelling
	// that is not pedantry: it is what makes z_mmod_ss_ok() mean
	// something, and a model that answered regardless would have the
	// bus permanently wedged -- which is precisely the hardware
	// failure that check exists to detect.
	if (!dev.selected) return 0xff;

	if (dev.phase == 0) {

		dev.op = in;
		dev.addr_got = 0;
		dev.addr = 0;
		dev.data_n = 0;

		switch (in) {
		case 0x06: dev.wren = true; dev.wren_count++; dev.phase = 2; break;
		case 0x05: dev.phase = 2; break;					// RDSR
		case 0x9f: dev.phase = 2; break;					// RDID
		case 0x03: case 0x13:								// READ
		case 0x02: case 0x12:								// PP
		case 0x20: case 0x21:								// SE
			dev.phase = 1; break;
		default: dev.phase = 2; break;
		}

		return 0xff;

	}

	if (dev.phase == 1) {
		dev.addr = (dev.addr << 8) | in;
		if (++dev.addr_got >= dev.addr_bytes) {
			dev.phase = 2;
			dev.page_base = dev.addr - (dev.addr % DEV_PAGE);
			if (dev.op == 0x20 || dev.op == 0x21) {
				// erase acts on the whole sector at once
				uint32_t base = dev.addr - (dev.addr % DEV_SECTOR);
				if (!dev.wren) dev.fault_no_wren++;
				memset(dev.mem + (base % DEV_SIZE), 0xff, DEV_SECTOR);
				dev.wren = false;
				dev.busy = 3;
				dev.erases++;
			}
		}
		return 0xff;
	}

	// phase 2: data
	switch (dev.op) {

	case 0x05:									// RDSR
		out = dev.busy > 0 ? 0x01 : 0x00;
		if (dev.busy > 0) dev.busy--;
		break;

	case 0x9f:									// RDID
		out = dev.data_n < 3 ? dev.id[dev.data_n] : 0xff;
		break;

	case 0x03: case 0x13:						// READ
		out = dev.mem[(dev.addr + dev.data_n) % DEV_SIZE];
		break;

	case 0x02: case 0x12: {						// PAGE PROGRAM
		uint32_t a = dev.addr + dev.data_n;
		if (dev.data_n == 0) {
			if (!dev.wren) dev.fault_no_wren++;
			dev.programs++;
		}
		// Real flash WRAPS within the page rather than advancing, so
		// an over-long program overwrites its own start. Faulting is
		// how a test catches what hardware would silently do.
		if (a >= dev.page_base + DEV_PAGE) dev.fault_page_cross++;
		a %= DEV_SIZE;
		if (dev.needs_erase && (~dev.mem[a] & in)) dev.fault_unerased++;
		dev.mem[a] = dev.needs_erase ? (dev.mem[a] & in) : in;
		break;
	}

	default:
		break;

	}

	dev.data_n++;

	return out;

}

// -- z_spi_* replaced -------------------------------------------

bool z_spi_init(z_spi_t *s) { (void)s; return true; }

void z_spi_select(z_spi_t *s, bool on) {
	(void)s;
	if (on) {
		dev.selected = true;
		dev.phase = 0;
	} else {
		// Deselect ends the transaction. A program only commits on
		// the rising edge of CS on real parts, which is why this is
		// where busy starts rather than at the last data byte.
		if ((dev.op == 0x02 || dev.op == 0x12) && dev.selected) {
			dev.wren = false;
			dev.busy = 3;
		}
		dev.selected = false;
		dev.phase = 0;
	}
}

uint8_t z_spi_xfer8(z_spi_t *s, uint8_t o) { (void)s; return dev_byte(o); }

void z_spi_xfer(z_spi_t *s, const uint8_t *tx, uint8_t *rx, uint32_t n) {
	uint32_t i;
	(void)s;
	for (i = 0; i < n; i++) {
		uint8_t got = dev_byte(tx ? tx[i] : 0xff);
		if (rx) rx[i] = got;
	}
}

// zmmod.c's wait_ready() uses wall-clock ticks; the model counts
// status reads instead, so these only have to advance.
uint32_t z_uptime_ticks(void) { static uint32_t t; return t++; }

// -- tests ------------------------------------------------------

int main(void) {

	z_mmod_t m;
	uint8_t buf[600], back[600];
	uint32_t bad;
	uint32_t i;
	z_mmod_rv rv;

	// -- detect --

	dev_reset(3, true);
	z_mmod_init(&m, 0, 1000);
	CK("detect", z_mmod_detect(&m), Z_MMOD_OK);
	CK("  class NOR", m.cls, Z_MMOD_NOR);
	CK("  size 64K", m.size, 65536);
	CK("  3-byte addressing", m.addr_bytes, 3);
	CK("  page 256", m.page, 256);
	CK("ss check passes", z_mmod_ss_ok(&m), 1);

	// A 32MB part must pick 4-byte addressing on its own: three bytes
	// cannot reach the top half at all.
	dev_reset(3, true);
	dev.id[2] = 0x19;
	z_mmod_init(&m, 0, 1000);
	z_mmod_detect(&m);
	CK("32MB picks 4-byte addressing", m.addr_bytes, 4);
	CK("  size 32MB", m.size, 32u * 1024u * 1024u);

	// JEDEC continuation is a FRAM signature, not a failed read.
	dev_reset(3, false);
	dev.id[0] = 0x7f;
	z_mmod_init(&m, 0, 1000);
	z_mmod_detect(&m);
	CK("7f detects as FRAM", m.cls, Z_MMOD_FRAM);
	CK("  no erase", m.needs_erase, 0);
	CK("  no page limit", m.page, 0);

	dev_reset(3, true);
	memset(dev.id, 0xff, 3);
	z_mmod_init(&m, 0, 1000);
	CK("all-ff is no device", z_mmod_detect(&m), Z_MMOD_NODEV);

	dev_reset(3, true);
	memset(dev.id, 0x00, 3);
	z_mmod_init(&m, 0, 1000);
	CK("all-00 is no device", z_mmod_detect(&m), Z_MMOD_NODEV);

	// -- read and write --

	dev_reset(3, true);
	z_mmod_init(&m, 0, 1000);
	z_mmod_detect(&m);
	z_mmod_ss_ok(&m);

	CK("blank check on erased device", z_mmod_blank_check(&m, 0, 512, &bad),
		Z_MMOD_OK);

	for (i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 7 + 3);

	// 600 bytes from offset 0x40 spans three pages and starts
	// mid-page: the case that catches a missing page split.
	CK("write 600 across 3 pages", z_mmod_write(&m, 0x40, buf, 600),
		Z_MMOD_OK);
	CK("  no page-boundary crossing", dev.fault_page_cross, 0);
	CK("  no unerased overwrite", dev.fault_unerased, 0);
	CK("  no missing WREN", dev.fault_no_wren, 0);
	CK("  one program per page", dev.programs, 3);
	CK("  one WREN per program", dev.wren_count, 3);

	CK("read back", z_mmod_read(&m, 0x40, back, 600), Z_MMOD_OK);
	CK("  matches", memcmp(back, buf, 600), 0);
	CK("verify agrees", z_mmod_verify(&m, 0x40, buf, 600, &bad), Z_MMOD_OK);

	buf[100] ^= 0xff;
	CK("verify catches a difference",
		z_mmod_verify(&m, 0x40, buf, 600, &bad), Z_MMOD_VERIFY);
	CK("  at the right offset", bad, 100);
	buf[100] ^= 0xff;

	// -- erase --

	CK("blank check now fails", z_mmod_blank_check(&m, 0x40, 600, &bad),
		Z_MMOD_VERIFY);
	CK("  at the first written byte", bad, 0);

	CK("erase sector", z_mmod_erase_sector(&m, 0x100), Z_MMOD_OK);
	CK("  blank again", z_mmod_blank_check(&m, 0, DEV_SECTOR, &bad),
		Z_MMOD_OK);

	// -- the refusals --

	CK("write over unerased data refuses",
		z_mmod_write(&m, 0x40, buf, 16), Z_MMOD_OK);	// erased, so fine
	CK("  second write to the same place refuses",
		z_mmod_write(&m, 0x40, buf, 16), Z_MMOD_VERIFY);
	CK("  and did not touch the device", dev.fault_unerased, 0);

	m.ss_ok = false;
	CK("write refuses with SS unproven",
		z_mmod_write(&m, 0x2000, buf, 16), Z_MMOD_BADPROFILE);
	CK("erase refuses with SS unproven",
		z_mmod_erase_sector(&m, 0x2000), Z_MMOD_BADPROFILE);
	m.ss_ok = true;

	CK("read past the end refuses",
		z_mmod_read(&m, DEV_SIZE - 4, back, 16), Z_MMOD_RANGE);
	CK("write past the end refuses",
		z_mmod_write(&m, DEV_SIZE - 4, buf, 16), Z_MMOD_RANGE);

	m.addr_bytes = 1;
	CK("bad address width refuses",
		z_mmod_read(&m, 0, back, 4), Z_MMOD_BADPROFILE);
	m.addr_bytes = 3;

	// -- address width actually reaches the device --
	//
	// The model consumes exactly `dev.addr_bytes`, so a mismatch
	// desynchronises the transaction and the data comes back wrong.
	// That is what makes this a real check rather than a tautology.

	dev_reset(2, true);
	z_mmod_init(&m, 0, 1000);
	z_mmod_detect(&m);
	z_mmod_ss_ok(&m);
	m.addr_bytes = 2;
	m.size = DEV_SIZE;
	for (i = 0; i < 8; i++) buf[i] = (uint8_t)(0xa0 + i);
	CK("2-byte device, 2-byte write", z_mmod_write(&m, 0x30, buf, 8),
		Z_MMOD_OK);
	z_mmod_read(&m, 0x30, back, 8);
	CK("  reads back", memcmp(back, buf, 8), 0);

	m.addr_bytes = 3;
	z_mmod_read(&m, 0x30, back, 8);
	CK("wrong width reads wrong data", memcmp(back, buf, 8) != 0, 1);

	// -- FRAM: no erase, no page limit --

	dev_reset(3, false);
	dev.id[0] = 0x7f;
	z_mmod_init(&m, 0, 1000);
	z_mmod_detect(&m);
	z_mmod_ss_ok(&m);
	m.size = DEV_SIZE;

	for (i = 0; i < 600; i++) buf[i] = (uint8_t)(i ^ 0x5a);
	CK("FRAM write 600 in one go", z_mmod_write(&m, 0x40, buf, 600),
		Z_MMOD_OK);
	CK("  one program, no page split", dev.programs, 1);
	z_mmod_read(&m, 0x40, back, 600);
	CK("  reads back", memcmp(back, buf, 600), 0);

	// Overwriting without erasing is normal on FRAM, and the write
	// path must not blank-check it.
	for (i = 0; i < 600; i++) buf[i] = (uint8_t)(i ^ 0x33);
	CK("FRAM overwrite in place", z_mmod_write(&m, 0x40, buf, 600),
		Z_MMOD_OK);
	z_mmod_read(&m, 0x40, back, 600);
	CK("  reads back the new data", memcmp(back, buf, 600), 0);

	CK("FRAM erase refuses", z_mmod_erase_sector(&m, 0), Z_MMOD_BADPROFILE);

	printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);

	return fails != 0;

}
