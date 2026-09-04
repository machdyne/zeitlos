#ifndef ZI2C_H
#define ZI2C_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * I2C master, bit-banged over any two GPIO pins (sw/common/zgpio.h,
 * docs/gpio.md). There is no I2C hardware on this SOC and this is why
 * there does not need to be.
 *
 * -- Open drain, done in the fabric --
 *
 * Every line here is driven the way I2C requires and no other way: the
 * pin's OUT bit is parked at 0 once, and after that a 0 on the bus is
 * "make the pin an output" (DIRSET) and a 1 is "make it an input"
 * (DIRCLR) and let the pull-up take it high. One store per edge.
 *
 * The safety property is structural rather than careful: the register
 * that would have to hold a 1 to drive a line HIGH is never written to
 * anything but 0, so this code physically cannot drive into a line
 * another device is pulling down. That is the failure that ends with a
 * hot part rather than a wrong reading, and it is worth removing by
 * construction.
 *
 * -- Pull-ups --
 *
 * Every GPIO pin has a weak internal pull-up (release/hw/pmods/
 * gpio.spec sets PULLMODE=UP; docs/gpio.md explains why). Tens of
 * kOhm, which is far too weak to meet the 1us rise time 100kHz I2C
 * specifies -- and it does not matter here, because this master picks
 * its own clock and a microsecond or two of RC rise is invisible
 * against a 20us bit period.
 *
 * WHAT MAKES THAT SAFE RATHER THAN LUCKY is that this file never
 * assumes a released line is immediately high. After releasing SCL it
 * polls until SCL actually reads high -- which is also exactly how
 * clock stretching works, so it is code that has to exist anyway --
 * and it does the same on SDA before every rising SCL edge. A slow
 * pull-up therefore costs BIT RATE, not correctness: the bus runs
 * slower than requested and every byte is still right.
 *
 * Bring your own 2.2k-4.7k for anything past a short cable or a couple
 * of devices. z_i2c_measured_khz() below reports what the bus is
 * actually managing, which is how you find out you need them.
 *
 * -- This runs under a preemptive scheduler, and that is fine --
 *
 * A KTIMER tick can land anywhere in a transfer, including between
 * setting SDA and raising SCL. It cannot corrupt anything: every
 * interval in I2C is a MINIMUM, so being descheduled makes the clock
 * low period longer, which is precisely what a slave does deliberately
 * when it stretches. There is no maximum this code can violate by
 * being slow.
 *
 * WITH ONE EXCEPTION WORTH KNOWING: SMBus devices (and a few I2C parts
 * that borrow its rules) time out and reset their state machine if SCL
 * is held low for 25-35ms. A slice here is 1.365ms and a process can be
 * off the CPU for several in a row, so a badly timed preemption in the
 * middle of a byte can make an SMBus part abandon the transfer. The
 * symptom is an occasional NACK or a garbage read that goes away on
 * retry. If that matters, mask interrupts around the transfer
 * (maskirq(), sw/common/zeitlos.h) -- this library deliberately does
 * not, because a multi-byte transfer at 50kHz is milliseconds long and
 * masking that is worse for the system than an occasional retry is for
 * the caller.
 *
 * -- Error reporting --
 *
 * NACK and TIMEOUT are separate return values and the distinction is
 * the whole point of having an enum here rather than a bool: a NACK
 * means the bus works and nobody answered (which is what a scan is
 * made of), and a TIMEOUT means a line never came up, which is a
 * wiring or pull-up fault and no amount of retrying will fix it.
 * Folding them together would make a missing pull-up look like an
 * absent device.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgpio.h"

typedef enum {
	Z_I2C_OK = 0,

	// The addressed device did not pull SDA down during the ack bit.
	// Almost always "nothing at that address"; also what a device
	// answers when it is busy with the last thing you asked it.
	Z_I2C_NACK,

	// A line this code RELEASED never read back high within the
	// timeout. Not a device problem: nothing on an I2C bus can hold a
	// line high, so this means no pull-up, a short, or a device stuck
	// mid-byte holding SDA down. Try z_i2c_recover() for the last of
	// those.
	Z_I2C_TIMEOUT,

	// The bus was already unusable when the transfer started -- SDA
	// or SCL low with nothing driving them. Reported separately from
	// TIMEOUT because it is detected before anything is sent, so
	// nothing was half-transmitted and a retry after
	// z_i2c_recover() is safe.
	Z_I2C_BUSY
} z_i2c_rv;

