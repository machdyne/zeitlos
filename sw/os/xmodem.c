/*
 * Zeitlos OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * XMODEM/CRC receiver.
 *
 * Adapted from the Blaustahl firmware's xmodem.c (same copyright
 * holder). The protocol handling is that implementation's, which is
 * known good against real senders; what changed here is the platform
 * layer:
 *
 *   cdc_getchar()          -> k_uart_rx_empty()/k_uart_getc()
 *   cdc_putchar_reliable() -> k_uart_tx_full()/k_uart_putc()
 *   absolute_time_t        -> z_uptime_ticks() deltas at Z_TICK_HZ
 *   internal malloc()      -> caller-supplied buffer (see xmodem.h)
 *
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "../common/zeitlos.h"
#include "../common/zsoc.h"	// Z_TICK_HZ
#include "uart.h"
#include "xmodem.h"

#define X_SOH   0x01
#define X_STX   0x02
#define X_EOT   0x04
#define X_ACK   0x06
#define X_NAK   0x15
#define X_CAN   0x18
#define X_CTRLZ 0x1a

// 128 for SOH blocks, 1024 for STX ("XMODEM-1K") blocks. Senders pick
// per block and may mix them, so this has to hold the larger.
#define XM_BLOCK_MAX 1024u

/*
 * Timeouts, in ticks.
 *
 * z_uptime_ticks() is the only clock available here and it runs at
 * Z_TICK_HZ (~732Hz, sw/common/zsoc.h), so ~1.37ms of resolution.
 * XMODEM's timeouts are all whole seconds, so that is plenty, but the
 * conversion has to happen somewhere -- doing it here, on constants,
 * means it folds at compile time and no division ever runs on a build
 * without the M extension (see sw/common/arch.mk).
 */
#define XM_MS_TO_TICKS(ms) (((uint32_t)(ms) * Z_TICK_HZ) / 1000u)

// Handshake budget: a HUMAN-operated transfer, not two programs
// starting in lockstep. The user types `xmf`, then has to switch to
// their terminal program and find its own send menu, which routinely
// takes well over a minute. 60 * 3s = 3 minutes before giving up.
// Short timeouts here read as "xmodem is broken" when the real problem
// is that the firmware gave up before the human caught up.
#define XM_HANDSHAKE_TRIES 60
#define XM_HANDSHAKE_TICKS XM_MS_TO_TICKS(3000)

#define XM_BYTE_TICKS   XM_MS_TO_TICKS(1000)	// within a block
#define XM_HEADER_TICKS XM_MS_TO_TICKS(3000)	// waiting for the next block
#define XM_FLUSH_TICKS  XM_MS_TO_TICKS(200)		// draining trailing bytes

/*
 * Static rather than on the stack: a block has to be buffered whole
 * and CRC-checked before any of it is committed, since a bad block
 * must not be allowed to corrupt the good bytes already received. At
 * 1KB that is more than the shell's stack wants to carry, and fs.c
 * has already been bitten once by deep stacks reaching into the FatFs
 * work area (see the f_stat() note in fs_list_dir()).
 */
static uint8_t xm_block[XM_BLOCK_MAX];

static uint16_t crc16_update(uint16_t crc, uint8_t byte) {

	crc = (uint16_t)(crc ^ ((uint16_t)byte << 8));

	for (int i = 0; i < 8; i++) {
		if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
		else crc = (uint16_t)(crc << 1);
	}

	return crc;

}

static void xm_putch(uint8_t c) {
	// k_uart_tx_full() pumps the ring on the way past, so this always
	// makes progress on its own and cannot deadlock waiting for the
	// UART interrupt (see the tx_pump() comment in uart.c).
	while (k_uart_tx_full()) /* wait */;
	k_uart_putc((char)c);
}

/*
 * Returns the next byte as 0..255, or -1 if `timeout_ticks` elapsed
 * first.
 *
 * k_uart_rx_empty() is the ONLY safe test for "no byte available".
 * k_uart_getc() returns int16_t and sign-extends the byte it pops
 * (plain char is signed on RISC-V, and nothing here builds with
 * -funsigned-char), so a legitimate 0xff data byte comes back as -1 --
 * exactly the value it uses for an empty FIFO. Text transfers never
 * notice; binary ones hit it constantly. xfer.c's mygetch() is careful
 * about this for the same reason.
 */
static int xm_getch(uint32_t timeout_ticks) {

	uint32_t start = z_uptime_ticks();

	do {
		if (!k_uart_rx_empty())
			return (int)(uint8_t)k_uart_getc();
	} while (z_uptime_ticks() - start < timeout_ticks);

	return -1;

}

// drain trailing bytes (e.g. a sender's second CAN) so they don't leak
// into the next shell prompt
static void xm_flush(void) {
	while (xm_getch(XM_FLUSH_TICKS) != -1) /* discard */;
}

static void xm_cancel(void) {
	xm_putch(X_CAN);
	xm_putch(X_CAN);
	xm_flush();
}

