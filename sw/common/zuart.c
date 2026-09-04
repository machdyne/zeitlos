/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UART1 -- see sw/common/zuart.h for the interface and the design, and
 * docs/uart1.md for how to use it.
 *
 * The register map is already in sw/common/zeitlos.h (reg_uart1_*),
 * where sw/apps/net/esp32link.c has been using it for the ULX3S link
 * all along. This file does not redefine it.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zuart.h"

// 16550 LSR bits.
#define LSR_DR   0x01u		// data ready
#define LSR_OE   0x02u		// overrun
#define LSR_PE   0x04u		// parity
#define LSR_FE   0x08u		// framing
#define LSR_BI   0x10u		// break
#define LSR_THRE 0x20u		// transmit holding register empty

// FCR: enable the FIFOs and clear both. Written once at open; the
// FIFOs are the only reason a polled reader is viable at all.
#define FCR_ENABLE 0x07u

// Accumulated LSR error bits -- see z_uart1_status() on why this is
// kept here rather than read from the hardware on demand.
static uint32_t err_sticky;

// Every read of the data register passes through here, so the sticky
// bits pick up an overrun that happened between two reads rather than
// losing it to the LSR's read-to-clear behaviour.
static uint8_t lsr_poll(void) {
	uint8_t s = reg_uart1_lsr;
	if (s & LSR_OE) err_sticky |= Z_UART1_OVERRUN;
	if (s & LSR_PE) err_sticky |= Z_UART1_PARITY;
	if (s & LSR_FE) err_sticky |= Z_UART1_FRAMING;
	if (s & LSR_BI) err_sticky |= Z_UART1_BREAK;
	return s;
}

bool z_uart1_present(void) {
	// The feature bit is the only answer available. See zuart.h: the
	// 16550 has no identity register, and on a board without one this
	// address falls through to whatever the bus resolves to.
	return z_soc_has_feature2(Z_FEATURE2_UART1);
}

// The 16550 divides sys_clk by 16 * n. Rounded rather than truncated,
// which matters at the top of the range: truncating 921600 at 48MHz
// gives n=3 (1 Mbaud, 8.5% fast) where rounding also gives 3 -- but
// truncating 460800 gives 6 (500k, 8.5% fast) where rounding gives 7
// (428.6k, 7% slow), and neither is usable. That is what
// z_uart1_baud_error() exists to report instead of pretending.
static uint32_t baud_div(uint32_t baud) {
	uint32_t d;
	if (!baud) return 0;
	d = (Z_SYSCLK_HZ + (baud * 8u)) / (baud * 16u);
	if (d < 1) d = 1;
	if (d > 0xffffu) d = 0xffffu;
	return d;
}

uint32_t z_uart1_baud_error(uint32_t baud) {

	uint32_t d = baud_div(baud);
	uint32_t actual;

	if (!d) return 10000;

	actual = Z_SYSCLK_HZ / (16u * d);

	// Percent times 100, computed without overflowing 32 bits: the
	// difference is small, so scaling it up first is safe where
	// scaling `actual` would not be.
	if (actual > baud) return ((actual - baud) * 10000u) / baud;
	return ((baud - actual) * 10000u) / baud;

}

