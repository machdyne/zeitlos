/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UART interface.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "../common/zeitlos.h"
#include "kernel.h"
#include "uart.h"

#define UART_FIFO_SIZE 512

volatile uint8_t __attribute__((section(".bss"))) uart_rx_fifo[UART_FIFO_SIZE];
volatile uint8_t __attribute__((section(".bss"))) uart_tx_fifo[UART_FIFO_SIZE];
/* uint16_t, not uint8_t: UART_FIFO_SIZE is 512, and (head + 1) % 512
 * truncated into a uint8_t wraps at 256 -- so the ring silently had
 * half the depth it claims, and index arithmetic disagreed with the
 * array bounds. */
volatile uint16_t __attribute__((section(".bss"))) rx_head = 0, rx_tail = 0;
volatile uint16_t __attribute__((section(".bss"))) tx_head = 0, tx_tail = 0;

uint16_t leds = 0x00;

void z_uart_init(void) {

   reg_uart0_fcr = (uint8_t)0b00000111; // flush FIFOs
	reg_uart0_ier = (uint8_t)0b00000001; // enable RX interrupt

	reg_leds = 0;

}

/* Push as much of the software ring into the 16550 as it will take,
 * right now, without waiting for anything.
 *
 * This is the fix for a deadlock that was latent for as long as this
 * file has existed and that a faster CPU core made reachable.
 *
 * The old code sent a character directly ONLY when the ring happened
 * to be empty and THRE was set, and otherwise queued it and left the
 * draining to z_uart_irq(). That has two problems:
 *
 *   1. z_uart_irq() is only reachable once reg_kernel (0x0000000c) is
 *      set, which kernel.c does AFTER k_soc_report(). Every printf()
 *      before that point queued into a ring nothing emptied.
 *   2. Even afterwards, the "ring was empty" test is a one-way latch.
 *      _write() calls k_uart_putc() once per character with only a
 *      few instructions in between, so a single _write() of a 40-byte
 *      string outruns the UART (1 Mbaud is 480 CPU cycles per
 *      character) and leaves the ring non-empty. From that moment
 *      fifo_was_empty is false forever and no character is ever sent
 *      directly again, no matter how idle the CPU becomes.
 *
 * Combined, that meant _write()'s `while (k_uart_tx_full()) ;` could
 * spin forever with no trap and no diagnostic. picorv32 survived it
 * only by being slow enough to stay on the right side of the burst
 * rate; zeitlos32 is 20-35%% fewer cycles for the same work and
 * crosses it.
 *
 * Pumping unconditionally removes the dependency on the interrupt for
 * forward progress. The interrupt still helps -- it drains in the
 * background instead of making the writer wait -- but nothing needs
 * it to make progress any more.
 *
 * Callers must already hold the IRQ mask: this touches tx_tail, which
 * z_uart_irq() also writes.
 */
static void tx_pump(void) {
	while ((tx_head != tx_tail) && (reg_uart0_lsr & 0x20)) {
		reg_uart0_data = uart_tx_fifo[tx_tail];
		tx_tail = (tx_tail + 1) % UART_FIFO_SIZE;
	}
}

void uart_irq_enable(void) {
    uint32_t mask = maskirq(0);               // read current IRQ mask
    mask &= ~(1 << 4);                        // clear bit 4 to unmask UART
    maskirq(mask);                            // write new mask
}

void uart_irq_disable(void) {
    uint32_t mask = maskirq(0);               // read current IRQ mask
    mask |= (1 << 4);                         // set bit 4 to mask UART
    maskirq(mask);                            // write new mask
}

uint32_t ints = 0;

void z_uart_irq(void) {

	uint8_t iir = reg_uart0_iir;

	if (!(iir & 0x01)) {

		uint8_t lsr = reg_uart0_lsr;

		// error
		if (lsr & 0x80) {
			char c = reg_uart0_data;
			return;
		}

		uint8_t int_id = (iir >> 1) & 0x07;

		//reg_leds = int_id;

		switch (int_id) {

			case 0x01: // Transmit Holding Register Empty (THRE)
				tx_pump();
				// nothing left to send: stop asking to be told about it
				if (tx_head == tx_tail) reg_uart0_ier = 0x01;
				break;

			case 0x02: // Received Data Available (RDA)
			case 0x06: // Character Timeout Indication (treated same as RDA)

				while (reg_uart0_lsr & 0x01) {  // data Ready

					uint8_t c = reg_uart0_data; // reading data clears error state
					uint16_t next = (rx_head + 1) % UART_FIFO_SIZE;
					if (next != rx_tail) {  // RX FIFO not full
						uart_rx_fifo[rx_head] = c;
						rx_head = next;
					} else {
						// TODO: handle RX overflow; currently drops character
					}
	
				}

				// Wake the console reader.
				//
				// pid 0 (the kernel shell, sw/os/sh.c) is the only
				// process that reads this port's FIFO as a console,
				// and readline() now blocks rather than spinning on
				// an empty FIFO -- so without this the prompt would
				// never wake up and the serial console would be dead.
				//
				// Hardcoded rather than a subscription like the HID
				// pointer's: there is exactly one serial console and
				// it belongs to the shell, whereas the pointer has a
				// real choice of consumer.
				//
				// Safe on a pid 0 that is not currently blocked --
				// k_proc_unblock() records the wakeup in
				// Z_PROC_FLAG_WAKE rather than losing it, which is
				// also what closes the race between deciding to wait
				// and actually being marked BLOCKED.
				k_proc_unblock(0);

				break;

			default:
				break;

		}
	}
}

