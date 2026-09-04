/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Bit-banged I2C master -- see sw/common/zi2c.h for the interface and
 * the design, and docs/i2c.md for how to use it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>	// NULL -- see z_i2c_probe()

#include "zi2c.h"
#include "zsoc.h"		// Z_SYSCLK_HZ
#include "zcycles.h"	// z_cycles() -- see there on the host branch

// Busy-wait. Unsigned subtraction, so a counter wrap during the wait
// is handled by the arithmetic rather than by a special case.
//
// Being preempted in here makes the wait LONGER, never shorter, which
// is the property that makes bit-banging safe under a preemptive
// scheduler at all -- every interval in these protocols is a minimum.
// See this library's header for the one exception (SMBus timeouts).
static void zi2c_delay(uint32_t cycles) {
	uint32_t t0;
	if (!cycles) return;
	t0 = z_cycles();
	while ((z_cycles() - t0) < cycles)
		;
}

// -- the two lines --
//
// Driving low is DIRSET, releasing is DIRCLR, reading is the pin. The
// OUT bit is parked at 0 by z_i2c_init() and never touched again; see
// zi2c.h on why that makes driving a line high structurally
// impossible here.

static void scl_low(z_i2c_t *b) {
	z_gpio_od_write(b->scl_port, b->scl_pin, false);
}

static void sda_low(z_i2c_t *b) {
	z_gpio_od_write(b->sda_port, b->sda_pin, false);
}

static void sda_release(z_i2c_t *b) {
	z_gpio_od_write(b->sda_port, b->sda_pin, true);
}

static bool sda_read(z_i2c_t *b) {
	return z_gpio_read(b->sda_port, b->sda_pin);
}

// Release SCL and WAIT FOR IT TO ACTUALLY BE HIGH.
//
// This one function is doing two jobs that happen to be the same job,
// and that is the reason the weak internal pull-ups are usable at all:
//
//   - Clock stretching. A slave that needs more time holds SCL down,
//     and the master is required to wait. Not optional.
//   - RC rise time. A released line does not go high instantly, and
//     with tens of kOhm of internal pull-up it can take microseconds.
//
// Code that assumed SCL was high the instant it was released would be
// wrong about both. Waiting for the pin to read back is right about
// both, and costs nothing when the line is fast.
static bool scl_release_wait(z_i2c_t *b) {

	uint32_t t0;

	z_gpio_od_write(b->scl_port, b->scl_pin, true);

	if (z_gpio_read(b->scl_port, b->scl_pin)) return true;

	t0 = z_cycles();
	while (!z_gpio_read(b->scl_port, b->scl_pin)) {
		if ((z_cycles() - t0) > b->timeout_cycles) return false;
	}

	return true;

}

// The same for SDA, used before every rising SCL edge on which a 1 is
// being presented. Without it a slow rise clocks out a 0 -- silently,
// and only on the bits that happen to follow a 0, which is the kind of
// bug that looks like a flaky device.
static bool sda_release_wait(z_i2c_t *b) {

	uint32_t t0;

	sda_release(b);

	if (sda_read(b)) return true;

	t0 = z_cycles();
	while (!sda_read(b)) {
		if ((z_cycles() - t0) > b->timeout_cycles) return false;
	}

	return true;

}

// -- bus phases --

static z_i2c_rv i2c_start(z_i2c_t *b) {

	// Works as both a START and a REPEATED START: both lines are
	// brought high first, so a caller that is mid-transfer with SCL
	// low gets the extra clock edge it needs, and a caller on an idle
	// bus finds both lines already high and pays two reads for it.
	if (!sda_release_wait(b)) return Z_I2C_TIMEOUT;
	if (!scl_release_wait(b)) return Z_I2C_TIMEOUT;
	zi2c_delay(b->half_cycles);

	sda_low(b);
	zi2c_delay(b->half_cycles);
	scl_low(b);
	zi2c_delay(b->half_cycles);

	return Z_I2C_OK;

}

static z_i2c_rv i2c_stop(z_i2c_t *b) {

	sda_low(b);
	zi2c_delay(b->half_cycles);

	if (!scl_release_wait(b)) return Z_I2C_TIMEOUT;
	zi2c_delay(b->half_cycles);

	// SDA rising while SCL is high: the STOP condition. The wait
	// matters here too -- a STOP that does not complete leaves the
	// bus held and the next START will not be seen.
	if (!sda_release_wait(b)) return Z_I2C_TIMEOUT;
	zi2c_delay(b->half_cycles);

	return Z_I2C_OK;

}

// One bit out: present it on SDA while SCL is low, then pulse SCL.
static z_i2c_rv i2c_write_bit(z_i2c_t *b, bool bit) {

	if (bit) {
		if (!sda_release_wait(b)) return Z_I2C_TIMEOUT;
	} else {
		sda_low(b);
	}

	zi2c_delay(b->half_cycles);

	if (!scl_release_wait(b)) return Z_I2C_TIMEOUT;
	zi2c_delay(b->half_cycles);

	scl_low(b);

	return Z_I2C_OK;

}

