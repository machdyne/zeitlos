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
		if (c == EOF) continue;

		if (c == CH_CR || c == CH_LF) {
			break;
		}
		else if (pl && (c == CH_BS || c == CH_DEL)) {
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
