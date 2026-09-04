#ifndef ZGPIO_H
#define ZGPIO_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO -- up to eight ports of eight bidirectional pins on the PMOD
 * connectors. Hardware is rtl/gpio.v; docs/gpio.md is the reference
 * for the register map, the pin numbering and the pull-ups.
 *
 * This is the layer the bit-bang I2C (sw/common/zi2c.h) and SPI
 * (sw/common/zspi.h) libraries are built on, and it is shaped for
 * them: the single-pin calls are one store each with no
 * read-modify-write anywhere, because they are the inner loop of every
 * byte those libraries move.
 *
 * -- Plain MMIO, no syscalls --
 *
 * Same treatment as every other peripheral an app touches directly
 * (reg_sdcard, reg_eth, the gpu_* registers): these are volatile
 * accesses at a fixed physical address, and the kernel is not
 * involved. See docs/app_runtime.md.
 *
 * WHICH MEANS THERE IS NO ARBITRATION. Two processes writing the same
 * port will interleave, and nothing detects it. That is the same deal
 * the SD card and the ethernet MAC already have, and it is fine for
 * the same reason: one process owns the hardware by convention. If
 * that ever stops being enough, the answer is a server process that
 * owns the pins and takes messages, not a lock in here.
 *
 * -- Pins are (port, pin), both numbers, everywhere --
 *
 * There is no letter form and no pin-name string to parse. Ports are
 * 0..7 and pins are 0..7, in the API, at the shell prompt, in Scheme,
 * in the docs.
 *
 * There WAS a letter form -- "B3" for port 1 pin 3 -- and it was
 * removed, because letters already mean something else here. A board
 * spec in release/hw/boards says `pmod.a` and `pmod.b`, and
 * THOSE ARE THE PHYSICAL CONNECTORS. Port indices are not: the
 * hardware numbers ports in the order rtl/sysctl.v declares them and
 * knows nothing about silkscreens, so on obst_uart_gpio the port the
 * release system calls `b` is the one this API calls 0. Under the old
 * scheme that was also "A", which meant two lettering systems in one
 * project disagreeing about the same connector. The header, the shell
 * and the docs each carried a warning paragraph about it, which is
 * usually a sign the notation is wrong rather than that the reader is
 * careless.
 *
 * Write "port 0 pin 3" in prose, or "0.3" where something shorter is
 * wanted. Neither is parsed by anything; they are just how to say it.
 * The release notes for a target say which connector each port is on.
 *
 * -- Nothing here validates against the hardware --
 *
 * z_gpio_write(3, 0, true) on a board with one port writes to a
 * register that does not exist, and rtl/gpio.v drops it. No fault, no
 * error, no way to find out afterwards. That is a deliberate property
 * of this bus, not an oversight (see rtl/gpio.v's header), and it is
 * why z_gpio_port_count() exists: ask BEFORE you write. The bounds
 * check that IS here is against Z_GPIO_MAX_PORTS, which only stops a
 * wild index from landing on another peripheral.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"

// rtl/gpio.v's register map. Word addresses in the Verilog, byte
// addresses here -- see docs/gpio.md for the full table.
#define reg_gpio_led    (*(volatile uint32_t*)0xe0000000)
#define reg_gpio_leds   (*(volatile uint32_t*)0xe0000004)
#define reg_gpio_magic  (*(volatile uint32_t*)0xe0000008)
#define reg_gpio_config (*(volatile uint32_t*)0xe000000c)

#define Z_GPIO_MAGIC     0x5A475049u	// "ZGPI" -- see rtl/gpio.v
#define Z_GPIO_CONFIG_SIG    0x4750u	// "GP", top half of CONFIG

// Port register block: 0xe000_1000 + port * 0x20.
#define Z_GPIO_PORT_BASE 0xe0001000u
#define Z_GPIO_PORT_SIZE 0x20u

#define Z_GPIO_REG_DIR    0x00
#define Z_GPIO_REG_OUT    0x04
#define Z_GPIO_REG_IN     0x08
#define Z_GPIO_REG_OUTSET 0x0c
#define Z_GPIO_REG_OUTCLR 0x10
#define Z_GPIO_REG_DIRSET 0x14
#define Z_GPIO_REG_DIRCLR 0x18

// What the register map reserves, not what any board builds. Ports are
// declared four at a time in rtl/sysctl.v today; ask
// z_gpio_port_count() for the real number.
#define Z_GPIO_MAX_PORTS 8
#define Z_GPIO_PINS_PER_PORT 8

// Direct register access. Everything below is built on these, and they
// are exposed because a tight bit-bang loop sometimes wants to hoist
// the address computation out of the loop itself.
//
// `port` is NOT bounds-checked here. Use the wrappers below unless you
// have already checked it.
static inline volatile uint32_t *z_gpio_reg(uint32_t port, uint32_t off) {
	return (volatile uint32_t *)(Z_GPIO_PORT_BASE
		+ port * Z_GPIO_PORT_SIZE + off);
}

// -- presence and geometry --------------------------------------

// true if this bitstream has GPIO with at least one port that has
// pins.
//
// Checks BOTH the FEATURES2 bit and rtl/gpio.v's own MAGIC, which is
// belt and braces on purpose. The feature bit is authoritative for
// "were pins built", and the magic is authoritative for "is the block
// there at all" -- and they can disagree in one direction that
// matters: a bitstream new enough to have FEATURES2 but built from a
// tree where somebody changed the GPIO block would report the bit and
// fail the magic.
//
// Unlike rtl/rtc.v and rtl/trng.v, reading the magic FIRST would be
// safe here -- the 0xE nibble has been decoded and acked on every
// bitstream ever built, because rtl/debug.v lived there before
// rtl/gpio.v did. So there is no hang hazard and no ordering rule; the
// order below is just the cheaper test first.
bool z_gpio_present(void);

// How many ports have pins, 0 if there is no GPIO. From rtl/gpio.v's
// CONFIG register, whose signature is checked -- a bitstream that
// predates this block answers that address with the LED register's
// value, and "1" would otherwise read as "one port".
uint32_t z_gpio_port_count(void);

// -- whole-port access ------------------------------------------
//
// Eight pins in one bus transaction. Use these to set up a port; use
// the per-pin calls below in a loop that is toggling things.
//
// Out-of-range ports are a no-op for the setters and 0 for the
// getters, so a caller that forgot to check the port count gets
// nothing rather than something surprising elsewhere in the address
// map.

uint8_t z_gpio_dir_get(uint32_t port);
void    z_gpio_dir_set(uint32_t port, uint8_t mask);	// 1 = output
uint8_t z_gpio_out_get(uint32_t port);
void    z_gpio_out_put(uint32_t port, uint8_t val);
uint8_t z_gpio_in_get(uint32_t port);					// the pins

// -- per-pin access ---------------------------------------------

typedef enum {
	// Input. The pin floats as far as this chip is concerned, and the
	// weak internal pull-up (release/hw/pmods/gpio.spec) makes it read
	// high when nothing else drives it. This is the reset state of
	// every pin on every port.
	Z_GPIO_IN = 0,

	// Push-pull output. Drives both levels.
	Z_GPIO_OUT,

	// Open drain, which is NOT a hardware mode -- rtl/gpio.v has no
	// such thing and does not need one. Setting this mode clears the
	// pin's OUT bit and floats it; z_gpio_write() on a pin in this
	// mode then moves DIR instead of OUT, so a 0 drives low and a 1
	// releases to the pull-up.
	//
	// This is what I2C needs, and it is safe by construction rather
	// than by care: the register that would have to hold a 1 to drive
	// the line high is never written to anything but 0, so this code
	// physically cannot drive into a line another device is pulling
	// down. See docs/gpio.md.
	//
	// The mode is not stored in hardware -- there is nowhere to store
	// it -- so z_gpio_mode_get() reports IN or OUT for a pin set this
	// way, depending on whether it currently happens to be driving.
	// Callers that need to remember keep their own record;
	// sw/common/zi2c.h holds the pins in a struct and never asks.
	Z_GPIO_OD
} z_gpio_mode_t;

void z_gpio_mode(uint32_t port, uint32_t pin, z_gpio_mode_t mode);

// What DIR currently says: Z_GPIO_OUT if the pin is driving,
// Z_GPIO_IN if not. Never returns Z_GPIO_OD -- see that mode's note.
z_gpio_mode_t z_gpio_mode_get(uint32_t port, uint32_t pin);

// Read the PIN, not the output register. On an input this is whatever
// the outside world is doing; on an output it is what this chip is
// driving, unless something else is fighting it.
bool z_gpio_read(uint32_t port, uint32_t pin);

// Drive a pin. ONE STORE, to OUTSET or OUTCLR -- no read, no
// read-modify-write, and therefore nothing a timer interrupt can land
// in the middle of. That is why those registers exist; see
// rtl/gpio.v.
//
// Does not change DIR. A pin that is an input will not start driving
// because you called this; it stages the value for whenever it does.
void z_gpio_write(uint32_t port, uint32_t pin, bool value);

// Read OUT (not the pin) and write back the opposite. Two
// transactions, and unlike z_gpio_write() this one IS a
// read-modify-write -- but only of this pin's own bit, and the
// intervening write would have to come from another process on the
// same port to matter.
void z_gpio_toggle(uint32_t port, uint32_t pin);

// Open-drain write: false drives the pin low, true releases it to the
// pull-up. One store, to DIRSET or DIRCLR.
//
// Assumes the pin's OUT bit is 0, which z_gpio_mode(.., Z_GPIO_OD)
// arranges and which is also the reset state of every pin -- so a
// caller that only ever uses this and never touches OUT is correct
// without doing anything. Note the inversion against z_gpio_write():
// here `false` means "drive", because that is what open drain is.
void z_gpio_od_write(uint32_t port, uint32_t pin, bool value);

// -- board LEDs --------------------------------------------------
//
// The same block, at words 0 and 1. They are here because they are in
// this block, not because they have anything to do with GPIO -- and
// they are at the addresses rtl/debug.v had them at, so sw/bios/bios.c
// and sw/os/uart.c keep working untouched. Present on every board;
// z_gpio_present() has nothing to say about them.

void z_led_set(bool on);			// LED_B, the board LED
void z_led_bar_set(uint8_t bits);	// DBG[7:0], the `LED_DEBUG bar

#endif