uint32_t xmodem_recv(uint32_t addr_ptr, uint32_t capacity,
	xmodem_result_t *result) {

	uint8_t *dst = (uint8_t *)(uintptr_t)addr_ptr;

	uint32_t total = 0;
	uint32_t last_block_size = 0;
	uint8_t expected_block = 1;

	xmodem_result_t res = XMODEM_TIMEOUT;
	int c = -1;

	// Establish the transfer: request CRC mode ('C') and keep asking
	// until the sender answers with a block header or we run out of
	// patience. The stray 'C's appear as garbage in the user's
	// terminal until their sender starts -- that is normal, and it is
	// how the sender knows we want CRC-16 rather than the original
	// checksum.
	for (int tries = 0; tries < XM_HANDSHAKE_TRIES; tries++) {
		xm_putch('C');
		c = xm_getch(XM_HANDSHAKE_TICKS);
		if (c == X_SOH || c == X_STX || c == X_EOT) break;
		if (c == X_CAN) { xm_flush(); res = XMODEM_CANCELLED; goto done; }
		c = -1;
	}

	if (c == -1) { res = XMODEM_TIMEOUT; goto done; }

	while (1) {

		if (c == X_EOT) {
			xm_putch(X_ACK);
			res = XMODEM_OK;
			break;
		}

		if (c == X_CAN) {
			xm_flush();
			res = XMODEM_CANCELLED;
			goto done;
		}

		if (c != X_SOH && c != X_STX) {
			// unexpected byte where a block header was expected
			xm_putch(X_NAK);
			c = xm_getch(XM_HEADER_TICKS);
			if (c == -1) { res = XMODEM_TIMEOUT; goto done; }
			continue;
		}

		uint32_t block_size = (c == X_STX) ? 1024u : 128u;

		int blk      = xm_getch(XM_BYTE_TICKS);
		int blk_comp = xm_getch(XM_BYTE_TICKS);

		uint16_t crc = 0;
		bool timed_out = false;

		for (uint32_t i = 0; i < block_size; i++) {
			int b = xm_getch(XM_BYTE_TICKS);
			if (b == -1) { timed_out = true; break; }
			xm_block[i] = (uint8_t)b;
			crc = crc16_update(crc, (uint8_t)b);
		}

		int crc_hi = timed_out ? -1 : xm_getch(XM_BYTE_TICKS);
		int crc_lo = timed_out ? -1 : xm_getch(XM_BYTE_TICKS);

		bool block_ok = !timed_out && blk != -1 && blk_comp != -1 &&
			crc_hi != -1 && crc_lo != -1 &&
			((blk + blk_comp) == 255) &&
			((uint16_t)((crc_hi << 8) | crc_lo) == crc);

		if (!block_ok) {
			xm_putch(X_NAK);
			c = xm_getch(XM_HEADER_TICKS);
			if (c == -1) { res = XMODEM_TIMEOUT; goto done; }
			continue;
		}

		if ((uint8_t)blk == expected_block) {

			if (total + block_size > capacity) {
				xm_cancel();
				res = XMODEM_TOO_LARGE;
				goto done;
			}

			memcpy(&dst[total], xm_block, block_size);
			total += block_size;
			last_block_size = block_size;
			expected_block++;

		} else if ((uint8_t)blk != (uint8_t)(expected_block - 1)) {

			// Neither the block we asked for nor a retransmit of the
			// one before it (our ACK having been lost). A conforming
			// sender cannot produce this, so the stream is not what
			// we think it is -- give up rather than ACK it, which
			// would silently drop or duplicate a block and write a
			// corrupt file that looks like it transferred fine.
			//
			// This is the one deliberate divergence from the
			// Blaustahl original, which ACKs any CRC-valid block
			// whose number isn't the expected one.
			xm_cancel();
			res = XMODEM_SEQUENCE;
			goto done;

		}
		// else: duplicate retransmit of the block we already have --
		// ACK it again without re-appending. Standard receiver
		// behaviour; keeps the sender's window in sync.

		xm_putch(X_ACK);
		c = xm_getch(XM_HEADER_TICKS);
		if (c == -1) { res = XMODEM_TIMEOUT; goto done; }

	}

	/*
	 * Trim the final block's padding.
	 *
	 * XMODEM has no length field: the last block is padded out to the
	 * block boundary, classically with CTRL-Z, sometimes with NUL.
	 * Guessing is the only option, so keep the guess as narrow as
	 * possible -- only ever look inside the last block received, since
	 * 0x1a and 0x00 are perfectly ordinary bytes earlier in a binary.
	 *
	 * This is still a guess, and it is why xmf is not the right way to
	 * upload an executable: a .data section that genuinely ends in
	 * zeros gets trimmed too, and fs_exec_info() derives the data
	 * length from the file size. Use `xf` for those. (The real fix
	 * would be a data length in the ZEXE header -- see
	 * sw/common/zexec.h and tools/mkexec.py, which currently record
	 * bss size but not data size.)
	 */
	if (res == XMODEM_OK && last_block_size) {

		uint32_t block_start = total - last_block_size;

		while (total > block_start &&
				(dst[total - 1] == X_CTRLZ || dst[total - 1] == 0x00))
			total--;

	}

done:
	if (result) *result = res;
	return total;

}

const char *xmodem_strerror(xmodem_result_t result) {

	switch (result) {
		case XMODEM_OK:			return "ok";
		case XMODEM_CANCELLED:	return "cancelled";
		case XMODEM_TOO_LARGE:	return "file too large";
		case XMODEM_TIMEOUT:	return "timed out";
		case XMODEM_SEQUENCE:	return "block sequence error";
	}

	return "unknown error";

}
