/*
 * zconsole.h -- the kernel console log and input, for sw/apps/console
 *
 * The kernel keeps the last Z_KLOG_SIZE bytes it sent to the serial
 * console (UART0) in a ring: everything printf() and kprint() print,
 * boot messages and crash reports included (sw/os/uart.c). Two
 * syscalls let an app see that output and type into the kernel shell,
 * so the console can be used from a term window on a machine with no
 * serial cable. See docs/console.md.
 *
 * Output a program writes straight to UART0's registers, rather than
 * through the kernel, does not reach the ring; neither does anything
 * the BIOS printed before the kernel started.
 */

#ifndef Z_CONSOLE_H
#define Z_CONSOLE_H

#include <stdint.h>
#include "zeitlos.h"

// Size of the kernel's ring. KEEP IN SYNC with K_KLOG_SIZE in
// sw/os/uart.c (a power of two).
#define Z_KLOG_SIZE 4096

// Registered name of the console port provider (sw/apps/console):
// "console0". term's CONSOLE button connects to it.
#define Z_CONSOLE_NAME "console"

typedef struct {
	uint8_t *buf;		// in: where to copy to
	uint32_t len;		// in: room in buf
	uint32_t pos;		// in: where to read from; out: where to read next
	uint32_t n;		// out: bytes copied
	uint32_t lost;		// out: bytes between pos and the oldest byte
				//      still held, which were overwritten
} z_klog_args_t;

// Copies console output from *pos onward into buf, and advances *pos.
//
// *pos is a count of bytes since boot, so every reader keeps its own and
// readers never disturb one another. Start at 0 for "everything still
// held". If *pos has already been overwritten, reading starts at the
// oldest byte still held and *lost says how many were skipped. Returns
// the number of bytes copied; 0 means nothing new.
static inline uint32_t z_klog_read(uint32_t *pos, uint8_t *buf, uint32_t len,
		uint32_t *lost) {
	z_klog_args_t a;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a.buf = buf; a.len = len; a.pos = *pos; a.n = 0; a.lost = 0;
	k(Z_SYS_KLOG_READ, (uint32_t *)&a, 0);
	*pos = a.pos;
	if (lost) *lost = a.lost;
	return a.n;
}

// Feeds bytes to the kernel shell as if typed on the serial console.
// Returns how many were taken (fewer if its input buffer is full).
static inline uint32_t z_console_input(const uint8_t *buf, uint32_t len) {
	z_klog_args_t a;
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	a.buf = (uint8_t *)buf; a.len = len; a.pos = 0; a.n = 0; a.lost = 0;
	k(Z_SYS_CONSOLE_INPUT, (uint32_t *)&a, 0);
	return a.n;
}

#endif
