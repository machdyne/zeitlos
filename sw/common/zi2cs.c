/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The GPIO I2C slave -- sw/common/zi2cs.h for the interface, docs/gpio.md
 * for the registers, rtl/gpio_i2cs.v for the hardware.
 *
 * Standalone, like zgpio_stream.c: it reads the GPIO block's MAGIC and
 * CONFIG itself. Every register access goes through RD() and WR(), which
 * sw/common/tests/test_i2cs.c points at a model of the slave.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zi2cs.h"
#include "zgpio_stream.h"		// the engines' registers, to keep off their pins

#ifdef Z_I2CS_HOST_TEST
uint32_t z_i2cs_test_rd(uint32_t addr);
void z_i2cs_test_wr(uint32_t addr, uint32_t val);
#define RD(a)    z_i2cs_test_rd(a)
#define WR(a, v) z_i2cs_test_wr((a), (v))
#else
#define RD(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define WR(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#endif

#define GPIO_MAGIC_ADDR  0xe0000008u
#define GPIO_CONFIG_ADDR 0xe000000cu
#define GPIO_MAGIC       0x5A475049u
#define GPIO_CONFIG_SIG  0x4750u

#define REG(off) (Z_I2CS_BASE + (off))

static uint32_t config_word(void) {
	uint32_t c;
	if (RD(GPIO_MAGIC_ADDR) != GPIO_MAGIC) return 0;
	c = RD(GPIO_CONFIG_ADDR);
	if (((c >> 16) & 0xffffu) != GPIO_CONFIG_SIG) return 0;
	return c;
}

uint32_t z_i2cs_slots(void) {
	uint32_t n = (config_word() >> 12) & 7u;
	return n > Z_I2CS_MAX_SLOTS ? Z_I2CS_MAX_SLOTS : n;
}

static bool slot_ok(int s) {
	return s >= 0 && (uint32_t)s < z_i2cs_slots();
}

// Does a configured stream engine have this pin? (zgpio_stream.c's
// z_gs_config() makes the same check the other way round.)
static bool engine_has(int pin) {
	uint32_t c = config_word();
	uint32_t n = (c >> 4) & 0xfu;
	for (uint32_t e = 0; e < n && e < Z_GS_MAX_ENGINES; e++) {
		uint32_t base = Z_GS_BASE + e * Z_GS_SIZE;
		uint32_t s;
		if ((RD(base + Z_GS_REG_SCTL) & 7u) == 0) continue;
		s = RD(base + Z_GS_REG_SPINS);
		for (int r = 0; r < 4; r++) {
			uint32_t b = (s >> (8 * r)) & 0xffu;
			if ((b & 0x80u) && (int)(b & 0x3fu) == pin) return true;
		}
	}
	return false;
}

int z_i2cs_open(int scl, int sda) {
	uint32_t c = config_word();
	int npins = (int)(c & 0xfu) * 8;
	if (!z_i2cs_slots()) return Z_I2CS_ENONE;
	if (scl < 0 || sda < 0 || scl >= npins || sda >= npins || scl == sda)
		return Z_I2CS_EPIN;
	if (RD(REG(Z_I2CS_REG_ICTL)) & 1u) return Z_I2CS_EBUSY;
	if (engine_has(scl) || engine_has(sda)) return Z_I2CS_EPINBUSY;
	for (int s = 0; s < Z_I2CS_MAX_SLOTS; s++)
		WR(REG(Z_I2CS_REG_ISLOT0 + 4 * (uint32_t)s), 1u << 15);	// unmapped, pointer 0
	WR(REG(Z_I2CS_REG_IFLUSH), 1);
	WR(REG(Z_I2CS_REG_ISTAT), 0xfu << 16);			// read flags
	WR(REG(Z_I2CS_REG_ICTL), 1u | ((uint32_t)scl << 8) | ((uint32_t)sda << 16));
	return Z_I2CS_OK;
}

void z_i2cs_close(void) {
	if (!z_i2cs_slots()) return;
	WR(REG(Z_I2CS_REG_ICTL), 0);
	for (int s = 0; s < Z_I2CS_MAX_SLOTS; s++)
		WR(REG(Z_I2CS_REG_ISLOT0 + 4 * (uint32_t)s), 0);
}

// ISLOT: [6:0] address, [7] valid, [9:8] auto-increment, [15] set the
// pointer to [31:24]; reads give the pointer in [31:24] and the last
// read's start in [23:16].
static uint32_t islot(int s) {
	return REG(Z_I2CS_REG_ISLOT0 + 4 * (uint32_t)s);
}

