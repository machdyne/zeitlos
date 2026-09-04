/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * GPIO -- see sw/common/zgpio.h for the interface and docs/gpio.md
 * for the hardware.
 *
 * A .c rather than a header of inlines, unlike sw/common/zsoc.h which
 * this sits next to. The presence and parsing functions have real
 * bodies, and the per-pin calls are small but are called from a loop
 * in another translation unit (sw/common/zi2c.c) -- so inlining them
 * everywhere would put a copy in every app that links this for the
 * sake of the LED helpers. Section GC (see any app Makefile) drops
 * whatever is not called, so the cost of the ones you do not use is
 * zero either way.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgpio.h"

// Everything below funnels through this. Out of range means the
// register write goes nowhere rather than into whatever the address
// arithmetic would otherwise land on -- see zgpio.h's note on why the
// check is against the map's reserved maximum and not against what
// this board actually built.
static bool port_ok(uint32_t port) {
	return port < Z_GPIO_MAX_PORTS;
}

static bool pin_ok(uint32_t pin) {
	return pin < Z_GPIO_PINS_PER_PORT;
}

bool z_gpio_present(void) {

	// Cheaper test first; both have to pass. See zgpio.h on why this
	// checks two things, and on why -- unlike the RTC and the TRNG --
	// the order carries no hang hazard.
	if (!z_soc_has_feature2(Z_FEATURE2_GPIO)) return false;
	return reg_gpio_magic == Z_GPIO_MAGIC;

}

uint32_t z_gpio_port_count(void) {

	uint32_t cfg;

	if (reg_gpio_magic != Z_GPIO_MAGIC) return 0;

	cfg = reg_gpio_config;

	// The signature is not paranoia. A bitstream from before this
	// block existed answers every address in the 0xE nibble it does
	// not recognise with { 31'b0, led } -- so CONFIG reads back as 0
	// or 1, and 1 is exactly what one built port would report. The
	// "GP" in the top half is the only thing separating those. (Such
	// a bitstream fails the magic check above too, so this is the
	// second of two nets; the first one to go would be the magic if
	// somebody ever reads CONFIG directly.)
	if (((cfg >> 16) & 0xffffu) != Z_GPIO_CONFIG_SIG) return 0;

	cfg &= 0xfu;
	if (cfg > Z_GPIO_MAX_PORTS) return Z_GPIO_MAX_PORTS;

	return cfg;

}

// -- whole-port access ------------------------------------------

uint8_t z_gpio_dir_get(uint32_t port) {
	if (!port_ok(port)) return 0;
	return (uint8_t)(*z_gpio_reg(port, Z_GPIO_REG_DIR) & 0xffu);
}

void z_gpio_dir_set(uint32_t port, uint8_t mask) {
	if (!port_ok(port)) return;
	*z_gpio_reg(port, Z_GPIO_REG_DIR) = mask;
}

uint8_t z_gpio_out_get(uint32_t port) {
	if (!port_ok(port)) return 0;
	return (uint8_t)(*z_gpio_reg(port, Z_GPIO_REG_OUT) & 0xffu);
}

void z_gpio_out_put(uint32_t port, uint8_t val) {
	if (!port_ok(port)) return;
	*z_gpio_reg(port, Z_GPIO_REG_OUT) = val;
}

uint8_t z_gpio_in_get(uint32_t port) {
	if (!port_ok(port)) return 0;
	return (uint8_t)(*z_gpio_reg(port, Z_GPIO_REG_IN) & 0xffu);
}

// -- per-pin access ---------------------------------------------

void z_gpio_mode(uint32_t port, uint32_t pin, z_gpio_mode_t mode) {

	uint32_t bit;

	if (!port_ok(port) || !pin_ok(pin)) return;

	bit = 1u << pin;

	switch (mode) {

	case Z_GPIO_OUT:
		*z_gpio_reg(port, Z_GPIO_REG_DIRSET) = bit;
		break;

	case Z_GPIO_OD:
		// Order matters, and it is the safe one: clear OUT while the
		// pin is still an input, THEN leave it floating. Doing it the
		// other way round -- float, then clear -- would be fine too,
		// but a version that set DIR anywhere in here would drive the
		// line for as long as it took to get to the next store.
		//
		// The pin is left released (an input), not driving. That is
		// the idle state of every open-drain bus, so a caller can set
		// the mode on both I2C pins and then start a transfer without
		// an intermediate "and now let go" step.
		*z_gpio_reg(port, Z_GPIO_REG_OUTCLR) = bit;
		*z_gpio_reg(port, Z_GPIO_REG_DIRCLR) = bit;
		break;

	case Z_GPIO_IN:
	default:
		*z_gpio_reg(port, Z_GPIO_REG_DIRCLR) = bit;
		break;

	}

}

z_gpio_mode_t z_gpio_mode_get(uint32_t port, uint32_t pin) {

	if (!port_ok(port) || !pin_ok(pin)) return Z_GPIO_IN;

	return (*z_gpio_reg(port, Z_GPIO_REG_DIR) & (1u << pin))
		? Z_GPIO_OUT : Z_GPIO_IN;

}

bool z_gpio_read(uint32_t port, uint32_t pin) {

	if (!port_ok(port) || !pin_ok(pin)) return false;

	return (*z_gpio_reg(port, Z_GPIO_REG_IN) & (1u << pin)) != 0;

}

void z_gpio_write(uint32_t port, uint32_t pin, bool value) {

	if (!port_ok(port) || !pin_ok(pin)) return;

	// One store. The whole reason OUTSET and OUTCLR exist -- see
	// rtl/gpio.v's header on what the read-modify-write alternative
	// costs on the inner loop of a bit-bang transfer, and on why it
	// is not atomic against the KTIMER interrupt.
	*z_gpio_reg(port, value ? Z_GPIO_REG_OUTSET : Z_GPIO_REG_OUTCLR)
		= 1u << pin;

}

void z_gpio_toggle(uint32_t port, uint32_t pin) {

	uint32_t bit;

	if (!port_ok(port) || !pin_ok(pin)) return;

	bit = 1u << pin;

	// Reads OUT, not IN. Toggling relative to the pin would mean an
	// output fighting an external driver flips to agree with it,
	// which is not what anyone means by "toggle".
	if (*z_gpio_reg(port, Z_GPIO_REG_OUT) & bit)
		*z_gpio_reg(port, Z_GPIO_REG_OUTCLR) = bit;
	else
		*z_gpio_reg(port, Z_GPIO_REG_OUTSET) = bit;

}

void z_gpio_od_write(uint32_t port, uint32_t pin, bool value) {

	if (!port_ok(port) || !pin_ok(pin)) return;

	// Inverted against z_gpio_write() on purpose: driving is what a 0
	// means on an open-drain line. One store either way.
	*z_gpio_reg(port, value ? Z_GPIO_REG_DIRCLR : Z_GPIO_REG_DIRSET)
		= 1u << pin;

}

// -- board LEDs --------------------------------------------------

void z_led_set(bool on) {
	reg_gpio_led = on ? 1u : 0u;
}

void z_led_bar_set(uint8_t bits) {
	reg_gpio_leds = bits;
}