bool z_uart1_config(uint32_t baud, uint8_t bits, char parity, uint8_t stop) {

	uint32_t d;
	uint8_t lcr;

	if (!z_uart1_present()) return false;

	if (bits < 5 || bits > 8) return false;
	if (stop != 1 && stop != 2) return false;
	if (parity != 'n' && parity != 'e' && parity != 'o') return false;

	// 3% is roughly where a UART stops working: the receiver samples
	// mid-bit and accumulates the error over ten bit times, so a few
	// percent walks the sample point off the end of the byte. Refusing
	// is better than producing a port that transmits garbage and looks
	// like a wiring fault -- see zuart.h.
	if (z_uart1_baud_error(baud) > 300) return false;

	d = baud_div(baud);
	if (!d) return false;

	lcr = (uint8_t)(bits - 5);
	if (stop == 2) lcr |= 0x04u;
	if (parity == 'e') lcr |= 0x18u;
	else if (parity == 'o') lcr |= 0x08u;

	// Interrupts stay off. Nothing handles cpu_irq[9] -- see zuart.h
	// on why that is deliberate -- and enabling them here would leave
	// the 16550 asserting a level nobody lowers.
	reg_uart1_ier = 0x00;

	// DLAB set to reach the divisor latches, then cleared. The order
	// matters: writing the divisor with DLAB clear writes the transmit
	// register instead, which sends a byte nobody asked for.
	reg_uart1_lcr = (uint8_t)(lcr | 0x80u);
	reg_uart1_dlbl = (uint8_t)(d & 0xffu);
	reg_uart1_dlbh = (uint8_t)((d >> 8) & 0xffu);
	reg_uart1_lcr = lcr;

	reg_uart1_fcr = FCR_ENABLE;

	err_sticky = 0;
	z_uart1_flush_rx();

	return true;

}

bool z_uart1_open(uint32_t baud) {
	return z_uart1_config(baud, 8, 'n', 1);
}

void z_uart1_close(void) {
	if (!z_uart1_present()) return;
	reg_uart1_ier = 0x00;
	// The divisor is left alone on purpose: reopening at the same rate
	// then costs nothing, and there is nothing to release.
}

bool z_uart1_tx_ready(void) {
	if (!z_uart1_present()) return false;
	return (reg_uart1_lsr & LSR_THRE) != 0;
}

void z_uart1_putc(char c) {
	if (!z_uart1_present()) return;
	while ((reg_uart1_lsr & LSR_THRE) == 0)
		;
	reg_uart1_data = (uint8_t)c;
}

uint32_t z_uart1_write(const void *buf, uint32_t n) {

	const uint8_t *p = buf;
	uint32_t i;

	if (!z_uart1_present() || !p) return 0;

	for (i = 0; i < n; i++) z_uart1_putc((char)p[i]);

	return n;

}

uint32_t z_uart1_write_nb(const void *buf, uint32_t n) {

	const uint8_t *p = buf;
	uint32_t i = 0;

	if (!z_uart1_present() || !p) return 0;

	// THRE means the holding register is empty, not that the FIFO is.
	// This core does not expose a fill level, so there is no way to
	// know how many more would fit -- one byte per THRE is the only
	// answer that cannot overrun the transmitter.
	//
	// That makes this slower than it looks: a caller draining a large
	// buffer gets a byte or so per call and has to come back. That is
	// the right shape for a port server, which is coming back anyway,
	// and it is why z_uart1_write() still exists for callers who
	// genuinely want to block.
	while (i < n && (reg_uart1_lsr & LSR_THRE)) {
		reg_uart1_data = p[i];
		i++;
	}

	return i;

}

bool z_uart1_rx_ready(void) {
	if (!z_uart1_present()) return false;
	return (lsr_poll() & LSR_DR) != 0;
}

int z_uart1_getc(void) {
	if (!z_uart1_present()) return -1;
	if (!(lsr_poll() & LSR_DR)) return -1;
	return (int)reg_uart1_data;
}

uint32_t z_uart1_read(void *buf, uint32_t n) {

	uint8_t *p = buf;
	uint32_t i = 0;

	if (!z_uart1_present() || !p) return 0;

	while (i < n && (lsr_poll() & LSR_DR)) {
		p[i] = reg_uart1_data;
		i++;
	}

	return i;

}

void z_uart1_flush_rx(void) {
	if (!z_uart1_present()) return;
	// Bounded rather than "until empty": at speed, a live sender can
	// refill the FIFO as fast as this drains it, and an unbounded loop
	// would never return. 64 is four times the FIFO depth, which is
	// enough to clear anything that was already there.
	{
		int i;
		for (i = 0; i < 64 && (reg_uart1_lsr & LSR_DR); i++)
			(void)reg_uart1_data;
	}
	err_sticky = 0;
}

uint32_t z_uart1_status(void) {
	uint32_t s;
	if (!z_uart1_present()) return 0;
	lsr_poll();
	s = err_sticky;
	err_sticky = 0;
	return s;
}