int z_i2cs_map(int slot, uint8_t addr, int inc) {
	if (!slot_ok(slot)) return Z_I2CS_ESLOT;
	WR(islot(slot), (uint32_t)(addr & 0x7fu) | 0x80u | (((uint32_t)inc & 3u) << 8));
	return Z_I2CS_OK;
}

void z_i2cs_unmap(int slot) {
	uint32_t v;
	if (!slot_ok(slot)) return;
	v = RD(islot(slot)) & 0x37fu;		// keep address and rule, drop valid
	WR(islot(slot), v);
}

void z_i2cs_set_ptr(int slot, uint8_t reg) {
	uint32_t v;
	if (!slot_ok(slot)) return;
	v = RD(islot(slot)) & 0x3ffu;
	WR(islot(slot), v | (1u << 15) | ((uint32_t)reg << 24));
}

uint8_t z_i2cs_ptr(int slot) {
	if (!slot_ok(slot)) return 0;
	return (uint8_t)(RD(islot(slot)) >> 24);
}

uint8_t z_i2cs_read_start(int slot) {
	if (!slot_ok(slot)) return 0;
	return (uint8_t)(RD(islot(slot)) >> 16);
}

static void maddr(int slot, uint8_t reg) {
	WR(REG(Z_I2CS_REG_IMADDR), ((uint32_t)slot << 8) | reg);
}

void z_i2cs_mirror_write(int slot, uint8_t reg, uint8_t val, bool writable) {
	if (!slot_ok(slot)) return;
	maddr(slot, reg);
	WR(REG(Z_I2CS_REG_IMDATA), (uint32_t)val | (writable ? 0x100u : 0));
}

uint8_t z_i2cs_mirror_read(int slot, uint8_t reg, bool *writable) {
	uint32_t v;
	if (!slot_ok(slot)) {
		if (writable) *writable = false;
		return 0;
	}
	maddr(slot, reg);
	v = RD(REG(Z_I2CS_REG_IMDATA));
	if (writable) *writable = (v & 0x100u) != 0;
	return (uint8_t)v;
}

void z_i2cs_mirror_load(int slot, uint8_t first, const uint8_t *vals,
                        uint32_t n, bool writable) {
	if (!slot_ok(slot) || !vals) return;
	if (n > 256u - first) n = 256u - first;		// IMADDR would run into the next slot
	maddr(slot, first);
	for (uint32_t i = 0; i < n; i++)		// IMADDR advances by itself
		WR(REG(Z_I2CS_REG_IMDATA), (uint32_t)vals[i] | (writable ? 0x100u : 0));
}

// ISTAT: [9:0] log level, [12] overflow, [13] busy, [19:16] read
// flags, [20] live. Writing 1 to bit 12 or 16-19 clears it.
uint32_t z_i2cs_log_level(void) {
	if (!z_i2cs_slots()) return 0;
	return RD(REG(Z_I2CS_REG_ISTAT)) & 0x3ffu;
}

bool z_i2cs_log_pop(z_i2cs_event_t *ev) {
	uint32_t v;
	if (!z_i2cs_slots()) return false;
	v = RD(REG(Z_I2CS_REG_ILOG));		// 0, or { valid, slot, reg, data }
	if (!(v & 0x80000000u)) return false;
	if (ev) {
		ev->slot = (uint8_t)((v >> 24) & 3u);
		ev->reg = (uint8_t)(v >> 8);
		ev->data = (uint8_t)v;
	}
	return true;
}

void z_i2cs_log_flush(void) {
	if (z_i2cs_slots()) WR(REG(Z_I2CS_REG_IFLUSH), 1);
}

bool z_i2cs_overflowed(bool clear) {
	bool o;
	if (!z_i2cs_slots()) return false;
	o = (RD(REG(Z_I2CS_REG_ISTAT)) >> 12) & 1u;
	if (o && clear) WR(REG(Z_I2CS_REG_ISTAT), 1u << 12);
	return o;
}

uint32_t z_i2cs_reads(bool clear) {
	uint32_t r;
	if (!z_i2cs_slots()) return 0;
	r = (RD(REG(Z_I2CS_REG_ISTAT)) >> 16) & 0xfu;
	// clear only what was seen, so a read in between is not lost
	if (r && clear) WR(REG(Z_I2CS_REG_ISTAT), r << 16);
	return r;
}

bool z_i2cs_busy(void) {
	if (!z_i2cs_slots()) return false;
	return (RD(REG(Z_I2CS_REG_ISTAT)) >> 13) & 1u;
}
