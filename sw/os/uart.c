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
#include "../common/zconsole.h"
#include "../common/zsoc.h"	// game mode, for the panic screen
#include "../common/zfont.h"	// z_font_5x8, for the panic screen

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

// Set while the kernel is handling an interrupt, and for good by a
// panic. A full TX ring then drains by polling instead of blocking the
// current process: inside the interrupt handler the UART interrupt that
// would wake it cannot be taken, so blocking there hung the machine. Any
// printf from interrupt context could hit that -- crash reports, and the
// scheduler's own messages when it cleans up a process.
volatile bool k_uart_polled;

// -- console log --
//
// The last K_KLOG_SIZE bytes sent to UART0, so the console can be seen
// without a serial cable: sw/apps/console serves it to term as the
// "console0" port (docs/console.md). k_klog_pos counts bytes since boot;
// a byte's slot is its count modulo the size, so there is no separate
// head and tail to keep consistent, and each reader keeps its own
// position. Wraps after 4GB of output, which only confuses a reader
// that has been disconnected for that long.
//
// Recording costs a store and an increment per byte, inside
// k_uart_putc()'s existing masked section. It lives in .bss, but
// kernel.bin is padded through .bss, so all 4KB count against the
// kernel's 256KB limit (docs/console.md).
#define K_KLOG_SIZE 4096	// KEEP IN SYNC with Z_KLOG_SIZE (zconsole.h)
static uint8_t k_klog[K_KLOG_SIZE];
static volatile uint32_t k_klog_pos;

// Caller holds the IRQ mask (k_uart_putc() does).
static inline void k_klog_put_locked(uint8_t c) {
	k_klog[k_klog_pos & (K_KLOG_SIZE - 1)] = c;
	k_klog_pos++;
}

