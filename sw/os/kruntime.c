/*
 * ZEITLOS OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Kernel runtime.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/zeitlos.h"
#include "../common/zsoc.h"	// Z_TICK_HZ
#include "kernel.h"
#include "uart.h"

bool term_echo = true;

void echo(void) { term_echo = true; }
void noecho(void) { term_echo = false; }

ssize_t _read(int file, void *ptr, size_t len)
{
   unsigned char *p = ptr;
	ssize_t i;
   for (i = 0; i < len; i++) {
		while (k_uart_rx_empty()) /* wait */;
		p[i] = (char)k_uart_getc();
		if (p[i] == 0x0a) return i + 1;
		if (p[i] == 0x0d) { p[i] = 0x0a; return i + 1; }
		if (term_echo) {
			while (k_uart_tx_full()) /* wait */;
			reg_uart0_data = p[i];
		}
   }
   return len;
}

int getch(void) {
	if (k_uart_rx_empty()) {
		return EOF;
	} else {
		return k_uart_getc();
	}
}

void readline(char *buf, int maxlen) {

	int c;
	int pl = 0;

	memset(buf, 0x00, maxlen + 1);

	while (1) {

		c = getch();

		if (c == EOF) {

			// Nothing in the FIFO. This used to `continue`, i.e. spin
			// at full speed for as long as the prompt sat idle -- and
			// pid 0 spinning is not free: the scheduler divides the
			// CPU between RUNNABLE processes, so an idle shell was
			// taking a full share out of whatever was in the
			// foreground. wm at least yielded a tick.
			//
			// z_uart_irq() (sw/os/uart.c) calls k_proc_unblock(0) as
			// soon as a byte lands, so this wakes on the very next
			// keystroke rather than at the end of the timeout.
			//
			// The timeout is a backstop, not the mechanism. It covers
			// a byte that arrived in the window between getch()
			// finding the FIFO empty and this call marking us blocked
			// -- k_proc_unblock() records that case in
			// Z_PROC_FLAG_WAKE so it cannot actually be lost, but a
			// bounded wait means a missed wakeup degrades to a small
			// latency rather than a hung console.
			// k_proc_wait() is the syscall handler and takes its
			// timeout in a z_obj_t, so it is called the same way
			// here as from the dispatch table -- the kernel is not
			// going through reg_kernel to reach its own function.
			{
				z_obj_t w;
				w.type = Z_UINT32;
				w.val.uint32 = Z_TICK_HZ / 4;
				k_proc_wait(&w);
			}

			continue;

		}

		if (c == CH_CR || c == CH_LF) {
			break;
		}
		else if (c == CH_BS || c == CH_DEL) {

			// The empty-line test belongs INSIDE this branch, not in
			// its condition. With `pl &&` up there, a backspace on an
			// empty line matched no branch above and fell through to
			// the printable case below, which echoed it and stored it
			// -- so the terminal walked its cursor back over the
			// prompt and 0x08 went into the command. Swallowing it
			// here is what makes backspace stop at column 0.
			if (!pl) continue;

			// pl is the INSERT position -- one past the last
			// character -- so the character to remove is at pl-1 and
			// the index has to move back with it. Clearing buf[pl]
			// and leaving pl alone erased the character on screen and
			// left it in the buffer: typing `ls`, backspace, `d`
			// showed "ld" and ran "lsd".
			pl--;
			buf[pl] = 0x00;
			printf(VT100_CURSOR_LEFT);
			printf(" ");
			printf(VT100_CURSOR_LEFT);
			fflush(stdout);
		}
		else if (c > 0) {
			if (term_echo) k_uart_putc((char)c);
			buf[pl++] = c;
		}

		if (pl == maxlen) break;

   }

	return;

}

ssize_t _write(int file, const void *ptr, size_t len)
{
	const unsigned char *p = ptr;
	for (int i = 0; i < len; i++) {
		if (p[i] == 0x0a) {
			while (k_uart_tx_full()) /* wait */;
			k_uart_putc(0x0d);
		}
		while (k_uart_tx_full()) /* wait */;
		k_uart_putc(p[i]);
	}
	return len;
}

int _open(const char *pathname, int flags) {
	errno = ENOENT;
	return -1;
}

int _close(int fd) {
	return 0;
}

int _fstat(int file, struct stat *st) {
	errno = ENOENT;
	return -1;
}

int _isatty(int fd) {
    return 1;
}

extern char _end;
static char *heap_end = 0;

void *_sbrk(int incr) {

    char *prev_heap_end;
    register char *sp asm ("sp");

    if (heap_end == 0) {
        heap_end = &_end;
    }
    prev_heap_end = heap_end;

    if (heap_end + incr > sp) {
        return (void *) -1; // out of memory
    }

    heap_end += incr;
    return (void *)prev_heap_end;
}

void _exit(int exit_status)
{
	asm volatile ("li a0, 0x00000000");
	asm volatile ("jr a0");
	__builtin_unreachable();
}

// -- picolibc stdio glue --
//
// Only compiled when building against picolibc (Debian/Ubuntu's
// gcc-riscv64-unknown-elf ships no C library of its own, so picolibc
// is the one paired with it -- see docs/toolchain.md). Newlib
// toolchains define stdin/stdout/stderr themselves and skip all of
// this; __PICOLIBC__ comes from picolibc.h, which stdio.h includes.
//
// The difference that makes this necessary: newlib provides the three
// standard streams and routes them through _write()/_read() below.
// picolibc's tinystdio does NOT -- it leaves stdin/stdout/stderr for
// the application to define, so without this every printf() in the
// tree fails to link with "undefined reference to `stdout`".
//
// Both hooks just forward to the same _write()/_read() the newlib
// build uses, so behaviour is identical either way. One byte at a
// time is not fast, but printf here ends up in a UART FIFO regardless
// and correctness matters more than buffering.
#ifdef __PICOLIBC__

static int z_picolibc_putc(char c, FILE *f) {
	(void)f;
	_write(1, &c, 1);
	return (unsigned char)c;
}

static int z_picolibc_getc(FILE *f) {
	char c;
	(void)f;
	if (_read(0, &c, 1) != 1) return EOF;
	return (unsigned char)c;
}

static FILE z_picolibc_stdio = FDEV_SETUP_STREAM(
	z_picolibc_putc, z_picolibc_getc, NULL, _FDEV_SETUP_RW);

FILE *const stdin = &z_picolibc_stdio;
FILE *const stdout = &z_picolibc_stdio;
FILE *const stderr = &z_picolibc_stdio;

#endif // __PICOLIBC__
