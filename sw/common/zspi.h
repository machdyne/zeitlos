#ifndef ZSPI_H
#define ZSPI_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI master, bit-banged over GPIO pins (sw/common/zgpio.h,
 * docs/gpio.md). All four clock modes, either bit order, optional
 * chip select, optional MISO.
 *
 * -- This is not the SD card's SPI, and not the ethernet's --
 *
 * Those go through rtl/spisd.v and rtl/spim.v, which generate SCLK in
 * gateware. That is deliberate and rtl/spisd.v's own header explains
 * why: the SD card was bit-banged once, which made the SPI clock rate
 * a function of compiler codegen, and it broke when the toolchain
 * changed. Anything that needs a guaranteed clock rate or real
 * throughput belongs in gateware.
 *
 * THIS IS FOR THE OTHER CASE: a display, a sensor, a shift register, a
 * flash chip you are poking at once -- devices where a few tens of
 * kilobits per second is plenty and having the pins be software is
 * worth more than having them be fast. If you find yourself wanting
 * megabits, the answer is a peripheral, not a tighter loop here.
 *
 * -- Push-pull, unlike I2C --
 *
 * SPI lines have exactly one driver each, so SCLK, MOSI and CS are
 * ordinary outputs (Z_GPIO_OUT) rather than the open-drain arrangement
 * sw/common/zi2c.h uses. The weak internal pull-ups (docs/gpio.md) are
 * irrelevant to a driven pin; they matter only to MISO, where they
 * mean an absent or unselected device reads as 0xff rather than as
 * noise. That is a useful default -- 0xff from a device that should
 * have answered is a recognisable "nothing there" rather than a
 * plausible-looking value.
 *
 * -- Clock modes --
 *
 *   mode  CPOL  CPHA  idle SCLK   sampled on
 *   0     0     0     low         rising edge     (the common one)
 *   1     0     1     low         falling edge
 *   2     1     0     high        falling edge
 *   3     1     1     high        rising edge
 *
 * CPHA is the one people get wrong, and the symptom is characteristic:
 * every byte comes back shifted by one bit, so 0x01 reads as 0x02 or
 * 0x80. If a device is answering with something that looks like your
 * data doubled, try the other CPHA before suspecting the wiring.
 *
 * -- Preemption --
 *
 * A KTIMER tick can land between any two edges here, stretching the
 * clock low (or high) period. SPI has no minimum frequency and no
 * timeout in the protocol, so for almost every device this is
 * invisible -- the same property that makes bit-banged I2C safe (see
 * sw/common/zi2c.h).
 *
 * The exceptions are devices that are not really SPI: anything with a
 * self-clocking or timing-encoded protocol driven through a shift
 * register (addressable LEDs being the usual example) will glitch when
 * the clock stalls for a millisecond. Those need maskirq()
 * (sw/common/zeitlos.h) around the transfer, or gateware. This library
 * deliberately does not mask, because a long transfer would then block
 * the whole system.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgpio.h"

// For miso_port/cs_port: this device has no such pin.
//
// A write-only display needs no MISO; a device selected by something
// else (or the only one on the bus, tied low) needs no CS. Rather than
// a pair of has_* flags, the pin field itself says so -- one thing to
// set, and it cannot disagree with itself.
#define Z_SPI_NO_PIN 0xffu

typedef struct {

	// -- caller fills these in before z_spi_init() --

	uint8_t sck_port, sck_pin;
	uint8_t mosi_port, mosi_pin;
	uint8_t miso_port, miso_pin;	// port may be Z_SPI_NO_PIN
	uint8_t cs_port, cs_pin;		// port may be Z_SPI_NO_PIN

	uint8_t mode;					// 0-3, see the table above
	bool lsb_first;					// default (false) is MSB first
	bool cs_active_high;			// default (false) is the usual /CS

	// Requested bit rate. 0 means "no delay loop", which lands
	// somewhere around 100-200kHz -- set by the cost of the wishbone
	// transactions, which is a floor this cannot go below whatever
	// you ask for. Ask for less than that and you get less.
	uint32_t khz;

	// -- derived by z_spi_init(); do not set --

	uint32_t half_cycles;

} z_spi_t;

// Configure the pins and leave the bus idle: SCLK at its mode's idle
// level, CS deasserted, MOSI low, MISO an input.
//
// Returns false only for a mode outside 0-3. There is nothing else to
// fail: unlike I2C there is no bus state to inspect, because every
// line here has exactly one driver.
//
// Safe to call repeatedly, and worth calling again if another process
// might have used the same pins -- it restores every one of them.
bool z_spi_init(z_spi_t *s);

// Assert or deassert CS, honouring cs_active_high. A no-op if this
// device has no CS pin.
//
// Separate from the transfer calls on purpose: almost every real SPI
// device wants several transfers inside one selection (a command byte,
// then an address, then a burst of data), and a select-per-transfer
// API would make that impossible to express.
void z_spi_select(z_spi_t *s, bool on);

// One byte out, one byte in, simultaneously -- which is what SPI is.
// Returns 0xff on a bus with no MISO pin configured, matching what an
// unconnected input reads as.
uint8_t z_spi_xfer8(z_spi_t *s, uint8_t out);

// n bytes. Either buffer may be NULL: a NULL `tx` sends
// Z_SPI_TX_IDLE, and a NULL `rx` discards what comes back.
//
// tx and rx may be the SAME buffer, which is the usual way to do a
// command-and-response in place.
void z_spi_xfer(z_spi_t *s, const uint8_t *tx, uint8_t *rx, uint32_t n);

// What z_spi_xfer() sends when `tx` is NULL.
//
// 0xff, not 0x00, because that is what the conventions of the devices
// this will talk to expect: SD cards require it, and on most flash and
// sensor parts 0x00 is a meaningful command byte where 0xff is not.
// Sending zeros while reading is a real way to erase something.
#define Z_SPI_TX_IDLE 0xffu

#endif