// For kprint() and other direct UART0 writers.
void k_klog_putc(uint8_t c) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	k_klog_put_locked(c);
	maskirq(old_mask);
}
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
			k_klog_put_locked((uint8_t)c);
			tx_pump();
			reg_uart0_ier = (tx_head != tx_tail) ? 0b00000011 : 0b00000001;
			maskirq(old_mask);
			return;
		}

		// Full. Before the scheduler exists, IRQs do not reach
		// z_uart_irq (reg_kernel is still 0), so waitirq would
		// hang -- pump under the mask the way this used to, just
		// for that window.
		if (k_uart_polled || reg_kernel == 0 ||
			!(z_procs[z_pid].flags & Z_PROC_FLAG_ACTIVE)) {
			while (next == tx_tail) {
				tx_pump();
				next = (tx_head + 1) % UART_FIFO_SIZE;
			}
			uart_tx_fifo[tx_head] = c;
			tx_head = next;
			k_klog_put_locked((uint8_t)c);
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

// -- console log and input syscalls (sw/common/zconsole.h) --

// Z_SYS_KLOG_READ. At most 128 bytes per call, copied with interrupts
// masked so the ring cannot move underneath (~20us); callers loop.
z_obj_t *k_klog_read(z_obj_t *args) {
	z_klog_args_t *a = (z_klog_args_t *)args;
	if (!a || !a->buf) return &z_fail;
	// k_user_ok(): the kernel must not write where the app has no memory (docs/mpu.md)
	if (!k_user_ok(a->buf, a->len)) return &z_fail;
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	uint32_t end = k_klog_pos;
	uint32_t start = a->pos;
	uint32_t avail = end - start;		// wrap-safe
	a->lost = 0;
	if (avail > K_KLOG_SIZE) {
		if ((int32_t)avail < 0) {	// a position from the future
			start = end;
			avail = 0;
		} else {
			a->lost = avail - K_KLOG_SIZE;
			start = end - K_KLOG_SIZE;
			avail = K_KLOG_SIZE;
		}
	}
	uint32_t n = avail;
	if (n > a->len) n = a->len;
	if (n > 128) n = 128;
	for (uint32_t i = 0; i < n; i++)
		a->buf[i] = k_klog[(start + i) & (K_KLOG_SIZE - 1)];
	a->pos = start + n;
	a->n = n;
	maskirq(old_mask);
	return &z_ok;
}

// Z_SYS_CONSOLE_INPUT: bytes into the same RX ring the UART interrupt
// fills, then the same wake-up, so the kernel shell cannot tell them
// from typing on the serial console.
z_obj_t *k_console_input(z_obj_t *args) {
	z_klog_args_t *a = (z_klog_args_t *)args;
	if (!a || !a->buf) return &z_fail;
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	uint32_t n = 0;
	while (n < a->len) {
		uint16_t next = (rx_head + 1) % UART_FIFO_SIZE;
		if (next == rx_tail) break;		// full: take what fits
		uart_rx_fifo[rx_head] = a->buf[n++];
		rx_head = next;
	}
	if (n && uart_rx_wait_pid != ~0u) {
		k_proc_unblock(uart_rx_wait_pid);
		uart_rx_wait_pid = ~0u;
	}
	a->n = n;
	maskirq(old_mask);
	return &z_ok;
}

// -- panic support --

// Send everything still queued, by polling. For a panic, which halts
// with interrupts masked: the TX interrupt would never come, and the
// end of the report would stay in the ring unsent.
void k_uart_flush(void) {
	while (tx_head != tx_tail) tx_pump();
}

static inline uint8_t k_rev8(uint8_t b) {
	b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
	b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
	b = (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
	return b;
}

// The panic screen: the last K_PANIC_ROWS rows of the console log, the
// report at the bottom of them, drawn straight into VRAM at the top of
// the screen, so a panic can be read on a machine with no serial cable.
// Twenty rows, not the sixty that fit in 480 lines: a full screen of
// text did not all fit on a real display, and it is the end that
// matters. The rest of the screen is cleared.
//
// Deliberately primitive, because the system is broken: CPU stores
// only, no blitter, no wm, no font loaded into the GPU. Each glyph of
// z_font_5x8 sits in an 8-pixel cell so a glyph row is one byte store
// (80x60 characters). The framebuffer is 1bpp with the leftmost pixel in
// the lowest bit (zgfx.c's z_fb_set_pixel()); the font stores rows
// leftmost-pixel-highest, hence k_rev8(). Game mode is switched off so
// the text is not shown through a 320x240 viewport.
#define K_PANIC_VRAM ((volatile uint8_t *)0x20000000)
#define K_PANIC_COLS 80
#define K_PANIC_ROWS 20

void k_klog_panic_screen(void) {
	const z_font_t *f = &z_font_5x8;

	if (z_game_present()) reg_socctl_game = 0;

	for (uint32_t i = 0; i < (640u * 480u) / 8u; i++) K_PANIC_VRAM[i] = 0;

	// Back up from the end to where the last K_PANIC_ROWS screen rows
	// begin, counting a long line as the rows it wraps to -- so a wrapped
	// line cannot push the report itself off the bottom.
	uint32_t end = k_klog_pos;
	uint32_t held = (end < K_KLOG_SIZE) ? end : K_KLOG_SIZE;
	uint32_t oldest = end - held;
	uint32_t q = end;
	if (q != oldest && k_klog[(q - 1) & (K_KLOG_SIZE - 1)] == '\n') q--;
	uint32_t start = q, len = 0;
	int rows = 0;
	for (;;) {
		bool at_line = (q == oldest) ||
			k_klog[(q - 1) & (K_KLOG_SIZE - 1)] == '\n';
		if (at_line) {
			int r = len ? (int)((len + K_PANIC_COLS - 1) / K_PANIC_COLS) : 1;
			if (rows + r > K_PANIC_ROWS) break;
			rows += r;
			start = q;
			len = 0;
			if (q == oldest) break;
		} else if (k_klog[(q - 1) & (K_KLOG_SIZE - 1)] != '\r') {
			len++;
		}
		q--;
	}

	int row = 0, col = 0;
	for (uint32_t p = start; p != end && row < K_PANIC_ROWS; p++) {
		uint8_t c = k_klog[p & (K_KLOG_SIZE - 1)];
		if (c == '\r') continue;
		if (c == '\n') { row++; col = 0; continue; }
		if (col >= K_PANIC_COLS) {
			row++;
			col = 0;
			if (row >= K_PANIC_ROWS) break;
		}
		if (c < f->first || c > f->last) c = '?';
		const uint8_t *g = f->glyphs + (c - f->first) * f->h;
		for (int j = 0; j < 8; j++)
			K_PANIC_VRAM[(row * 8 + j) * K_PANIC_COLS + col] = k_rev8(g[j]);
		col++;
	}
}
