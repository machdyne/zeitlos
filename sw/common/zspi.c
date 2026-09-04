/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Bit-banged SPI master -- see sw/common/zspi.h for the interface and
 * the design, and docs/spi.md for how to use it.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zspi.h"
#include "zsoc.h"		// Z_SYSCLK_HZ
#include "zcycles.h"	// z_cycles() -- see there on the host branch

// Busy-wait. Unsigned subtraction, so a counter wrap during the wait
// is handled by the arithmetic rather than by a special case.
//
// Being preempted in here makes the wait LONGER, never shorter, which
// is the property that makes bit-banging safe under a preemptive
// scheduler at all -- every interval in these protocols is a minimum.
// See this library's header for the one exception (SMBus timeouts).
static void zspi_delay(uint32_t cycles) {
	uint32_t t0;
	if (!cycles) return;
	t0 = z_cycles();
	while ((z_cycles() - t0) < cycles)
		;
}

static bool has_pin(uint8_t port) {
	return port != Z_SPI_NO_PIN;
}

// CPOL is bit 1 of the mode; CPHA is bit 0.
static bool cpol(const z_spi_t *s) { return (s->mode & 2) != 0; }
static bool cpha(const z_spi_t *s) { return (s->mode & 1) != 0; }

bool z_spi_init(z_spi_t *s) {

	if (s->mode > 3) return false;

	if (s->khz == 0) s->half_cycles = 0;
	else s->half_cycles = Z_SYSCLK_HZ / (s->khz * 1000u) / 2u;

	// Push-pull outputs, not open drain: every line here has exactly
	// one driver. See zspi.h.
	z_gpio_mode(s->sck_port, s->sck_pin, Z_GPIO_OUT);
	z_gpio_mode(s->mosi_port, s->mosi_pin, Z_GPIO_OUT);

	// SCLK parked at its idle level for the mode. Getting this wrong
	// is not subtle -- the first clock edge of the first transfer is
	// then spurious and everything is off by a bit forever.
	z_gpio_write(s->sck_port, s->sck_pin, cpol(s));
	z_gpio_write(s->mosi_port, s->mosi_pin, false);

	if (has_pin(s->miso_port))
		z_gpio_mode(s->miso_port, s->miso_pin, Z_GPIO_IN);

	if (has_pin(s->cs_port)) {
		// Deasserted BEFORE it becomes an output, so configuring the
		// bus never produces a momentary select. The order matters:
		// the pin is an input until z_gpio_mode() runs, so staging
		// the value first means it is already correct at the instant
		// it starts driving. (z_gpio_write() on an input stages OUT
		// without driving -- see sw/common/zgpio.h.)
		z_gpio_write(s->cs_port, s->cs_pin, !s->cs_active_high);
		z_gpio_mode(s->cs_port, s->cs_pin, Z_GPIO_OUT);
	}

	return true;

}

void z_spi_select(z_spi_t *s, bool on) {

	if (!has_pin(s->cs_port)) return;

	z_gpio_write(s->cs_port, s->cs_pin, s->cs_active_high ? on : !on);

	// Devices specify a setup time between CS and the first clock
	// edge. Half a bit period is more than any of them ask for and
	// costs nothing at these rates.
	zspi_delay(s->half_cycles);

}

uint8_t z_spi_xfer8(z_spi_t *s, uint8_t out) {

	uint8_t in = 0;
	int i;

	for (i = 0; i < 8; i++) {

		bool bit = s->lsb_first
			? ((out >> i) & 1)
			: ((out >> (7 - i)) & 1);
		bool got;

		if (!cpha(s)) {

			// CPHA=0: data is presented BEFORE the leading edge and
			// sampled ON it. So MOSI is set while the clock is idle,
			// then the clock goes to its active level and both ends
			// sample.
			z_gpio_write(s->mosi_port, s->mosi_pin, bit);
			zspi_delay(s->half_cycles);

			z_gpio_write(s->sck_port, s->sck_pin, !cpol(s));
			got = has_pin(s->miso_port)
				? z_gpio_read(s->miso_port, s->miso_pin) : true;
			zspi_delay(s->half_cycles);

			z_gpio_write(s->sck_port, s->sck_pin, cpol(s));

		} else {

			// CPHA=1: the leading edge is a "get ready" edge and data
			// is presented on it; sampling happens on the trailing
			// edge.
			z_gpio_write(s->sck_port, s->sck_pin, !cpol(s));
			z_gpio_write(s->mosi_port, s->mosi_pin, bit);
			zspi_delay(s->half_cycles);

			z_gpio_write(s->sck_port, s->sck_pin, cpol(s));
			got = has_pin(s->miso_port)
				? z_gpio_read(s->miso_port, s->miso_pin) : true;
			zspi_delay(s->half_cycles);

		}

		if (got) {
			in = (uint8_t)(in | (s->lsb_first ? (1u << i) : (1u << (7 - i))));
		}

	}

	return in;

}

void z_spi_xfer(z_spi_t *s, const uint8_t *tx, uint8_t *rx, uint32_t n) {

	uint32_t i;

	for (i = 0; i < n; i++) {
		// Read tx[i] BEFORE writing rx[i], so tx and rx may be the
		// same buffer -- which is how a command-and-response in place
		// is written, and the obvious thing for a caller to try.
		uint8_t out = tx ? tx[i] : Z_SPI_TX_IDLE;
		uint8_t in = z_spi_xfer8(s, out);
		if (rx) rx[i] = in;
	}

}
