/*
 * Host test for sw/common/zi2cs.c, the GPIO I2C slave's driver, with
 * sw/common/zgpio_stream.c alongside (the two keep off each other's
 * pins).
 *
 *   make -C sw/common/tests -f Makefile.zi2cs
 *
 * The registers are modelled as rtl/gpio_i2cs.v documents them -- ICTL,
 * ISTAT with its write-1-to-clear bits, ISLOT, IMADDR/IMDATA advancing,
 * ILOG -- plus one stream engine's SCTL/SPINS/SLOCK. "The master" here
 * is the model's own functions putting entries in the log and setting
 * read flags; the protocol itself is rtl/tb/tb_gpio_i2cs.v's job. This
 * checks what the driver writes and how it reads what comes back.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zi2cs.h"
#include "zgpio_stream.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// -- the model --

static uint32_t slots_built = 4;
static uint32_t ictl, imaddr, islot[4], ovf, rdflags, busy;
static uint16_t mirror[1024];
static uint32_t logq[512];
static int log_n, log_h;
static uint32_t sctl, spins, slock;		// engine 0

static void master_write(int slot, uint8_t reg, uint8_t data) {
	if (log_n == 512) { ovf = 1; return; }
	logq[(log_h + log_n++) % 512] = (1u << 31) | ((uint32_t)slot << 24) | ((uint32_t)reg << 8) | data;
}

static void master_read(int slot) {
	rdflags |= 1u << slot;
	// the read starts at the pointer
	islot[slot] = (islot[slot] & 0xff00ffffu) | ((islot[slot] >> 24) << 16);
}

static uint32_t rd_i2cs(uint32_t off) {
	switch (off) {
	case Z_I2CS_REG_ICTL: return ictl;
	case Z_I2CS_REG_ISTAT:
		return (uint32_t)log_n | (ovf << 12) | (busy << 13) | (rdflags << 16) | ((ictl & 1u) << 20);
	case Z_I2CS_REG_IMADDR: return imaddr;
	case Z_I2CS_REG_IMDATA: { uint32_t v = mirror[imaddr]; imaddr = (imaddr + 1) & 0x3ffu; return v; }
	case Z_I2CS_REG_ILOG: {
		uint32_t v;
		if (!log_n) return 0;
		v = logq[log_h];
		log_h = (log_h + 1) % 512;
		log_n--;
		return v;
	}
	}
	if (off >= Z_I2CS_REG_ISLOT0 && off < Z_I2CS_REG_ISLOT0 + 16) {
		uint32_t s = (off - Z_I2CS_REG_ISLOT0) / 4;
		return s < slots_built ? islot[s] : 0;
	}
	return 0;
}

static void wr_i2cs(uint32_t off, uint32_t v) {
	switch (off) {
	case Z_I2CS_REG_ICTL: ictl = v & 0x3f3f01u; return;
	case Z_I2CS_REG_ISTAT:
		if (v & (1u << 12)) ovf = 0;
		rdflags &= ~((v >> 16) & 0xfu);
		return;
	case Z_I2CS_REG_IMADDR: imaddr = v & 0x3ffu; return;
	case Z_I2CS_REG_IMDATA: mirror[imaddr] = (uint16_t)(v & 0x1ffu); imaddr = (imaddr + 1) & 0x3ffu; return;
	case Z_I2CS_REG_IFLUSH: if (v & 1) { log_n = 0; ovf = 0; } return;
	}
	if (off >= Z_I2CS_REG_ISLOT0 && off < Z_I2CS_REG_ISLOT0 + 16) {
		uint32_t s = (off - Z_I2CS_REG_ISLOT0) / 4;
		if (s >= slots_built) return;
		islot[s] = (islot[s] & 0xffff0000u) | (v & 0x3ffu);
		if (v & (1u << 15)) islot[s] = (islot[s] & 0x00ffffffu) | (v & 0xff000000u);
	}
}

static uint32_t rd(uint32_t a) {
	if (a == 0xe0000008u) return 0x5A475049u;
	if (a == 0xe000000cu) return 0x47500000u | (slots_built << 12) | (0xFu << 8) | (1u << 4) | 2u;
	if (a >= Z_I2CS_BASE && a < Z_I2CS_BASE + 0x40) return rd_i2cs(a - Z_I2CS_BASE);
	if (a == Z_GS_BASE + Z_GS_REG_SCTL) return sctl;
	if (a == Z_GS_BASE + Z_GS_REG_SPINS) return spins;
	if (a == Z_GS_BASE + Z_GS_REG_SLOCK) { uint32_t l = slock; slock = 1; return l; }
	return 0;
}

static void wr(uint32_t a, uint32_t v) {
	if (a >= Z_I2CS_BASE && a < Z_I2CS_BASE + 0x40) { wr_i2cs(a - Z_I2CS_BASE, v); return; }
	if (a == Z_GS_BASE + Z_GS_REG_SCTL) sctl = v;
	if (a == Z_GS_BASE + Z_GS_REG_SPINS) spins = v;
	if (a == Z_GS_BASE + Z_GS_REG_SLOCK) slock = v & 1;
}

uint32_t z_i2cs_test_rd(uint32_t a) { return rd(a); }
void z_i2cs_test_wr(uint32_t a, uint32_t v) { wr(a, v); }
uint32_t z_gs_test_rd(uint32_t a) { return rd(a); }
void z_gs_test_wr(uint32_t a, uint32_t v) { wr(a, v); }

int main(void) {
	z_i2cs_event_t ev;
	uint8_t vals[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	bool w;

	printf("test_i2cs:\n");

	// not built: everything answers "none" and touches nothing
	slots_built = 0;
	CHECK(z_i2cs_slots() == 0, "no slots");
	CHECK(z_i2cs_open(2, 3) == Z_I2CS_ENONE, "open without the slave");
	CHECK(!z_i2cs_log_pop(&ev) && z_i2cs_log_level() == 0, "log without the slave");
	CHECK(z_i2cs_map(0, 0x20, Z_I2CS_INC_PAIR) == Z_I2CS_ESLOT, "map without the slave");
	slots_built = 4;

	// pins
	CHECK(z_i2cs_slots() == 4, "four slots");
	CHECK(z_i2cs_open(2, 2) == Z_I2CS_EPIN, "the same pin twice");
	CHECK(z_i2cs_open(2, 16) == Z_I2CS_EPIN, "a pin past the ports");
	// an engine on pin 3 keeps the slave off it...
	CHECK(z_gs_claim(0) == 0, "claim the engine");
	{
		z_gs_cfg_t c = Z_GS_CFG_INIT;
		c.mode = Z_GS_UART;
		c.tx = Z_GS_PIN(0, 3);
		CHECK(z_gs_config(0, &c) == Z_GS_OK, "engine on pin 3");
		CHECK(z_i2cs_open(2, 3) == Z_I2CS_EPINBUSY, "slave refused the engine's pin");
		z_gs_release(0);
	}
	CHECK(z_i2cs_open(2, 3) == Z_I2CS_OK, "open on 2 and 3");
	CHECK(ictl == (1u | (2u << 8) | (3u << 16)), "ICTL 0x%x", ictl);
	CHECK(z_i2cs_open(4, 5) == Z_I2CS_EBUSY, "a second open");
	// ...and the slave keeps an engine off its pins
	CHECK(z_gs_claim(0) == 0, "claim the engine again");
	{
		z_gs_cfg_t c = Z_GS_CFG_INIT;
		c.mode = Z_GS_UART;
		c.tx = Z_GS_PIN(0, 2);
		CHECK(z_gs_config(0, &c) == Z_GS_EPINBUSY, "engine refused the slave's pin");
		c.tx = Z_GS_PIN(0, 5);
		CHECK(z_gs_config(0, &c) == Z_GS_OK, "engine on another pin");
		z_gs_release(0);
	}

	// slots
	CHECK(z_i2cs_map(0, 0x20, Z_I2CS_INC_PAIR) == Z_I2CS_OK, "map");
	CHECK((islot[0] & 0x3ffu) == (0x20u | 0x80u | (2u << 8)), "ISLOT0 0x%x", islot[0]);
	CHECK(z_i2cs_map(4, 0x21, 0) == Z_I2CS_ESLOT, "slot 4");
	z_i2cs_set_ptr(0, 6);
	CHECK(z_i2cs_ptr(0) == 6 && (islot[0] & 0x3ffu) == 0x2a0u, "pointer set, mapping kept");
	z_i2cs_unmap(0);
	CHECK((islot[0] & 0x80u) == 0 && (islot[0] & 0x7fu) == 0x20, "unmapped, address kept");
	z_i2cs_map(0, 0x20, Z_I2CS_INC_PAIR);

	// the mirror
	z_i2cs_mirror_write(1, 7, 0x99, true);
	CHECK(mirror[256 + 7] == 0x199, "mirror write");
	CHECK(z_i2cs_mirror_read(1, 7, &w) == 0x99 && w, "mirror read");
	z_i2cs_mirror_load(0, 0, vals, 8, false);
	CHECK(mirror[0] == 1 && mirror[7] == 8 && !(mirror[3] & 0x100), "mirror load");
	z_i2cs_mirror_load(2, 252, vals, 8, true);		// stops at 255
	CHECK(mirror[512 + 255] == 0x104 && mirror[768] == 0, "load stops at the slot's end");

	// the log
	master_write(0, 2, 0xAA);
	master_write(1, 0x10, 0x55);
	CHECK(z_i2cs_log_level() == 2, "two logged");
	CHECK(z_i2cs_log_pop(&ev) && ev.slot == 0 && ev.reg == 2 && ev.data == 0xAA, "first entry");
	CHECK(z_i2cs_log_pop(&ev) && ev.slot == 1 && ev.reg == 0x10 && ev.data == 0x55, "second entry");
	CHECK(!z_i2cs_log_pop(&ev), "empty");
	for (int i = 0; i < 513; i++) master_write(1, (uint8_t)i, (uint8_t)i);
	CHECK(z_i2cs_overflowed(false) && z_i2cs_overflowed(true) && !z_i2cs_overflowed(false),
	      "overflow, then cleared");
	z_i2cs_log_flush();
	CHECK(z_i2cs_log_level() == 0, "flushed");

	// reads
	z_i2cs_set_ptr(0, 1);
	master_read(0);
	CHECK(z_i2cs_reads(false) == 1 && z_i2cs_read_start(0) == 1, "read flag and start");
	CHECK(z_i2cs_reads(true) == 1 && z_i2cs_reads(false) == 0, "cleared");

	z_i2cs_close();
	CHECK(ictl == 0 && !(islot[0] & 0x80u), "closed: disabled, unmapped");

	if (fails) { printf("test_i2cs: %d FAILED\n", fails); return 1; }
	printf("test_i2cs: PASS\n");
	return 0;
}
