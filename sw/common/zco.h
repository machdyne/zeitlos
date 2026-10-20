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
 *
 * -- the stack is a fixed buffer --
 *
 * Nothing grows it, and nothing stops a call chain from running off
 * its bottom into whatever lies below. So zco_init() fills it with a
 * pattern, and every time the coroutine hands back, zco checks that
 * the lowest ZCO_GUARD bytes still hold it. If not, the coroutine is
 * finished on the spot -- `overflow` set, never resumed again -- and
 * zco_resume() returns false as for an ending: the caller treats it as
 * a failure. That catches the overflow at the first yield after it,
 * which is too late to undo what it wrote, but early enough that a
 * stack too small shows up in a test rather than as a corruption
 * somewhere else, much later. zco_stack_used() says how deep it went,
 * to size a stack from a test run.
 *
 * Both read the pattern, so both can be fooled by a frame that
 * reserves a large array and writes only part of it: the call chain
 * can step over the guard, and the high-water mark reads low. Give a
 * coroutine a stack well above what zco_stack_used() reports -- twice
 * is a sensible rule -- rather than just enough. sw/apps/posix/job.c
 * adds a slack zone below its stack and checks that too.
 *
 * -- where the stack lives --
 *
 * If the coroutine calls malloc(), its stack must not come from
 * malloc(): newlib's _sbrk() refuses to grow the heap past the current
 * stack pointer, so with the stack inside the heap every allocation in
 * the coroutine fails. Put it in a static buffer, an arena, or the
 * main loop's own frame (posix). A coroutine that allocates nothing
 * may have its stack anywhere.
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
	bool		overflow;	// it ran off its stack (see above): done, failed
};

#define ZCO_GUARD	128		// bytes at the stack's bottom that must stay untouched

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

// The most of its stack it has used so far, in bytes (the high-water
// mark of the pattern zco_init() laid down).
size_t zco_stack_used(const zco_t *co);

#endif
