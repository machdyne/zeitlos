/*
 * Host test for sw/common/zgpio_stream.c.
 *
 *   cc -std=gnu99 -Wall -DZ_GS_HOST_TEST -I sw/common -o /tmp/t \
 *      sw/common/tests/test_gpio_stream.c sw/common/zgpio_stream.c && /tmp/t
 *
 * The real .c, unmodified, against a FUNCTIONAL model of the engines
 * behind its RD()/WR() hooks: SCTL/SPINS/SRATE as registers, SLOCK as a
 * test-and-set, both FIFOs at their real depth with the K flag, SRX4's
 * packing, and SFLUSH. What the engine does on the wire is not modelled
 * -- that is rtl/tb/tb_gpio_stream.v's job -- except that bytes sent
 * come back: UART, raw and zlink loop TX into RX, and SPI answers each
 * byte with its complement, like a device that echoes inverted.
 *
 * What this proves: the API's arithmetic and bookkeeping -- dividers,
 * pin checks, claim/release across "processes", STX4/SRX4 packing,
 * holding a zlink K symbol back for z_gs_read_sym(), SPI transfers
 * longer than the FIFOs, write-only SPI, and CS without a reset.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zgpio_stream.h"

// -- the model ----------------------------------------------------

#define NE 2
static int nports = 2;
static uint32_t modes_bits = 0xF;	// { raw, uart, spi, zlink }
static bool magic_ok = true;

typedef struct {
	uint32_t sctl, spins, div, lock, serr;
	uint16_t tx[1024]; int txn;		// 9-bit entries
	uint16_t rx[1024]; int rxh, rxn;
	bool kword, txovf;
	int stx4_writes, srx4_reads;
} eng_t;
static eng_t eng[NE];

static void rx_push(eng_t *g, uint16_t v) {
	if (g->rxn < 1024) {
		g->rx[(g->rxh + g->rxn) % 1024] = v;
		g->rxn++;
	}
}

// "Send" whatever is in TX: the wire is a loop, SPI answers inverted.
static void run(eng_t *g) {
	uint32_t m = g->sctl & 7u;
	if (m == 0) return;
	for (int i = 0; i < g->txn; i++) {
		uint16_t v = g->tx[i];
		if (!(g->sctl & Z_GS_RXEN)) continue;
		if (m == Z_GS_SPI) rx_push(g, (uint16_t)(~v & 0xff));
		else rx_push(g, v);
	}
	g->txn = 0;
}

static uint16_t rx_pop(eng_t *g) {
	uint16_t v = g->rx[g->rxh];
	g->rxh = (g->rxh + 1) % 1024;
	g->rxn--;
	return v;
}

uint32_t z_gs_test_rd(uint32_t a) {
	if (a == 0xe0000008u) return magic_ok ? 0x5A475049u : 0;
	if (a == 0xe000000cu)
		return 0x47500000u | (modes_bits << 8) | (NE << 4) | (uint32_t)nports;
	int e = (int)((a - Z_GS_BASE) / Z_GS_SIZE);
	uint32_t r = (a - Z_GS_BASE) % Z_GS_SIZE;
	eng_t *g = &eng[e];
	switch (r) {
	case Z_GS_REG_SCTL: return g->sctl;
	case Z_GS_REG_SPINS: return g->spins;
	case Z_GS_REG_SRATE: return g->div;
	case Z_GS_REG_SSTAT:
		run(g);
		return (uint32_t)g->txn | ((uint32_t)g->rxn << 16) |
		       (g->kword ? Z_GS_ST_KWORD : 0) | (g->txovf ? Z_GS_ST_TXOVF : 0);
	case Z_GS_REG_SRX:
		run(g);
		if (!g->rxn) return 0;
		return 0x200u | rx_pop(g);
	case Z_GS_REG_SRX4: {
		uint32_t w = 0;
		run(g);
		g->srx4_reads++;
		if (g->rxn < 4) return 0;	// four or nothing, as in the RTL
		for (int i = 0; i < 4; i++) {
			uint16_t v = rx_pop(g);
			if (v & 0x100) g->kword = true;
			w |= (uint32_t)(v & 0xff) << (8 * i);
		}
		return w;
	}
	case Z_GS_REG_SERR: return g->serr;
	case Z_GS_REG_SLOCK: { uint32_t l = g->lock; g->lock = 1; return l; }
	}
	return 0;
}

void z_gs_test_wr(uint32_t a, uint32_t v) {
	int e = (int)((a - Z_GS_BASE) / Z_GS_SIZE);
	uint32_t r = (a - Z_GS_BASE) % Z_GS_SIZE;
	eng_t *g = &eng[e];
	switch (r) {
	case Z_GS_REG_SCTL: {
		uint32_t m = v & 7u;
		// a mode this build lacks reads back as off, as in the RTL
		if (m && !(modes_bits & (1u << (m - 1)))) v &= ~7u;
		g->sctl = v;
		break;
	}
	case Z_GS_REG_SPINS: g->spins = v; break;
	case Z_GS_REG_SRATE: g->div = v & 0xffff; break;
	case Z_GS_REG_STX:
		if (g->txn < 1024) g->tx[g->txn++] = (uint16_t)(v & 0x1ff);
		else g->txovf = true;
		break;
	case Z_GS_REG_STX4:
		g->stx4_writes++;
		for (int i = 0; i < 4; i++) {
			if (g->txn < 1024) g->tx[g->txn++] = (uint16_t)((v >> (8 * i)) & 0xff);
			else g->txovf = true;
		}
		break;
	case Z_GS_REG_SFLUSH:
		if (v & 1) g->txn = 0;
		if (v & 2) g->rxn = 0;
		if (v & 4) { g->serr = 0; g->kword = false; g->txovf = false; }
		break;
	case Z_GS_REG_SLOCK: g->lock = v & 1; break;
	}
}

// -- the tests ----------------------------------------------------

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static z_gs_cfg_t cfg(z_gs_mode_t m, int tx, int rx, int clk, int cs, uint32_t fl) {
	z_gs_cfg_t c = Z_GS_CFG_INIT;
	c.mode = m; c.tx = (int8_t)tx; c.rx = (int8_t)rx;
	c.clk = (int8_t)clk; c.cs = (int8_t)cs; c.flags = fl;
	return c;
}

int main(void) {
	uint32_t act;
	uint8_t buf[4096], out[4096];
	z_gs_cfg_t c;

	// presence
	CHECK(z_gs_count() == 2, "count %u", z_gs_count());
	CHECK(z_gs_modes() == 0x1E, "modes %x", z_gs_modes());
	magic_ok = false;
	CHECK(z_gs_count() == 0, "no magic, no engines");
	magic_ok = true;

	// dividers
	CHECK(z_gs_div_for(Z_GS_UART, 115200, &act) == 103 && act == 115384,
	      "uart 115200: %u", act);
	CHECK(z_gs_div_for(Z_GS_UART, 31250, &act) == 383 && act == 31250, "midi %u", act);
	CHECK(z_gs_div_for(Z_GS_UART, 3000000, &act) == 3 && act == 3000000, "3M %u", act);
	CHECK(z_gs_div_for(Z_GS_ZLINK, 12000000, &act) == 0 && act == 12000000, "zlink 12M");
	CHECK(z_gs_div_for(Z_GS_ZLINK, 50000000, &act) == 0 && act == 12000000, "clamped high");
	CHECK(z_gs_div_for(Z_GS_SPI, 12000000, &act) == 1 && act == 12000000, "spi 12M");
	CHECK(z_gs_div_for(Z_GS_SPI, 8000000, &act) == 2 && act == 8000000, "spi 8M");
	CHECK(z_gs_div_for(Z_GS_RAW, 48000000, &act) == 0 && act == 48000000, "raw 48M");
	CHECK(z_gs_div_for(Z_GS_UART, 50, &act) == 65535, "clamped low");

	// claim and release
	int a = z_gs_claim(Z_GS_ANY);
	int b = z_gs_claim(Z_GS_ANY);
	CHECK(a == 0 && b == 1, "claims %d %d", a, b);
	CHECK(z_gs_claim(Z_GS_ANY) == -1, "third claim");
	CHECK(z_gs_claim(0) == -1, "claim taken engine");
	z_gs_release(b);
	CHECK(z_gs_claim(1) == 1, "reclaim");

	// another process holds engine 1: this one may not configure it
	z_gs_release(1);
	eng[1].lock = 1;		// someone else's SLOCK
	c = cfg(Z_GS_UART, 0, 1, -1, -1, 0);
	CHECK(z_gs_config(1, &c) == Z_GS_EBADENGINE, "foreign engine");
	z_gs_force_release(1);
	CHECK(eng[1].lock == 0, "forced");

	// pin checks
	c = cfg(Z_GS_UART, 16, 1, -1, -1, 0);
	CHECK(z_gs_config(a, &c) == Z_GS_EPIN, "pin 16 on a 2-port board");
	c = cfg(Z_GS_UART, 3, 3, -1, -1, 0);
	CHECK(z_gs_config(a, &c) == Z_GS_EPINDUP, "dup");
	c = cfg(Z_GS_SPI, 5, 6, -1, 7, 0);
	CHECK(z_gs_config(a, &c) == Z_GS_ENOPIN, "spi without clk");
	c = cfg(Z_GS_UART, -1, -1, -1, -1, 0);
	CHECK(z_gs_config(a, &c) == Z_GS_ENOPIN, "uart without pins");
	c = cfg(Z_GS_UART, 2, -1, -1, -1, 0);
	CHECK(z_gs_config(a, &c) == Z_GS_OK, "1-wire uart");
	CHECK(eng[0].spins == 0x82, "spins %x", eng[0].spins);
	b = z_gs_claim(1);
	c = cfg(Z_GS_RAW, 9, 2, -1, -1, 0);
	CHECK(z_gs_config(b, &c) == Z_GS_EPINBUSY, "pin used by engine 0");
	modes_bits = 0xD;	// no SPI in this "build"
	c = cfg(Z_GS_SPI, 9, 10, 11, 12, 0);
	CHECK(z_gs_config(b, &c) == Z_GS_EMODE, "spi not built");
	modes_bits = 0xF;

	// UART loop: 3000 bytes through STX4/SRX4
	c = cfg(Z_GS_UART, Z_GS_PIN(1, 2), Z_GS_PIN(1, 6), -1, -1, Z_GS_RXEN);
	c.div = z_gs_div_for(Z_GS_UART, 115200, NULL);
	CHECK(z_gs_config(b, &c) == Z_GS_OK, "uart config");
	CHECK(eng[1].div == 103 && (eng[1].sctl & 0x3ff) == (3 | Z_GS_RXEN), "sctl %x", eng[1].sctl);
	for (int i = 0; i < 3000; i++) buf[i] = (uint8_t)(i * 7 + 3);
	{
		uint32_t s = 0, r = 0;
		eng[1].stx4_writes = eng[1].srx4_reads = 0;
		while (r < 3000) {
			s += z_gs_write(b, buf + s, 3000 - s);
			r += z_gs_read(b, out + r, 3000 - r);
		}
		CHECK(memcmp(buf, out, 3000) == 0, "uart loop data");
		CHECK(eng[1].stx4_writes >= 740, "STX4 used: %d", eng[1].stx4_writes);
		CHECK(eng[1].srx4_reads >= 740, "SRX4 used: %d", eng[1].srx4_reads);
	}

	// zlink: a K symbol in the stream is held back for z_gs_read_sym()
	c = cfg(Z_GS_ZLINK, Z_GS_PIN(1, 2), Z_GS_PIN(1, 6), -1, -1, Z_GS_RXEN);
	CHECK(z_gs_config(b, &c) == Z_GS_OK, "zlink config");
	z_gs_write(b, (const uint8_t *)"abc", 3);
	CHECK(z_gs_write_k(b, Z_GS_K27_7), "write K");
	z_gs_write(b, (const uint8_t *)"de", 2);
	memset(out, 0, 16);
	CHECK(z_gs_read(b, out, 16) == 3 && !memcmp(out, "abc", 3), "data before K");
	CHECK(z_gs_rx_level(b) == 3, "level counts the held K: %u", z_gs_rx_level(b));
	CHECK(z_gs_read(b, out, 16) == 0, "read stops at the K");
	CHECK(z_gs_read_sym(b) == (0x100 | Z_GS_K27_7), "the K");
	CHECK(z_gs_read(b, out, 16) == 2 && !memcmp(out, "de", 2), "data after K");
	CHECK(z_gs_read_sym(b) == -1, "empty");
	CHECK(!(z_gs_status(b) & Z_GS_ST_KWORD), "zlink never uses SRX4");

	// SPI: 3000 bytes, more than either FIFO, answered inverted
	z_gs_release(a);
	c = cfg(Z_GS_SPI, Z_GS_PIN(0, 5), Z_GS_PIN(0, 6), Z_GS_PIN(0, 4),
	        Z_GS_PIN(0, 7), Z_GS_RXEN | Z_GS_CS);
	c.div = z_gs_div_for(Z_GS_SPI, 12000000, NULL);
	CHECK(z_gs_config(b, &c) == Z_GS_OK, "spi config");
	z_gs_cs(b, false);
	CHECK((eng[1].sctl & 7) == Z_GS_SPI && !(eng[1].sctl & Z_GS_CS), "cs low keeps mode");
	CHECK(z_gs_spi_xfer(b, buf, out, 3000) == 3000, "spi xfer");
	{
		bool ok = true;
		for (int i = 0; i < 3000; i++) if (out[i] != (uint8_t)~buf[i]) ok = false;
		CHECK(ok, "spi data");
	}
	CHECK(z_gs_spi_xfer(b, NULL, out, 8) == 8 && out[0] == 0x00, "tx NULL sends 0xFF");
	z_gs_cs(b, true);
	CHECK(eng[1].sctl & Z_GS_CS, "cs high");

	// write-only SPI
	c.flags = Z_GS_CS;
	CHECK(z_gs_config(b, &c) == Z_GS_OK, "write-only config");
	CHECK(z_gs_spi_xfer(b, buf, NULL, 2000) == 2000, "write-only xfer");
	CHECK(eng[1].rxn == 0, "nothing received");

	// SPI calls refuse other modes
	c = cfg(Z_GS_RAW, Z_GS_PIN(0, 0), -1, -1, -1, 0);
	CHECK(z_gs_config(b, &c) == Z_GS_OK, "raw config");
	CHECK(z_gs_spi_xfer(b, buf, out, 4) == -1, "xfer in raw mode");

	z_gs_release(b);
	CHECK(eng[1].sctl == 0 && eng[1].spins == 0 && eng[1].lock == 0, "released");

	if (fails) { printf("test_gpio_stream: %d FAILED\n", fails); return 1; }
	printf("test_gpio_stream: PASS\n");
	return 0;
}
