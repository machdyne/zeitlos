/*
 * Zeitlos OS
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

void print_hex32(uint32_t val);

bool uart_rx_empty(void) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_RX_EMPTY, (uint32_t *)&obj, 0);
	return (bool)obj.val.int32;
}

bool uart_tx_full(void) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_TX_FULL, (uint32_t *)&obj, 0);
	return (bool)obj.val.int32;
}

void uart_putc(char c) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	obj.val.int32 = c;
	z_kernel_ptr(Z_SYS_UART_PUTC, (uint32_t *)&obj, 0);
}

int16_t uart_getc(void) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_UART_GETC, (uint32_t *)&obj, 0);
	return (int16_t)obj.val.int32;
}

// pops the next queued raw USB HID keyboard event (see sw/os/hid.c),
// or -1 if none is pending. non-blocking, same "pop or -1" shape as
// uart_getc() above. the returned value, if >= 0, is packed as:
// bit0 = pressed(1)/released(0), bits8:1 = HID usage code,
// bits16:9 = modifier byte -- see sw/os/hid.c's HID_EVENT() macro
// and sw/common/zkbd.h for turning the usage code into a keysym.
int32_t hid_read_key(void) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	z_kernel_ptr(Z_SYS_HID_READ_KEY, (uint32_t *)&obj, 0);
	return obj.val.int32;
}

// -- messaging --

z_rv z_msg_send(z_msg_t *msg) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_MSG_SEND, (uint32_t *)msg, 0);
	return rv->val.uint32;
}

z_rv z_msg_read(z_msg_t *msg) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_MSG_READ, (uint32_t *)msg, 0);
	return rv->val.uint32;
}

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	z_msg_t msg;
	msg.to = to;
	msg.subject = subject;
	msg.tag = tag;
	msg.obj = obj;
	return z_msg_send(&msg);
}

z_rv z_msg_wait(z_msg_t *msg, uint32_t subject, uint32_t tag) {
	while (1) {
		if (z_msg_read(msg) == Z_OK) {
			if (msg->subject == subject && msg->tag == tag)
				return Z_OK;
			// not the message we're waiting for -- discard and keep going
		}
	}
}

uint32_t z_uptime_ticks(void) {
	z_obj_t obj = {0};
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_kernel_ptr(Z_SYS_UPTIME, (uint32_t *)&obj, 0);
	return obj.val.uint32;
}

// busy-waits for at least `ms` milliseconds, using z_uptime_ticks()
// (the KTIMER IRQ's own ~732Hz tick counter, sw/os/kernel.c) rather
// than an arbitrary `for (volatile int i = 0; i < N; i++);` spin loop
// -- the kind of ad-hoc delay that's shown up repeatedly across this
// codebase (real-hardware timing tests, wait_for_redraw_done()'s own
// small per-iteration spin, and others), each one tuned by feel
// against whatever clock speed/compiler/optimization level happened
// to be in use at the time, with no actual relationship to wall-clock
// time. This one does: pass a real millisecond count and get
// (approximately) that long, regardless of CPU speed or how the
// caller was compiled.
//
// "approximately": ~732Hz isn't an exact round number (48MHz driving
// a 16-bit hardware counter, rtl/sysctl.v's own rtc_ctr -- see that
// file's own comment), so any ms-to-ticks conversion has some
// built-in imprecision from the hardware itself, not something this
// function can fix. Rounds UP (ticks = ceil(ms * 732 / 1000)) rather
// than down, specifically so a caller asking for "at least N ms"
// actually gets at least that much, never slightly less -- the
// opposite rounding direction would silently under-deliver by up to
// one tick's worth of time (~1.4ms) on every call, which compounds
// badly for a caller doing many small delays in a loop.
//
// A "delay" here means exactly what it means everywhere else in this
// codebase: busy-spin (this process keeps its CPU slice, doing
// nothing useful) until enough ticks have passed -- there's no
// sleep/yield-until-woken primitive in this kernel a process could
// use instead. Fine for the short, occasional delays this is meant
// for; not a substitute for actually blocking on a message/event if
// the wait could be long or unpredictable (z_msg_wait()/
// z_msg_wait_timeout(), zeitlos.h, do that instead).
void delay_ms(uint32_t ms) {
	uint32_t ticks = ((uint64_t)ms * 732 + 999) / 1000;
	uint32_t start = z_uptime_ticks();
	while (z_uptime_ticks() - start < ticks);
}

// -- PID name registry -- see zeitlos.h --

bool z_pid_register(const char *basename, char *out, uint32_t outlen) {

	if (outlen == 0) return false;

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)basename;

	// success/failure is read from the VALUE at the returned pointer
	// (rv->val.uint32 == Z_OK), not by comparing the pointer itself
	// against this file's own &z_ok -- z_ok/z_fail are `static`
	// (zobj.h), so the kernel's compiled copy and this app's compiled
	// copy are different objects at different addresses; only
	// z_msg_send()'s existing precedent (`return rv->val.uint32;`,
	// no pointer comparison at all) reads correctly across that
	// boundary. The actual output (the assigned full name) comes back
	// via `obj` itself, mutated in place by the kernel handler -- see
	// k_pid_register()'s comment in sw/os/pidreg.c for why that's the
	// convention here rather than a returned data object.
	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_PID_REGISTER, (uint32_t *)&obj, 0);
	if (rv->val.uint32 != Z_OK || obj.type != Z_STR || !obj.val.str) return false;

	uint32_t i;
	for (i = 0; i < outlen - 1 && obj.val.str[i]; i++)
		out[i] = obj.val.str[i];
	out[i] = 0;

	return true;

}

bool z_pid_lookup(const char *name, uint32_t *pid) {

	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)name;

	z_obj_t *rv = (z_obj_t *)z_kernel_ptr(Z_SYS_PID_LOOKUP, (uint32_t *)&obj, 0);
	if (rv->val.uint32 != Z_OK || obj.type != Z_UINT32) return false;

	*pid = obj.val.uint32;
	return true;

}

uint32_t z_getpid(void) {
	z_obj_t obj = {0};
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_kernel_ptr(Z_SYS_GETPID, (uint32_t *)&obj, 0);
	return obj.val.uint32;
}

// launches a new process from a named file on the FAT filesystem
// (e.g. "term", "gpu3d" -- bare name, no path/extension, same as
// typing `run term` at the kernel shell). Returns the new pid, or 0
// on failure (file not found, or no free process slot -- see
// Z_SYS_PROC_RUN in sw/os/kernel.c for the full writeup). The new
// process is started immediately; there's no separate "start" step
// for callers of this wrapper.
uint32_t z_proc_run(const char *name) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)name;
	z_kernel_ptr(Z_SYS_PROC_RUN, (uint32_t *)&obj, 0);
	return obj.val.uint32;
}

// kills another process by pid -- see zeitlos.h's own comment.
void z_proc_kill(uint32_t pid) {
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_obj_t obj;
	obj.type = Z_UINT32;
	obj.val.uint32 = pid;
	z_kernel_ptr(Z_SYS_PROC_KILL, (uint32_t *)&obj, 0);
}

// --

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

ssize_t _read(int fd, void *ptr, size_t len)
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

ssize_t _write(int fd, const void *ptr, size_t len)
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

int _fstat(int fd, struct stat *st) {
	errno = ENOENT;
	return -1;
}

int _isatty(int fd) {
	if (fd >= 0 && fd <= 2) return(1);
	return(0);
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
	z_kernel_ptr_t z_kernel_ptr = (z_kernel_ptr_t)(uintptr_t)(reg_kernel);
	z_kernel_ptr(Z_SYS_EXIT, NULL, 0);
/*
	asm volatile ("li a0, 0x00000000");
	asm volatile ("jr a0");
	__builtin_unreachable();
*/
}

// --

#define Z_IS_OK(obj)   ((obj) && (obj)->type == Z_RETVAL && (obj)->value.uint32 == 0)
#define Z_IS_FAIL(obj)  ((obj) && (obj)->type == Z_RETVAL && (obj)->value.uint32 == 1)
