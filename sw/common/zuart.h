#ifndef ZUART_H
#define ZUART_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UART1 -- a second 16550 at 0xf000_0100, available to software as a
 * general-purpose serial port. See docs/uart1.md.
 *
 * -- UART0 IS NOT THIS AND NEVER WILL BE --
 *
 * UART0 is the console. sw/bios/bios.c writes to it before anything
 * else in the system exists, the kernel prints to it, and `sh` reads
 * from it. There is deliberately no API here for it: an app that
 * reconfigured the console's baud rate would take the machine's only
 * diagnostic channel with it, and on a board where the console is the
 * only I/O at all (see rtl/uart_null.v's header for the opposite case)
 * that is unrecoverable without a reflash.
 *
 * -- One owner, by convention --
 *
 * These are plain MMIO accesses, like sw/common/zgpio.h and every other
 * peripheral an app touches directly (docs/app_runtime.md). Nothing
 * arbitrates. Two processes calling z_uart1_open() with different baud
 * rates will each believe they won.
 *
 * The convention is that sw/apps/serial owns UART1 and everything else
 * goes through it as a port (sw/common/zport.h) -- which is also what
 * makes it reachable from `term`. Using this header directly is right
 * for a program that IS the driver for whatever is plugged in; it is
 * wrong for a program that just wants to talk to a modem while
 * something else also does.
 *
 * -- Polled, not interrupt-driven --
 *
 * There is no ISR behind this. rtl/sysctl.v does NOT wire the 16550's
 * interrupt output to cpu_irq[9], and that is a considered omission
 * rather than unfinished work: a level-sensitive source with no
 * handler is a livelock, and one with a latched handler that ignores
 * it is a spurious ISR entry per edge for no benefit. The two-line
 * change to wire it (and to clear bit 9 of LATCHED_IRQ, for the same
 * reason bits 4 and 7 are clear) is described in docs/uart1.md, and it
 * is no more work later than now.
 *
 * WHAT THAT COSTS: the 16550's receive FIFO is 16 bytes. A reader that
 * polls once per scheduler slice sees 16 bytes per 1.365ms, which is
 * about 11.7 kB/s -- comfortable at 115200 (14.4 kB/s is close, so a
 * busy system will drop) and hopeless at 1 Mbaud. z_uart1_status()
 * reports the overrun bit, so a reader that is losing bytes can find
 * out rather than quietly receiving corruption.
 *
 * The ULX3S needed a 2KB block-RAM FIFO in gateware
 * (rtl/esp32_rxfifo.v) for exactly this reason at 1 Mbaud. If UART1
 * ever needs to go that fast here, that is the shape of the answer.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"
#include "zeitlos.h"	// reg_uart1_* -- the register map already lives there

// -- presence --

// true if this bitstream has a UART1 that is available to software.
//
// FEATURES2 bit 1, which is deliberately CLEAR on a board with
// `ESP32_LINK even though such a board does have a UART1: there it is
// soldered to the on-board ESP32 and is sw/apps/net's data plane, with
// no header and no second owner. See sw/common/zsoc.h.
//
// There is no magic register to cross-check against, unlike
// sw/common/zgpio.h -- the 16550 is a third-party core with no
// identity register, and its LSR reads as 0x60 whether it is there or
// not (on a board without it, 0xf000_0100 falls through to whatever
// the bus resolves to). So the feature bit is the only answer, and
// calling anything below without checking it first is reading a
// register that may not exist.
bool z_uart1_present(void);

// -- setup --

// 8 data bits, no parity, 1 stop bit, at `baud`. The common case.
//
// Returns false if there is no UART1, or if `baud` cannot be produced
// from Z_SYSCLK_HZ -- see z_uart1_baud_error() for how far off it
// would have been.
bool z_uart1_open(uint32_t baud);

// The general form. `bits` is 5-8, `parity` is 'n', 'e' or 'o', `stop`
// is 1 or 2.
bool z_uart1_config(uint32_t baud, uint8_t bits, char parity, uint8_t stop);

// Percent error between the requested baud rate and what the divisor
// actually produces, times 100 (so 137 means 1.37%).
//
// Worth checking rather than assuming, because the 16550 divides
// Z_SYSCLK_HZ by 16*n with n an integer, and at high baud rates n gets
// small enough that the rounding matters: at 48MHz, 1000000 baud is
// exactly n=3, but 921600 wants n=3.26 and lands on 3, which is 1
// Mbaud -- 8.5% fast, and far outside the ~2-3% a UART tolerates. The
// symptom is a port that works at 115200 and produces garbage at
// 921600, which reads like a cable problem.
//
// z_uart1_open() refuses anything worse than 3%.
uint32_t z_uart1_baud_error(uint32_t baud);

// Stop driving TX. Does not reset the 16550 -- there is no need, and
// leaving the divisor alone means reopening at the same rate is free.
//
// Nothing is released or freed; this exists so a process can say it is
// done in a way that reads as the opposite of open(). Whether TX idles
// high afterwards is the pin's business, not this block's.
void z_uart1_close(void);

// -- transmit --

bool     z_uart1_tx_ready(void);		// room in the transmit FIFO
void     z_uart1_putc(char c);			// blocks until there is room
uint32_t z_uart1_write(const void *buf, uint32_t n);	// blocks; returns n

// As above but never blocks: writes as many bytes as fit in the FIFO
// and returns how many that was.
//
// This is the one a port server wants. z_uart1_write() blocking looks
// harmless until the far end is not reading and CTS is not wired
// (which it is not -- see docs/uart1.md), at which point the process
// stops answering messages and `term` appears to hang.
uint32_t z_uart1_write_nb(const void *buf, uint32_t n);

// -- receive --

bool     z_uart1_rx_ready(void);
int      z_uart1_getc(void);			// -1 if nothing is waiting
uint32_t z_uart1_read(void *buf, uint32_t n);	// up to n; never blocks
void     z_uart1_flush_rx(void);

// -- status --

#define Z_UART1_OVERRUN 0x01u	// a byte arrived before the last was read
#define Z_UART1_PARITY  0x02u
#define Z_UART1_FRAMING 0x04u	// wrong bit count -- usually a baud mismatch
#define Z_UART1_BREAK   0x08u

// Sticky error bits since the last call, cleared by reading.
//
// Sticky because the 16550's own LSR bits are cleared by a read of
// that register, and every read of the data register goes past it --
// so an overrun that happened between two z_uart1_read() calls would
// be gone before anyone asked. This accumulates them instead.
//
// Z_UART1_OVERRUN means bytes were LOST, and on a polled receiver
// that is a real possibility rather than a theoretical one -- see this
// file's header on the 16-byte FIFO.
//
// Z_UART1_FRAMING usually means the baud rate is wrong, not that the
// wire is bad. Check z_uart1_baud_error() first.
uint32_t z_uart1_status(void);

#endif