static z_i2c_rv i2c_read_bit(z_i2c_t *b, bool *bit) {

	// SDA released so the slave can drive it. No wait needed on this
	// one: if the slave is pulling it down, waiting for it to rise
	// would time out on a perfectly good bus. The read below happens
	// after a full half period, which is the rise time budget.
	sda_release(b);
	zi2c_delay(b->half_cycles);

	if (!scl_release_wait(b)) return Z_I2C_TIMEOUT;

	// Sampled while SCL is high, which is where I2C says the data is
	// valid, and as late in the high period as possible so a slow
	// rise has had the whole half period to finish.
	zi2c_delay(b->half_cycles);
	*bit = sda_read(b);

	scl_low(b);

	return Z_I2C_OK;

}

static z_i2c_rv i2c_write_byte(z_i2c_t *b, uint8_t v, bool *acked) {

	z_i2c_rv rv;
	bool nack;
	int i;

	for (i = 7; i >= 0; i--) {
		rv = i2c_write_bit(b, (v >> i) & 1);
		if (rv != Z_I2C_OK) return rv;
	}

	b->last_bits += 9;

	// The ninth bit is the slave's. A LOW is the acknowledgement --
	// I2C's ack is an active pull-down, so "nobody there" and "device
	// says no" are the same thing on the wire, which is why
	// z_i2c_probe() can be built out of exactly this.
	rv = i2c_read_bit(b, &nack);
	if (rv != Z_I2C_OK) return rv;

	if (acked) *acked = !nack;

	return Z_I2C_OK;

}

static z_i2c_rv i2c_read_byte(z_i2c_t *b, uint8_t *v, bool ack) {

	z_i2c_rv rv;
	bool bit;
	uint8_t out = 0;
	int i;

	for (i = 0; i < 8; i++) {
		rv = i2c_read_bit(b, &bit);
		if (rv != Z_I2C_OK) return rv;
		out = (uint8_t)((out << 1) | (bit ? 1 : 0));
	}

	b->last_bits += 9;

	rv = i2c_write_bit(b, !ack);
	if (rv != Z_I2C_OK) return rv;

	*v = out;

	return Z_I2C_OK;

}

// -- setup --

z_i2c_rv z_i2c_init(z_i2c_t *b) {

	// Open-drain mode parks OUT at 0 and leaves the pin floating,
	// which is the idle state of both lines. After this, nothing in
	// this file ever writes OUT again.
	z_gpio_mode(b->scl_port, b->scl_pin, Z_GPIO_OD);
	z_gpio_mode(b->sda_port, b->sda_pin, Z_GPIO_OD);

	if (b->timeout_us == 0) b->timeout_us = 1000;
	b->timeout_cycles = (Z_SYSCLK_HZ / 1000000u) * b->timeout_us;

	if (b->khz == 0) {
		// "as fast as the bus goes" -- no delay at all. The rate is
		// then set by the wishbone transactions, which is a real
		// floor this cannot go below anyway.
		b->half_cycles = 0;
	} else {
		b->half_cycles = Z_SYSCLK_HZ / (b->khz * 1000u) / 2u;
	}

	b->last_bits = 0;
	b->last_cycles = 0;

	// Both lines should be high on an idle bus. If they are not,
	// nothing this code does will work, and saying so here is much
	// kinder than a mysterious NACK on the first transfer.
	if (!z_gpio_read(b->scl_port, b->scl_pin)
		|| !z_gpio_read(b->sda_port, b->sda_pin))
		return Z_I2C_BUSY;

	return Z_I2C_OK;

}

z_i2c_rv z_i2c_recover(z_i2c_t *b) {

	int i;

	z_gpio_mode(b->scl_port, b->scl_pin, Z_GPIO_OD);
	z_gpio_mode(b->sda_port, b->sda_pin, Z_GPIO_OD);

	sda_release(b);

	// Nine clocks, because a slave can be at most eight bits plus an
	// ack into a byte it thinks it is transmitting. Walking it to the
	// end is the only way to make it let go of SDA -- it is not
	// listening for anything else.
	for (i = 0; i < 9 && !sda_read(b); i++) {
		scl_low(b);
		zi2c_delay(b->half_cycles ? b->half_cycles : 100);
		if (!scl_release_wait(b)) return Z_I2C_TIMEOUT;
		zi2c_delay(b->half_cycles ? b->half_cycles : 100);
	}

	if (!sda_read(b)) return Z_I2C_TIMEOUT;

	// A STOP to put any slave that was mid-transaction back into its
	// idle state, rather than leaving it half-addressed.
	i2c_stop(b);

	return Z_I2C_OK;

}

// -- transfers --

// Everything below funnels through this so the measurement in
// z_i2c_measured_khz() covers whole transfers rather than each one
// starting the clock again.
static void xfer_begin(z_i2c_t *b) {
	b->last_bits = 0;
	b->last_cycles = z_cycles();
}

static void xfer_end(z_i2c_t *b) {
	b->last_cycles = z_cycles() - b->last_cycles;
}

