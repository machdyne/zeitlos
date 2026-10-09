/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO stream engines -- see sw/common/zgpio_stream.h for the
 * interface, docs/gpio.md for the registers and rtl/gpio_stream.v for
 * the hardware.
 *
 * Standalone: this does not link sw/common/zgpio.c. It reads the GPIO
 * block's MAGIC and CONFIG itself (the same two checks zgpio.c makes),
 * so an app that only wants an engine does not carry the pin API.
 *
 * Every register access goes through RD() and WR(). On the target they
 * are plain volatile loads and stores; sw/common/tests/test_gpio_stream.c
 * builds this file with Z_GS_HOST_TEST and supplies a functional model
 * of the engines behind them.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zgpio_stream.h"

#ifdef Z_GS_HOST_TEST
uint32_t z_gs_test_rd(uint32_t addr);
void z_gs_test_wr(uint32_t addr, uint32_t val);
#define RD(a)    z_gs_test_rd(a)
#define WR(a, v) z_gs_test_wr((a), (v))
#else
#define RD(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define WR(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#endif

#define GPIO_MAGIC_ADDR  0xe0000008u
#define GPIO_CONFIG_ADDR 0xe000000cu
#define GPIO_MAGIC       0x5A475049u
#define GPIO_CONFIG_SIG  0x4750u

// Every engine runs off the 48 MHz bus clock (docs/gpio.md).
#define CLK_HZ 48000000u

static inline uint32_t ereg(int e, uint32_t off) {
	return Z_GS_BASE + (uint32_t)e * Z_GS_SIZE + off;
}

// Engines this PROCESS has claimed. SLOCK says an engine is taken; this
// says it was taken by us, so z_gs_config() can refuse an engine that
// some other process owns.
static uint8_t claimed;

// zlink: a control symbol z_gs_read() popped while looking for data,
// held for z_gs_read_sym(). The hardware FIFO cannot be peeked.
static int16_t pending[Z_GS_MAX_ENGINES] = { -1, -1, -1, -1 };

// -- presence ----------------------------------------------------

static uint32_t config_word(void) {
	uint32_t c;
	if (RD(GPIO_MAGIC_ADDR) != GPIO_MAGIC) return 0;
	c = RD(GPIO_CONFIG_ADDR);
	// The signature guards against a bitstream from before CONFIG had
	// one (zgpio.c explains); without it the engine bits mean nothing.
	if (((c >> 16) & 0xffffu) != GPIO_CONFIG_SIG) return 0;
	return c;
}

static uint32_t port_count(void) {
	return config_word() & 0xfu;
}

uint32_t z_gs_count(void) {
	uint32_t n = (config_word() >> 4) & 0xfu;
	return n > Z_GS_MAX_ENGINES ? Z_GS_MAX_ENGINES : n;
}

uint32_t z_gs_modes(void) {
	uint32_t m = (config_word() >> 8) & 0xfu;	// { raw, uart, spi, zlink }
	if (z_gs_count() == 0) return 0;
	return m << 1;								// bit (1 << mode)
}

static bool engine_ok(int e) {
	return e >= 0 && (uint32_t)e < z_gs_count();
}

static bool mine(int e) {
	return engine_ok(e) && (claimed & (1u << e));
}

// -- ownership ---------------------------------------------------

int z_gs_claim(int engine) {
	uint32_t n = z_gs_count();
	for (uint32_t e = 0; e < n; e++) {
		if (engine != Z_GS_ANY && (int)e != engine) continue;
		// Test-and-set: reading SLOCK takes it. A 0 back means it was
		// free and is now ours.
		if ((RD(ereg((int)e, Z_GS_REG_SLOCK)) & 1u) == 0) {
			claimed |= (uint8_t)(1u << e);
			pending[e] = -1;
			return (int)e;
		}
	}
	return -1;
}

