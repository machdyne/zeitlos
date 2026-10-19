#ifndef ZCO_H
#define ZCO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zco -- a function run a slice at a time from an app's main loop: a
 * coroutine, with a stack of its own. docs/responsive.md.
 *
 * -- why --
 *
 * An app's main loop has to keep reading its messages: wm waits for
 * redraw acks, keys must be answered, and the person must be able to
 * stop anything long. Some work is one long call that cannot be cut
 * into steps without rewriting it -- an image decoder is a loop over
 * the whole file. zco runs such a call on its own stack, and the call
 * hands control back (zco_yield()) from inside its callbacks: when it
 * needs input the main loop has not read yet, or when its slice of
 * time is used. The main loop resumes it when it has nothing else to
 * do. Nothing in the call changes.
 *
 * Stopping is instant: the main loop simply never resumes it again
 * (zco_abandon()). That is safe only for work that holds nothing but
 * memory the caller owns -- the decoders in zimg allocate nothing --
 * and does no I/O and sends no messages of its own: those stay in the
 * main loop, which hands the work its input.
 *
 * -- the machine --
 *
 * On the board a switch saves and loads the callee-saved registers
 * (ra, sp, s0-s11); an interrupt saves registers to the kernel's own
 * area and runs on its own stack, so a stack in the arena is as good
 * as the process's. On the build machine (the host tests) it is
 * ucontext. One coroutine runs at a time; it may not start another.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct zco zco_t;
typedef void (*zco_fn)(void *arg);

#if defined(__riscv)
typedef struct { uint32_t r[14]; } zco_regs_t;		// ra, sp, s0..s11
#else
#include <ucontext.h>
typedef struct { ucontext_t uc; } zco_regs_t;
#endif

struct zco {
	zco_regs_t	co, caller;
	zco_fn		fn;
	void		*arg;
	void		*stack;
	size_t		bytes;
	bool		started, done, running;
};

// Readies fn(arg) to run on `stack` (bytes, 16-aligned or not: it is
// aligned here). Nothing runs until zco_resume().
void zco_init(zco_t *co, zco_fn fn, void *arg, void *stack, size_t bytes);

// Runs it until it yields or returns. Returns true while there is more
// to do (it yielded), false once it has returned.
bool zco_resume(zco_t *co);

// From inside: back to whoever resumed it.
void zco_yield(void);

// The coroutine running now, or NULL (in the main loop).
zco_t *zco_current(void);

// Forget it, wherever it stands: it is never resumed. Its stack is the
// caller's to free.
void zco_abandon(zco_t *co);

#endif