z_i2c_rv z_i2c_write(z_i2c_t *b, uint8_t addr,
	const uint8_t *data, uint32_t n, bool stop) {

	z_i2c_rv rv;
	bool acked = false;
	uint32_t i;

	xfer_begin(b);

	rv = i2c_start(b);
	if (rv != Z_I2C_OK) goto out;

	// 7-bit address, shifted up, R/W bit clear. See zi2c.h on why
	// this library takes 7-bit addresses everywhere.
	rv = i2c_write_byte(b, (uint8_t)(addr << 1), &acked);
	if (rv != Z_I2C_OK) goto out;
	if (!acked) { rv = Z_I2C_NACK; goto out_stop; }

	for (i = 0; i < n; i++) {
		rv = i2c_write_byte(b, data[i], &acked);
		if (rv != Z_I2C_OK) goto out;
		if (!acked) { rv = Z_I2C_NACK; goto out_stop; }
	}

out_stop:
	// A STOP even on NACK, and deliberately: the alternative is
	// leaving the bus held by a failed transfer, which makes the NEXT
	// caller fail too and turns one missing device into a dead bus.
	if (stop || rv == Z_I2C_NACK) {
		z_i2c_rv srv = i2c_stop(b);
		if (rv == Z_I2C_OK) rv = srv;
	}

out:
	xfer_end(b);
	return rv;

}

z_i2c_rv z_i2c_read(z_i2c_t *b, uint8_t addr,
	uint8_t *data, uint32_t n, bool stop) {

	z_i2c_rv rv;
	bool acked = false;
	uint32_t i;

	if (n == 0) return Z_I2C_OK;

	xfer_begin(b);

	rv = i2c_start(b);
	if (rv != Z_I2C_OK) goto out;

	rv = i2c_write_byte(b, (uint8_t)((addr << 1) | 1), &acked);
	if (rv != Z_I2C_OK) goto out;
	if (!acked) { rv = Z_I2C_NACK; goto out_stop; }

	for (i = 0; i < n; i++) {
		// ACK every byte except the last. The final NACK is how the
		// slave is told to stop driving SDA; without it the bus is
		// left with two devices' idea of who owns the line and the
		// STOP that follows is not seen.
		rv = i2c_read_byte(b, &data[i], i + 1 < n);
		if (rv != Z_I2C_OK) goto out;
	}

out_stop:
	if (stop || rv == Z_I2C_NACK) {
		z_i2c_rv srv = i2c_stop(b);
		if (rv == Z_I2C_OK) rv = srv;
	}

out:
	xfer_end(b);
	return rv;

}

z_i2c_rv z_i2c_write_read(z_i2c_t *b, uint8_t addr,
	const uint8_t *w, uint32_t wn, uint8_t *r, uint32_t rn) {

	z_i2c_rv rv;

	// No STOP between the two halves -- see zi2c.h on why a
	// write-STOP-read pair is a different and often wrong thing.
	rv = z_i2c_write(b, addr, w, wn, false);
	if (rv != Z_I2C_OK) return rv;

	return z_i2c_read(b, addr, r, rn, true);

}

z_i2c_rv z_i2c_reg_read8(z_i2c_t *b, uint8_t addr, uint8_t reg,
	uint8_t *val) {
	return z_i2c_write_read(b, addr, &reg, 1, val, 1);
}

z_i2c_rv z_i2c_reg_write8(z_i2c_t *b, uint8_t addr, uint8_t reg,
	uint8_t val) {
	uint8_t buf[2];
	buf[0] = reg;
	buf[1] = val;
	return z_i2c_write(b, addr, buf, 2, true);
}

bool z_i2c_probe(z_i2c_t *b, uint8_t addr) {
	return z_i2c_write(b, addr, NULL, 0, true) == Z_I2C_OK;
}

int z_i2c_scan(z_i2c_t *b, uint8_t *found, int max) {

	int n = 0;
	uint8_t a;

	// 0x00-0x07 and 0x78-0x7f are reserved by the specification --
	// general call, 10-bit addressing, and so on. Probing them is not
	// harmless: a general-call write can reset every device on the
	// bus, which is a memorable way to debug a scan.
	for (a = 0x08; a <= 0x77; a++) {
		if (!z_i2c_probe(b, a)) continue;
		if (n < max) found[n] = a;
		n++;
	}

	return n;

}

uint32_t z_i2c_measured_khz(const z_i2c_t *b) {

	if (!b->last_bits || !b->last_cycles) return 0;

	// bits per second = bits * SYSCLK / cycles; divide by 1000 for
	// kHz. Ordered to keep the intermediate inside 32 bits: a
	// transfer is at most a few hundred bits and cycles is large, so
	// (SYSCLK/1000) first is safe where (bits * SYSCLK) would not be.
	return (b->last_bits * (Z_SYSCLK_HZ / 1000u)) / b->last_cycles;

}

const char *z_i2c_strerror(z_i2c_rv rv) {
	switch (rv) {
	case Z_I2C_OK:      return "ok";
	case Z_I2C_NACK:    return "nack";
	case Z_I2C_TIMEOUT: return "timeout";
	case Z_I2C_BUSY:    return "busy";
	default:            return "unknown";
	}
}