static void off(int e) {
	WR(ereg(e, Z_GS_REG_SCTL), 0);		// pins back to GPIO
	WR(ereg(e, Z_GS_REG_SPINS), 0);
	WR(ereg(e, Z_GS_REG_SFLUSH), 7);	// FIFOs, sticky flags, errors
	pending[e] = -1;
}

void z_gs_release(int e) {
	if (!mine(e)) return;
	off(e);
	claimed &= (uint8_t)~(1u << e);
	WR(ereg(e, Z_GS_REG_SLOCK), 0);
}

void z_gs_force_release(int e) {
	if (!engine_ok(e)) return;
	off(e);
	claimed &= (uint8_t)~(1u << e);
	WR(ereg(e, Z_GS_REG_SLOCK), 0);
}

// -- configuration -----------------------------------------------

static uint32_t role_byte(int8_t pin) {
	if (pin < 0) return 0;
	return 0x80u | ((uint32_t)pin & 0x3fu);
}

// Does an enabled role of SPINS `s` sit on `pin`?
static bool spins_has(uint32_t s, int pin) {
	for (int r = 0; r < 4; r++) {
		uint32_t b = (s >> (8 * r)) & 0xffu;
		if ((b & 0x80u) && (int)(b & 0x3fu) == pin) return true;
	}
	return false;
}

int z_gs_config(int e, const z_gs_cfg_t *c) {
	int8_t pins[4];
	uint32_t npins, sctl;

	if (!mine(e)) return Z_GS_EBADENGINE;
	if (c->mode != Z_GS_OFF && !(z_gs_modes() & (1u << c->mode)))
		return Z_GS_EMODE;

	// What each mode cannot do without. 1-wire zlink, UART and raw are
	// fine: transmit-only or receive-only.
	switch (c->mode) {
	case Z_GS_SPI:
		if (c->clk < 0 || (c->tx < 0 && c->rx < 0)) return Z_GS_ENOPIN;
		break;
	case Z_GS_ZLINK: case Z_GS_UART: case Z_GS_RAW:
		if (c->tx < 0 && c->rx < 0) return Z_GS_ENOPIN;
		break;
	default:
		break;
	}

	pins[0] = c->tx; pins[1] = c->rx; pins[2] = c->clk; pins[3] = c->cs;
	npins = port_count() * 8u;
	for (int i = 0; i < 4; i++) {
		if (pins[i] < 0) continue;
		if ((uint32_t)pins[i] >= npins) return Z_GS_EPIN;
		for (int j = 0; j < i; j++)
			if (pins[j] == pins[i]) return Z_GS_EPINDUP;
		// an engine that is on owns its pins; two on one pin would
		// fight (the hardware ORs them -- docs/gpio.md)
		for (int k = 0; k < (int)z_gs_count(); k++) {
			if (k == e) continue;
			if ((RD(ereg(k, Z_GS_REG_SCTL)) & 7u) == 0) continue;
			if (spins_has(RD(ereg(k, Z_GS_REG_SPINS)), pins[i]))
				return Z_GS_EPINBUSY;
		}
	}

	// Off first, so the pins are released while the roles move, then
	// everything else, then the mode -- which is what takes the pins.
	off(e);
	WR(ereg(e, Z_GS_REG_SPINS),
	   role_byte(c->tx) | (role_byte(c->rx) << 8) |
	   (role_byte(c->clk) << 16) | (role_byte(c->cs) << 24));
	WR(ereg(e, Z_GS_REG_SRATE), c->div);
	sctl = (uint32_t)c->mode | (c->flags & Z_GS_FLAGS) |
	       ((uint32_t)(c->spi_delay & 7u) << 16);
	WR(ereg(e, Z_GS_REG_SCTL), sctl);
	return Z_GS_OK;
}

// Clocks per bit for a mode: zlink and UART sample 4x, SPI spends one
// tick per SCK edge, raw one tick per bit.
static uint32_t ticks_per_bit(z_gs_mode_t m) {
	switch (m) {
	case Z_GS_ZLINK: case Z_GS_UART: return 4;
	case Z_GS_SPI: return 2;
	default: return 1;
	}
}

