/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zco -- coroutines. See zco.h.
 */

#include "zco.h"

#include <string.h>

static zco_t *cur;

zco_t *zco_current(void) { return cur; }

#if defined(__riscv)

// zco_switch(save, load): the callee-saved registers out to `save`, in
// from `load`, and return -- into whatever `load` was doing.
void zco_switch(zco_regs_t *save, zco_regs_t *load);
__asm__(
	".text\n"
	".globl zco_switch\n"
	".type zco_switch, @function\n"
	"zco_switch:\n"
	"	sw ra, 0(a0)\n"
	"	sw sp, 4(a0)\n"
	"	sw s0, 8(a0)\n"
	"	sw s1, 12(a0)\n"
	"	sw s2, 16(a0)\n"
	"	sw s3, 20(a0)\n"
	"	sw s4, 24(a0)\n"
	"	sw s5, 28(a0)\n"
	"	sw s6, 32(a0)\n"
	"	sw s7, 36(a0)\n"
	"	sw s8, 40(a0)\n"
	"	sw s9, 44(a0)\n"
	"	sw s10, 48(a0)\n"
	"	sw s11, 52(a0)\n"
	"	lw ra, 0(a1)\n"
	"	lw sp, 4(a1)\n"
	"	lw s0, 8(a1)\n"
	"	lw s1, 12(a1)\n"
	"	lw s2, 16(a1)\n"
	"	lw s3, 20(a1)\n"
	"	lw s4, 24(a1)\n"
	"	lw s5, 28(a1)\n"
	"	lw s6, 32(a1)\n"
	"	lw s7, 36(a1)\n"
	"	lw s8, 40(a1)\n"
	"	lw s9, 44(a1)\n"
	"	lw s10, 48(a1)\n"
	"	lw s11, 52(a1)\n"
	"	ret\n"
	".size zco_switch, .-zco_switch\n"
);

// Where a new coroutine starts: s0 holds the zco_t.
static void entry(void) __attribute__((noreturn, used));
static void entry(void) {
	zco_t *co = cur;
	co->fn(co->arg);
	co->done = true;
	co->running = false;
	cur = NULL;
	zco_switch(&co->co, &co->caller);
	for (;;) ;				// never resumed once done
}

void zco_init(zco_t *co, zco_fn fn, void *arg, void *stack, size_t bytes) {
	uintptr_t top = ((uintptr_t)stack + bytes) & ~(uintptr_t)15;
	memset(co, 0, sizeof(*co));
	co->fn = fn;
	co->arg = arg;
	co->stack = stack;
	co->bytes = bytes;
	co->co.r[0] = (uint32_t)(uintptr_t)entry;		// ra: where the first switch returns
	co->co.r[1] = (uint32_t)(top - 16);				// sp
}

#else	// the build machine: ucontext

static void entry(void) {
	zco_t *co = cur;
	co->fn(co->arg);
	co->done = true;
	co->running = false;
	cur = NULL;
	swapcontext(&co->co.uc, &co->caller.uc);
}

void zco_init(zco_t *co, zco_fn fn, void *arg, void *stack, size_t bytes) {
	memset(co, 0, sizeof(*co));
	co->fn = fn;
	co->arg = arg;
	co->stack = stack;
	co->bytes = bytes;
	getcontext(&co->co.uc);
	co->co.uc.uc_stack.ss_sp = stack;
	co->co.uc.uc_stack.ss_size = bytes;
	co->co.uc.uc_link = NULL;
	makecontext(&co->co.uc, entry, 0);
}

static void zco_switch(zco_regs_t *save, zco_regs_t *load) {
	swapcontext(&save->uc, &load->uc);
}

#endif

bool zco_resume(zco_t *co) {
	if (co->done || cur) return !co->done;
	co->started = true;
	co->running = true;
	cur = co;
	zco_switch(&co->caller, &co->co);
	// back here when it yields or ends
	cur = NULL;
	return !co->done;
}

void zco_yield(void) {
	zco_t *co = cur;
	if (!co) return;
	cur = NULL;
	zco_switch(&co->co, &co->caller);
	cur = co;
}

void zco_abandon(zco_t *co) {
	co->done = true;
	co->running = false;
	if (cur == co) cur = NULL;
}
