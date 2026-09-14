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

// Pid blocked waiting for RX / TX-ring space, or ~0u if nobody.
// Woken from z_uart_irq; set under the same mask as the empty/full
// test so a byte that arrives between the test and BLOCKED is not
// lost (same race k_proc_wait documents for the mailbox).
static volatile uint32_t uart_rx_wait_pid = ~0u;
static volatile uint32_t uart_tx_wait_pid = ~0u;

void z_uart_init(void) {

	// FIFO enable + RX/TX flush. FCR[7:6]=00 is trigger level 1 byte
	// (uart_regs.v: 00=1, 01=4, 10=8, 11=14). 0b111 was once read here
	// as trigger 14; those are the flush bits, not the trigger. Slack
	// at 1 Mbaud is then 16 bytes, not 2 -- leave it at 1.
	reg_uart0_fcr = (uint8_t)0b00000111;
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
				if (uart_tx_wait_pid != ~0u &&
					((tx_head + 1) % UART_FIFO_SIZE) != tx_tail) {
					k_proc_unblock(uart_tx_wait_pid);
					uart_tx_wait_pid = ~0u;
				}
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
				// readline() and _read() (sw/os/kruntime.c) block on
				// an empty FIFO rather than spinning, so without this
				// the prompt would never wake and the serial console
				// would be dead.
				//
				// The waiter registers itself in k_uart_wait_rx()
				// below rather than being hardcoded to pid 0, even
				// though pid 0 (the kernel shell, sw/os/sh.c) is the
				// only process that reads this port as a console
				// today: the pid is written under the same mask as
				// the empty test, which is what closes the window
				// between deciding to wait and actually being marked
				// BLOCKED, and it means an unrelated block of pid 0
				// is not woken by every byte that arrives.
				if (uart_rx_wait_pid != ~0u && rx_head != rx_tail) {
					k_proc_unblock(uart_rx_wait_pid);
					uart_rx_wait_pid = ~0u;
				}

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

// Pumps before answering. Callers that used to spin on this now
// block inside k_uart_putc() (kernel) or z_proc_wait(1) (apps), so a
// "full" answer is no longer a hang -- the TX ISR shrinks the ring.
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

	// The index update still masks ALL IRQs -- tx_head/tx_tail are
	// shared kernel state, and masking only the UART IRQ (bit 4)
	// used to let a KTIMER swap land another process in this same
	// function, corrupting the ring so uart_tx_fifo_full() stayed
	// true forever. What must not happen is spinning under that
	// mask: the old `while (next == tx_tail) tx_pump()` held every
	// IRQ off for as long as the 16550 took to drain, which is how
	// a 220-byte printf became 120-170 ms of wall.
	// The masked section is now the enqueue only (~20 cycles);
	// a full ring blocks the caller and lets the TX ISR drain it.

	for (;;) {

		uint32_t old_mask = maskirq(0xFFFFFFFF);
		uint16_t next;

		tx_pump();
		next = (tx_head + 1) % UART_FIFO_SIZE;

		if (next != tx_tail) {
			uart_tx_fifo[tx_head] = c;
			tx_head = next;
			tx_pump();
			reg_uart0_ier = (tx_head != tx_tail) ? 0b00000011 : 0b00000001;
			maskirq(old_mask);
			return;
		}

		// Full. Before the scheduler exists, IRQs do not reach
		// z_uart_irq (reg_kernel is still 0), so waitirq would
		// hang -- pump under the mask the way this used to, just
		// for that window.
		if (reg_kernel == 0 ||
			!(z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE)) {
			while (next == tx_tail) {
				tx_pump();
				next = (tx_head + 1) % UART_FIFO_SIZE;
			}
			uart_tx_fifo[tx_head] = c;
			tx_head = next;
			tx_pump();
			reg_uart0_ier = (tx_head != tx_tail) ? 0b00000011 : 0b00000001;
			maskirq(old_mask);
			return;
		}

		uart_tx_wait_pid = z_pid;
		z_procs[z_pid].wake_tick = 0;
		z_procs[z_pid].flags |= Z_PROC_FLAG_BLOCKED;
		// ring is non-empty, so THRE is already enabled -- the
		// next byte the 16550 takes will unblock us.
		maskirq(old_mask);
		k_proc_yield_blocked();

	}

}

void k_uart_wait_rx(void) {

	for (;;) {

		uint32_t old_mask = maskirq(0xFFFFFFFF);

		if (!uart_rx_fifo_empty()) {
			maskirq(old_mask);
			return;
		}

		if (reg_kernel == 0 ||
			!(z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE)) {
			maskirq(old_mask);
			return;
		}

		uart_rx_wait_pid = z_pid;
		z_procs[z_pid].wake_tick = 0;
		z_procs[z_pid].flags |= Z_PROC_FLAG_BLOCKED;
		maskirq(old_mask);
		k_proc_yield_blocked();

	}

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
