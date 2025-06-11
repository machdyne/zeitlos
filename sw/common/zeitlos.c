/*
 * ZEITLOS OS
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Application runtime.
 *
 * This is compiled into each app.
 *
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zeitlos.h"

bool term_echo = true;
bool term_escape = false;

void echo(void) { term_echo = true; }
void noecho(void) { term_echo = false; }

//

bool uart_rx_empty(void);
bool uart_tx_full(void);
void uart_putc(char c);
int16_t uart_getc(void);

bool uart_rx_empty(void) {
	uint32_t *(*z_kernel_ptr)(uint32_t, uint32_t *, uint32_t) =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_RX_EMPTY, (uint32_t *)&obj, 0);
	return (bool)obj.val.int32;
}

bool uart_tx_full(void) {
	uint32_t *(*z_kernel_ptr)(uint32_t, uint32_t *, uint32_t) =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;

//	reg_leds = 0xff;
//	print_hex32(&z_kernel_ptr);
//	while(1);  // XXX


	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_RX_EMPTY, (uint32_t *)&obj, 0);
	return (bool)obj.val.int32;
}

void uart_putc(char c) {
	uint32_t *(*z_kernel_ptr)(uint32_t, uint32_t *, uint32_t) =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;
	z_obj_t obj;
	obj.val.int32 = c;
	z_kernel_ptr(Z_SYS_UART_RX_EMPTY, (uint32_t *)&obj, 0);
}

int16_t uart_getc(void) {
	uint32_t *(*z_kernel_ptr)(uint32_t, uint32_t *, uint32_t) =
		(uint32_t *(*)(uint32_t, uint32_t *, uint32_t))(uintptr_t)reg_kernel;
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_RX_EMPTY, (uint32_t *)&obj, 0);
	return (int16_t)obj.val.int32;
}

void rt_delay() {
   volatile static int x, y;
   for (int i = 0; i < 10000; i++) {
      x += y;
   }
}

void print_hex_digit(uint8_t val) {
    if (val < 10) {
        reg_uart0_data = '0' + val;
    } else {
        reg_uart0_data = 'A' + (val - 10);
    }
    rt_delay();  // Small delay so UART has time to send the character
}

void print_hex32(uint32_t val) {
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0xF;
        print_hex_digit(nibble);
    }
}

void print_ptr_hex(const void *ptr) {
    print_hex32((uint32_t)(uintptr_t)ptr);
}

//
//

ssize_t _read(int file, void *ptr, size_t len)
{
   unsigned char *p = ptr;
	ssize_t i;
	int c;
   for (i = 0; i < len; i++) {

		// wait for character
		while (uart_rx_empty());
		
		p[i] = (char)c;
		if (p[i] == 0x0a) return i + 1;
		if (p[i] == 0x0d) { p[i] = 0x0a; return i + 1; }
		if (term_echo) {
			// wait for free buffer space
			while (uart_tx_full()) /* wait */;
			uart_putc(p[i]);
		}
   }
   return len;
}

// non-blocking getc
int getch(void) {
	if (uart_rx_empty()) {
		return EOF;
	} else {
		return (char)uart_getc();
	}
}

void readline(char *buf, int maxlen) {

	int c;
	int pl = 0;

	memset(buf, 0x00, maxlen + 1);

	while (1) {

		c = getch();

		if (c == CH_CR || c == CH_LF) {
			break;
		}
		else if (pl && (c == CH_BS || c == CH_DEL)) {
			pl--;
			buf[pl] = 0x00;
			printf(VT100_CURSOR_LEFT);
			printf(" ");
			printf(VT100_CURSOR_LEFT);
			fflush(stdout);
		}
		else if (c > 0) {
			if (term_echo) uart_putc(c);
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
			while (uart_tx_full()) /* wait */;
			uart_putc(0x0d);
		}
		while (uart_tx_full()) /* wait */;
		uart_putc(p[i]);
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

// --

#define Z_IS_OK(obj)   ((obj) && (obj)->type == Z_RETVAL && (obj)->value.uint32 == 0)
#define Z_IS_FAIL(obj)  ((obj) && (obj)->type == Z_RETVAL && (obj)->value.uint32 == 1)
