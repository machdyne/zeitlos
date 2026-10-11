/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Fast zlink -- sw/common/zfast.h for the interface, rtl/zfast.v for the
 * hardware. Every register access goes through RD()/WR(), which
 * sw/common/tests/test_zlink_fast.c points at a model of two blocks.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zfast.h"

#ifdef Z_ZF_HOST_TEST
uint32_t z_zf_test_rd(uint32_t addr);
void z_zf_test_wr(uint32_t addr, uint32_t val);
#define RD(a)    z_zf_test_rd(a)
#define WR(a, v) z_zf_test_wr((a), (v))
#else
#define RD(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define WR(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#endif

#define GPIO_MAGIC_ADDR  0xe0000008u
#define GPIO_CONFIG_ADDR 0xe000000cu
#define GPIO_MAGIC       0x5A475049u
#define GPIO_CONFIG_SIG  0x4750u

#define REG(off) (Z_ZF_BASE + (off))

// a control symbol z_zf_read() met, held for z_zf_read_sym()
#ifdef Z_ZF_HOST_TEST
// two boards in one process: the test says which one is running
extern int z_zf_test_side;
extern int16_t z_zf_test_pending[2];
#define pending z_zf_test_pending[z_zf_test_side]
#else
static int16_t pending = -1;
#endif

bool z_zf_present(void) {
	uint32_t c;
	if (RD(GPIO_MAGIC_ADDR) != GPIO_MAGIC) return false;
	c = RD(GPIO_CONFIG_ADDR);
	if (((c >> 16) & 0xffffu) != GPIO_CONFIG_SIG) return false;
	return (c >> 15) & 1u;			// gpio.v's CONFIG: fast zlink built
}

bool z_zf_tx_ds2(void) {
	return z_zf_present() && ((RD(REG(Z_ZF_REG_FCTL)) >> 16) & 1u);
}

bool z_zf_claim(void) {
	if (!z_zf_present()) return false;
	if (RD(REG(Z_ZF_REG_FLOCK)) & 1u) return false;		// reading takes it
	pending = -1;
	return true;
}

void z_zf_release(void) {
	if (!z_zf_present()) return;
	z_zf_off();
	WR(REG(Z_ZF_REG_FFLUSH), 7);
	WR(REG(Z_ZF_REG_FLOCK), 0);
}

void z_zf_config(int tx, int rx, int rate, uint32_t flags) {
	uint32_t v = 1u | (((uint32_t)tx & 3u) << 1) | ((rx == Z_ZF_DS2) ? (1u << 3) : 0) |
	             (((uint32_t)rate & 3u) << 6) |
	             (flags & (Z_ZF_RXEN | Z_ZF_LOOP | Z_ZF_TXINV | Z_ZF_RXINV));
	WR(REG(Z_ZF_REG_FCTL), v);
}

void z_zf_off(void) {
	WR(REG(Z_ZF_REG_FCTL), 0);
	pending = -1;
}

uint32_t z_zf_bps(int rate) {
	return rate == Z_ZF_25M ? 25000000u : rate == Z_ZF_50M ? 50000000u : 100000000u;
}

uint32_t z_zf_status(void) { return RD(REG(Z_ZF_REG_FSTAT)); }
uint32_t z_zf_tx_free(void) { return Z_ZF_FIFO_DEPTH - Z_ZF_ST_TXLEVEL(z_zf_status()); }
uint32_t z_zf_rx_level(void) {
	return Z_ZF_ST_RXLEVEL(z_zf_status()) + (pending >= 0 ? 1u : 0u);
}
bool z_zf_aligned(void) { return (z_zf_status() & Z_ZF_ST_ALIGNED) != 0; }
uint32_t z_zf_errors(void) { return RD(REG(Z_ZF_REG_FERR)) & 0xffffu; }

void z_zf_flush(bool tx, bool rx) {
	WR(REG(Z_ZF_REG_FFLUSH), (tx ? 1u : 0) | (rx ? 2u : 0) | 4u);
	if (rx) pending = -1;
}

uint32_t z_zf_write(const uint8_t *buf, uint32_t n) {
	uint32_t free = z_zf_tx_free(), done = 0;
	if (n > free) n = free;
	while (n - done >= 4u) {
		WR(REG(Z_ZF_REG_FTX4),
		   (uint32_t)buf[done] | ((uint32_t)buf[done + 1] << 8) |
		   ((uint32_t)buf[done + 2] << 16) | ((uint32_t)buf[done + 3] << 24));
		done += 4u;
	}
	while (done < n) WR(REG(Z_ZF_REG_FTX), buf[done++]);
	return done;
}

bool z_zf_write_k(uint8_t k) {
	if (z_zf_tx_free() == 0) return false;
	WR(REG(Z_ZF_REG_FTX), 0x100u | k);
	return true;
}

uint32_t z_zf_read_words(uint32_t *w, uint32_t n) {
	uint32_t got = 0, words;
	if (pending >= 0) return 0;
	words = Z_ZF_ST_RXLEVEL(z_zf_status()) / 4u;
	if (n > words) n = words;
	while (got < n) w[got++] = RD(REG(Z_ZF_REG_FRX4));
	return got;
}

bool z_zf_kword(void) {
	return (z_zf_status() >> 15) & 1u;
}

int z_zf_read_sym(void) {
	uint32_t v;
	if (pending >= 0) {
		int p = pending;
		pending = -1;
		return p;
	}
	v = RD(REG(Z_ZF_REG_FRX));
	if (!(v & 0x200u)) return -1;
	return (int)(v & 0x1ffu);
}

uint32_t z_zf_read(uint8_t *buf, uint32_t n) {
	uint32_t got = 0, level;
	if (pending >= 0) return 0;			// a K symbol is next
	level = Z_ZF_ST_RXLEVEL(z_zf_status());
	if (n > level) n = level;
	while (got < n) {
		uint32_t v = RD(REG(Z_ZF_REG_FRX));
		if (!(v & 0x200u)) break;
		if (v & 0x100u) {				// a control symbol: hold it
			pending = (int16_t)(v & 0x1ffu);
			break;
		}
		buf[got++] = (uint8_t)v;
	}
	return got;
}