uint32_t z_gs_rate(z_gs_mode_t mode, uint16_t div) {
	return CLK_HZ / (ticks_per_bit(mode) * ((uint32_t)div + 1u));
}

uint16_t z_gs_div_for(z_gs_mode_t mode, uint32_t bps, uint32_t *actual) {
	uint32_t per = ticks_per_bit(mode);
	uint32_t d;
	if (bps == 0) bps = 1;
	// nearest: (CLK / (per * bps)) rounded, minus one
	d = (CLK_HZ + (per * bps) / 2u) / (per * bps);
	if (d < 1u) d = 1u;
	if (d > 65536u) d = 65536u;
	d -= 1u;
	if (actual) *actual = z_gs_rate(mode, (uint16_t)d);
	return (uint16_t)d;
}

void z_gs_cs(int e, bool level) {
	uint32_t s;
	if (!mine(e)) return;
	// Only the CS bit changes, and MODE with it unchanged does not
	// reset the engine (rtl/gpio_stream.v resets on a MODE change).
	s = RD(ereg(e, Z_GS_REG_SCTL));
	s = level ? (s | Z_GS_CS) : (s & ~Z_GS_CS);
	WR(ereg(e, Z_GS_REG_SCTL), s);
}

// -- data --------------------------------------------------------

uint32_t z_gs_status(int e) {
	if (!engine_ok(e)) return 0;
	return RD(ereg(e, Z_GS_REG_SSTAT));
}

uint32_t z_gs_tx_free(int e) {
	if (!engine_ok(e)) return 0;
	return Z_GS_FIFO_DEPTH - Z_GS_ST_TXLEVEL(z_gs_status(e));
}

uint32_t z_gs_rx_level(int e) {
	if (!engine_ok(e)) return 0;
	return Z_GS_ST_RXLEVEL(z_gs_status(e)) + (pending[e] >= 0 ? 1u : 0u);
}

bool z_gs_busy(int e) {
	return (z_gs_status(e) & Z_GS_ST_BUSY) != 0;
}

bool z_gs_drain(int e, uint32_t max_polls) {
	while (max_polls--) {
		if (!z_gs_busy(e)) return true;
	}
	return !z_gs_busy(e);
}

uint32_t z_gs_write(int e, const uint8_t *buf, uint32_t n) {
	uint32_t free, done = 0;
	if (!mine(e)) return 0;
	free = z_gs_tx_free(e);
	if (n > free) n = free;
	// four at a time through STX4, byte 0 first
	while (n - done >= 4u) {
		WR(ereg(e, Z_GS_REG_STX4),
		   (uint32_t)buf[done] | ((uint32_t)buf[done + 1] << 8) |
		   ((uint32_t)buf[done + 2] << 16) | ((uint32_t)buf[done + 3] << 24));
		done += 4u;
	}
	while (done < n) {
		WR(ereg(e, Z_GS_REG_STX), buf[done]);
		done++;
	}
	return done;
}

bool z_gs_write_k(int e, uint8_t k) {
	if (!mine(e) || z_gs_tx_free(e) == 0) return false;
	WR(ereg(e, Z_GS_REG_STX), 0x100u | k);
	return true;
}

int z_gs_read_sym(int e) {
	uint32_t v;
	if (!mine(e)) return -1;
	if (pending[e] >= 0) {
		int p = pending[e];
		pending[e] = -1;
		return p;
	}
	v = RD(ereg(e, Z_GS_REG_SRX));
	if (!(v & 0x200u)) return -1;		// bit 9: valid
	return (int)(v & 0x1ffu);
}