static inline bool uart_tx_fifo_empty() {
    return tx_head == tx_tail;
}

static inline bool uart_rx_fifo_empty() {
    return rx_head == rx_tail;
}

static inline bool uart_tx_fifo_full() {
    return ((tx_head + 1) % UART_FIFO_SIZE) == tx_tail;
}

static inline bool uart_rx_fifo_full() {
    return ((rx_head + 1) % UART_FIFO_SIZE) == rx_tail;
}

// --

bool k_uart_rx_empty(void) {
	// same protection as k_uart_putc()/k_uart_getc(), and for the
	// same reason -- this reads rx_head/rx_tail as a pair, and an
	// unprotected read here was the actual remaining half of the
	// UART race: _read()'s poll loop (`while (uart_rx_empty())`)
	// could see an inconsistent snapshot mid-update by another
	// process's now-protected k_uart_putc()/getc() call, hanging
	// forever if that snapshot looked permanently empty/full.
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	bool v = uart_rx_fifo_empty();
	maskirq(old_mask);
	return v;
}

// Pumps before answering. sw/os/kruntime.c's _write() spins on this
// with no timeout, so an answer of "full" that nothing can change is a
// hang; pumping here means the question can always make progress on
// its own.
bool k_uart_tx_full(void) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	tx_pump();
	bool v = uart_tx_fifo_full();
	maskirq(old_mask);
	return v;
}


int16_t k_uart_getc(void) {

	// same protection as k_uart_putc() above, and for the same
	// reason -- see its comment.
	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (rx_head == rx_tail) {
		// RX FIFO is empty
		maskirq(old_mask);
		return -1;
	}

	char c = uart_rx_fifo[rx_tail];
	rx_tail = (rx_tail + 1) % UART_FIFO_SIZE;

	maskirq(old_mask);
	return c;

}

void k_uart_putc(char c) {

	// mask ALL irqs (not just the uart one) so a scheduler swap can't
	// interleave with another process also inside this function --
	// tx_head/tx_tail are shared kernel state that every process's
	// printf() ultimately writes through (apps via syscall, the
	// kernel/sh.c directly), so two processes concurrently mid-way
	// through this critical section corrupts the indices. this was a
	// real bug: uart_irq_disable() only masks the UART IRQ (bit 4),
	// not the KTIMER IRQ that drives scheduling, so a timer
	// preemption could switch to another process still inside this
	// same function. the corrupted indices could make
	// uart_tx_fifo_full() return true permanently, and _write()'s
	// `while (k_uart_tx_full()) /* wait */;` has no timeout -- so
	// whichever process's next printf() hit that state would hang
	// forever, while other processes kept running normally (matching
	// the observed symptom: net going silent forever mid-transfer
	// while sh.c's own unrelated code kept working).
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	uint16_t next;

	// Take whatever the UART will accept before deciding there is no
	// room. Without this the ring can only shrink from the interrupt,
	// which may not be reachable yet.
	tx_pump();

	next = (tx_head + 1) % UART_FIFO_SIZE;

	// Genuinely full. Block until the UART has taken something, rather
	// than dropping the character -- losing output silently is worse
	// than being slow, and this cannot deadlock because tx_pump() only
	// needs the LSR, never the interrupt.
	while (next == tx_tail) tx_pump();

	uart_tx_fifo[tx_head] = c;
	tx_head = next;

	// and send it now if the UART is ready, rather than waiting to be
	// told that it is
	tx_pump();

	// Ask for THRE only while something is actually queued. Leaving it
	// enabled with an empty ring means a THRE interrupt on every
	// character the UART finishes, for no reason.
	reg_uart0_ier = (tx_head != tx_tail) ? 0b00000011 : 0b00000001;

	maskirq(old_mask);

}

// --

z_obj_t *z_uart_rx_empty(z_obj_t *obj) {
	obj->val.int32 = k_uart_rx_empty();
	return (&z_ok);
}

z_obj_t *z_uart_tx_full(z_obj_t *obj) {
	obj->val.int32 = k_uart_tx_full();
	return (&z_ok);
}

z_obj_t *z_uart_getc(z_obj_t *obj) {
	obj->val.int32 = k_uart_getc();
	return (&z_ok);
}

z_obj_t *z_uart_putc(z_obj_t *obj) {
	char c = (char)obj->val.int32;
	k_uart_putc(c);
	return (&z_ok);
}
