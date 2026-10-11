#ifndef ZI2CS_H
#define ZI2CS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The GPIO I2C slave -- rtl/gpio_i2cs.v, docs/gpio.md ("I2C slave").
 * Lets a real I2C master -- a Zwölf module's LS10 -- talk to chips that
 * exist only in software, with no software in the transaction: up to
 * four SLOTS, each an emulated chip with an address, 256 registers (the
 * MIRROR), a register pointer and an auto-increment rule. The hardware
 * answers the master from the mirror; software keeps the mirror current
 * and applies the side effects of what the master wrote, from a log.
 * docs/bench.md is what it is for.
 *
 * Off unless the bitstream was built with it (rtl/boards.vh,
 * GPIO_I2CS_SLOTS). z_i2cs_slots() is 0 then, and on every older
 * bitstream.
 *
 * -- Use --
 *
 *   if (z_i2cs_open(Z_I2CS_PIN(0, 2), Z_I2CS_PIN(0, 3)) != Z_I2CS_OK) ...
 *   z_i2cs_map(0, 0x20, Z_I2CS_INC_PAIR);           // a TCA9535
 *   for (r = 0; r < 8; r++)
 *       z_i2cs_mirror_write(0, r, regs[r], r >= 2);  // 0-1 read only
 *   ...every loop:
 *   while (z_i2cs_log_pop(&ev)) apply(ev.slot, ev.reg, ev.data);
 *   if (z_i2cs_reads(true) & 1) ...the master read slot 0, from
 *                                  z_i2cs_read_start(0)
 *   z_i2cs_close();
 *
 * -- What the master sees --
 *
 *   - a slot's address ACKs (a bare probe too); any other does not
 *   - a write's first byte sets the slot's pointer; each byte after it
 *     is stored at the pointer, if that register is writable, and the
 *     pointer advances. A byte for a register that is not writable is
 *     ACKed and dropped, as a TCA's input port does
 *   - a read sends from the pointer, advancing; write-then-read with a
 *     repeated start sets the pointer and reads from it
 *   - the pointer PERSISTS between transactions, per slot
 *   - no clock stretching, ever
 *
 * -- What software must allow for (docs/gpio.md) --
 *
 *   - A register that depends on others (a TCA's input port showing
 *     what its outputs drive) is only as current as the last time
 *     software rewrote it: a loop behind.
 *   - A multi-byte read is consistent byte by byte only: a read during
 *     a software update can return some old bytes and some new.
 *   - The log holds 512 writes. Past that, writes still reach the
 *     mirror but are not logged, and z_i2cs_overflowed() says so.
 *
 * -- Ownership --
 *
 * The slave is one block for the whole machine: z_i2cs_open() refuses
 * when it is already enabled (Z_I2CS_EBUSY), and the pins it takes are
 * refused to a stream engine (zgpio_stream.h) and vice versa. Like an
 * engine, it stays on if its process exits without z_i2cs_close().
 */

#include <stdint.h>
#include <stdbool.h>

#define Z_I2CS_BASE      0xe0002100u

#define Z_I2CS_REG_ICTL    0x00
#define Z_I2CS_REG_ISTAT   0x04
#define Z_I2CS_REG_IMADDR  0x08
#define Z_I2CS_REG_IMDATA  0x0c
#define Z_I2CS_REG_ISLOT0  0x10	// + slot * 4
#define Z_I2CS_REG_ILOG    0x20
#define Z_I2CS_REG_IFLUSH  0x24

#define Z_I2CS_MAX_SLOTS 4
#define Z_I2CS_LOG_DEPTH 512

// Pins are port * 8 + pin, as zgpio_stream.h numbers them.
#define Z_I2CS_PIN(port, pin) ((port) * 8 + (pin))

// auto-increment, per slot
#define Z_I2CS_INC_NONE    0	// the pointer stays put
#define Z_I2CS_INC_LINEAR  1	// +1, wrapping at 255
#define Z_I2CS_INC_PAIR    2	// XOR 1: TCA9535/9555 register pairs

// results
#define Z_I2CS_OK         0
#define Z_I2CS_ENONE    (-1)	// not built into this bitstream
#define Z_I2CS_EPIN     (-2)	// a pin out of range, or both the same
#define Z_I2CS_EPINBUSY (-3)	// a stream engine has that pin
#define Z_I2CS_EBUSY    (-4)	// already enabled (another process?)
#define Z_I2CS_ESLOT    (-5)	// no such slot in this build

typedef struct {
	uint8_t slot;
	uint8_t reg;
	uint8_t data;
} z_i2cs_event_t;

// Slots this bitstream has: 0 (none), 1, 2 or 4.
uint32_t z_i2cs_slots(void);

// Enable the slave on two pins. Every slot starts unmapped.
int  z_i2cs_open(int scl, int sda);

// Disable it: the pins go back to GPIO, every slot is unmapped.
void z_i2cs_close(void);

// Answer `addr` (7-bit) as `slot`, with an auto-increment rule. The
// slot's pointer is left where it is (0 after open).
int  z_i2cs_map(int slot, uint8_t addr, int inc);
void z_i2cs_unmap(int slot);

// The slot's register pointer.
void z_i2cs_set_ptr(int slot, uint8_t reg);
uint8_t z_i2cs_ptr(int slot);

// One mirror register, and whether the master may write it.
void z_i2cs_mirror_write(int slot, uint8_t reg, uint8_t val, bool writable);
uint8_t z_i2cs_mirror_read(int slot, uint8_t reg, bool *writable);

// n registers from `first` (n <= 256), all with one writable setting --
// the quick way to load a whole chip, one bus write a register.
void z_i2cs_mirror_load(int slot, uint8_t first, const uint8_t *vals,
                        uint32_t n, bool writable);

// The log of what the master wrote, oldest first.
uint32_t z_i2cs_log_level(void);
bool z_i2cs_log_pop(z_i2cs_event_t *ev);
void z_i2cs_log_flush(void);
bool z_i2cs_overflowed(bool clear);

// Slots the master has started a read on (bit per slot) since the last
// clear, and the register the latest read on a slot started at.
uint32_t z_i2cs_reads(bool clear);
uint8_t z_i2cs_read_start(int slot);

// A transaction addressed to one of our slots is under way.
bool z_i2cs_busy(void);

#endif