typedef struct {

	// -- caller fills these in before z_i2c_init() --

	uint8_t scl_port, scl_pin;
	uint8_t sda_port, sda_pin;

	// Requested bit rate. Clamped to something this machine can
	// actually produce -- see z_i2c_init(). 100 is the sensible
	// default; there is no reason to go faster on a bit-banged bus
	// and every reason not to on a bus running off internal pull-ups.
	//
	// 0 means "as fast as possible": no delay loop at all, so the
	// rate is whatever the wishbone transactions come out at (on the
	// order of 100-200kHz). Useful for a scan; not for a device with
	// a documented maximum.
	uint32_t khz;

	// How long to wait for a released line to read back high before
	// giving up with Z_I2C_TIMEOUT. This is also the clock-stretch
	// budget, since they are the same wait.
	//
	// 1000 (1ms) is generous for both. A slow ADC can stretch for
	// hundreds of microseconds; nothing legitimate stretches for a
	// millisecond.
	uint32_t timeout_us;

	// -- derived by z_i2c_init(); do not set --

	uint32_t half_cycles;
	uint32_t timeout_cycles;
	uint32_t last_bits;			// for z_i2c_measured_khz()
	uint32_t last_cycles;

} z_i2c_t;

// Set up the pins and leave the bus idle (both lines released).
//
// Returns Z_I2C_OK, or Z_I2C_BUSY if either line is still low with
// nothing driving it -- which is a real answer rather than a failure
// to initialise: the pins are configured either way, so a caller can
// go straight to z_i2c_recover() and try again.
//
// Safe to call repeatedly. There is no z_i2c_close(): the pins are
// left as inputs by every operation here, which is the idle state of
// the bus and also harmless if the caller walks away.
z_i2c_rv z_i2c_init(z_i2c_t *b);

// Clock SCL up to nine times with SDA released, then send a STOP.
//
// This is the standard unwedge for the one failure a master can
// actually fix: a slave that was interrupted mid-byte (by a reset, or
// by this process being killed between two calls) is still holding SDA
// low waiting for clocks that will never come. Nine of them walk it to
// the end of the byte it thinks it is sending, and the STOP resets it.
//
// Returns Z_I2C_OK if SDA came back up, Z_I2C_TIMEOUT if it did not --
// at which point the problem is not a stuck slave and no software can
// fix it.
z_i2c_rv z_i2c_recover(z_i2c_t *b);

// Is there a device at this 7-bit address? A one-byte write with no
// data, which is the conventional probe and is what z_i2c_scan() is
// made of.
//
// `addr` is the 7-BIT address throughout this file -- 0x3c, not 0x78.
// The read/write bit is this library's business, not the caller's.
// Getting this wrong is the single most common I2C mistake and the
// only defence is to be consistent and say so.
bool z_i2c_probe(z_i2c_t *b, uint8_t addr);

// Probe every valid address (0x08-0x77; the rest are reserved) and
// write those that answered into `found`. Returns how many, which may
// exceed `max` -- in which case only the first `max` were stored.
int z_i2c_scan(z_i2c_t *b, uint8_t *found, int max);

// START, address+W, n bytes, then STOP unless `stop` is false.
//
// `stop == false` leaves the bus held for a repeated START, which is
// what z_i2c_write_read() below is built from. A caller doing that
// must follow up promptly: the bus is unusable by anything else until
// it does.
z_i2c_rv z_i2c_write(z_i2c_t *b, uint8_t addr,
	const uint8_t *data, uint32_t n, bool stop);

// START, address+R, n bytes, then STOP unless `stop` is false.
//
// The master NACKs the final byte, as the protocol requires -- that is
// how a slave is told to stop driving. n == 0 is a no-op returning
// Z_I2C_OK rather than an addressed read of nothing, which some
// devices dislike.
z_i2c_rv z_i2c_read(z_i2c_t *b, uint8_t addr,
	uint8_t *data, uint32_t n, bool stop);

// Write then read with a REPEATED START between, no STOP in the
// middle. This is the correct way to read a register from almost every
// I2C device, and doing it as a write-STOP-read pair instead is the
// second most common I2C mistake: a device that has been given a
// register pointer and then seen a STOP is entitled to forget it, and
// some do, intermittently.
z_i2c_rv z_i2c_write_read(z_i2c_t *b, uint8_t addr,
	const uint8_t *w, uint32_t wn, uint8_t *r, uint32_t rn);

// The common case of the above: one register byte out, one byte back.
z_i2c_rv z_i2c_reg_read8(z_i2c_t *b, uint8_t addr, uint8_t reg,
	uint8_t *val);
z_i2c_rv z_i2c_reg_write8(z_i2c_t *b, uint8_t addr, uint8_t reg,
	uint8_t val);

// What the bus actually managed during the last transfer, in kHz.
//
// Not the requested rate and usually below it, for two reasons worth
// telling apart: the delay loop cannot go below the cost of the
// wishbone transactions themselves, and a weak pull-up adds real rise
// time to every released edge. If this is far under `khz` on a bus
// that should be fast, the pull-ups are the thing to look at -- which
// is exactly the diagnosis this exists to make possible.
//
// 0 if nothing has been transferred yet.
uint32_t z_i2c_measured_khz(const z_i2c_t *b);

// "ok" / "nack" / "timeout" / "busy", for printing.
const char *z_i2c_strerror(z_i2c_rv rv);

#endif