uint32_t z_gs_read(int e, uint8_t *buf, uint32_t n) {
	uint32_t got = 0, level, sctl;
	bool zlink;
	if (!mine(e)) return 0;
	if (pending[e] >= 0) return 0;		// a K symbol is next

	sctl = RD(ereg(e, Z_GS_REG_SCTL));
	zlink = (sctl & 7u) == Z_GS_ZLINK;
	level = Z_GS_ST_RXLEVEL(z_gs_status(e));
	if (n > level) n = level;

	// Only zlink has control symbols, so only zlink has to look at each
	// entry; the other modes take four at a time.
	if (!zlink) {
		while (n - got >= 4u) {
			uint32_t w = RD(ereg(e, Z_GS_REG_SRX4));
			buf[got] = (uint8_t)w;
			buf[got + 1] = (uint8_t)(w >> 8);
			buf[got + 2] = (uint8_t)(w >> 16);
			buf[got + 3] = (uint8_t)(w >> 24);
			got += 4u;
		}
	}
	while (got < n) {
		uint32_t v = RD(ereg(e, Z_GS_REG_SRX));
		if (!(v & 0x200u)) break;
		if (v & 0x100u) {				// a control symbol: hold it
			pending[e] = (int16_t)(v & 0x1ffu);
			break;
		}
		buf[got++] = (uint8_t)v;
	}
	return got;
}

void z_gs_flush(int e, bool tx, bool rx) {
	if (!mine(e)) return;
	if (rx) pending[e] = -1;
	WR(ereg(e, Z_GS_REG_SFLUSH), (tx ? 1u : 0u) | (rx ? 2u : 0u));
}

uint32_t z_gs_errors(int e) {
	if (!engine_ok(e)) return 0;
	return RD(ereg(e, Z_GS_REG_SERR)) & 0xffffu;
}

void z_gs_clear_errors(int e) {
	if (!mine(e)) return;
	WR(ereg(e, Z_GS_REG_SFLUSH), 4u);
}

// -- SPI ---------------------------------------------------------

int z_gs_spi_xfer(int e, const uint8_t *tx, uint8_t *rx, uint32_t n) {
	uint32_t sent = 0, recvd = 0, idle = 0;
	uint8_t chunk[64], sink[64];

	uint32_t sctl;

	if (!mine(e)) return -1;
	sctl = RD(ereg(e, Z_GS_REG_SCTL));
	if ((sctl & 7u) != Z_GS_SPI) return -1;

	// Configured write-only (no Z_GS_RXEN): nothing will come back, so
	// send it all and wait for the last byte to leave.
	if (!(sctl & Z_GS_RXEN)) {
		while (sent < n) {
			uint32_t k;
			if (tx) k = z_gs_write(e, tx + sent, n - sent);
			else {
				uint32_t want = n - sent;
				if (want > sizeof chunk) want = sizeof chunk;
				for (uint32_t i = 0; i < want; i++) chunk[i] = 0xff;
				k = z_gs_write(e, chunk, want);
			}
			sent += k;
			if (k) idle = 0;
			else if (++idle > 1000000u) return (int)sent;
		}
		z_gs_drain(e, 1000000u);
		return (int)n;
	}

	// Keep the TX FIFO fed and the RX FIFO drained. The engine will not
	// start a byte without room to put the answer (rtl/gpio_stream.v),
	// so nothing is lost if this falls behind -- it just goes slower.
	while (recvd < n) {
		uint32_t progress = 0;
		if (sent < n) {
			uint32_t want = n - sent, k;
			if (want > sizeof chunk) want = sizeof chunk;
			if (tx) {
				k = z_gs_write(e, tx + sent, want);
			} else {
				for (uint32_t i = 0; i < want; i++) chunk[i] = 0xff;
				k = z_gs_write(e, chunk, want);
			}
			sent += k;
			progress += k;
		}
		{
			uint32_t want = n - recvd, k;
			if (want > sizeof sink) want = sizeof sink;
			k = z_gs_read(e, rx ? rx + recvd : sink, want);
			recvd += k;
			progress += k;
		}
		if (progress) idle = 0;
		else if (++idle > 1000000u) break;	// a stuck engine: give up
	}
	return (int)recvd;
}
