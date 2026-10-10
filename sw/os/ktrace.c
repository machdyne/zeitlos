/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The scheduler trace: a fixed ring of timestamped events from the
 * paths a message round trip takes -- send, unblock, block, yield,
 * interrupt entry, switch. docs/kernel.md, "Scheduler trace".
 *
 * Built only with `make KTRACE=1` (sw/os/Makefile). It exists to answer
 * questions like "why does an idle round trip take a tick", which
 * reasoning about the code had not answered; a normal kernel carries
 * the syscall entry (which fails) and nothing else.
 *
 *   ipcbench arms it around a few round trips (Z_SYS_KTRACE), or
 *   `ktrace arm` / `ktrace off` by hand; `ktrace` prints it.
 *
 * Recording stops when the ring is full rather than wrapping: the
 * question is always "what happened after I armed it".
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"

#ifdef KTRACE

#define KT_N 192

typedef struct {
	uint32_t cyc;
	uint16_t ev;
	uint16_t pid;
	uint32_t a;
} kt_ent_t;

// Absolute addressing (.bss): k_trace() runs on the syscall path with
// the caller's gp -- docs/kernel.md, "The gp hazard".
static __attribute__((section(".bss"))) kt_ent_t kt_buf[KT_N];
static __attribute__((section(".bss"))) volatile uint32_t kt_n;
static __attribute__((section(".bss"))) volatile uint32_t kt_on;

static inline uint32_t kt_cycles(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

void k_trace(uint32_t ev, uint32_t a) {
	if (!kt_on) return;
	uint32_t old = maskirq(0xffffffffu);
	if (kt_n < KT_N) {
		kt_ent_t *e = &kt_buf[kt_n++];
		e->cyc = kt_cycles();
		e->ev = (uint16_t)ev;
		e->pid = (uint16_t)z_pid;
		e->a = a;
	} else {
		kt_on = 0;
	}
	maskirq(old);
}

static void kt_arm(bool on) {
	uint32_t old = maskirq(0xffffffffu);
	if (on) kt_n = 0;
	kt_on = on ? 1 : 0;
	maskirq(old);
}

z_obj_t *k_ktrace(z_obj_t *args) {
	if (!args || args->type != Z_UINT32) return &z_fail;
	kt_arm(args->val.uint32 != 0);
	return &z_ok;
}

static const char *const kt_names[] = {
	"?", "send", "unblock", "block", "noblock", "yield", "irq",
	"switch", "keep", "resched", "defer", "tick", "handoff", "step",
};

static void kt_dump(void) {
	uint32_t n = kt_n;
	if (!n) { printf("ktrace: empty (arm it, then run something)\n"); return; }
	printf("ktrace: %lu events%s; cycles from the first, and from the previous\n",
		(unsigned long)n, kt_on ? " (still armed)" : "");
	uint32_t t0 = kt_buf[0].cyc, prev = t0;
	for (uint32_t i = 0; i < n; i++) {
		kt_ent_t *e = &kt_buf[i];
		const char *nm = e->ev < sizeof(kt_names) / sizeof(kt_names[0]) ? kt_names[e->ev] : "?";
		printf("%4lu %9lu %+8ld  pid %2u  %-8s %lx\n",
			(unsigned long)i, (unsigned long)(e->cyc - t0),
			(long)(e->cyc - prev), (unsigned)e->pid, nm, (unsigned long)e->a);
		prev = e->cyc;
	}
}

void k_ktrace_cmd(const char *arg) {
	if (arg && !strcmp(arg, "arm")) { kt_arm(true); printf("ktrace: armed\n"); }
	else if (arg && !strcmp(arg, "off")) { kt_arm(false); printf("ktrace: stopped\n"); }
	else kt_dump();
}

#else

z_obj_t *k_ktrace(z_obj_t *args) {
	(void)args;
	return &z_fail;
}

#endif
