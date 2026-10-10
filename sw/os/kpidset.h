#ifndef K_PIDSET_H
#define K_PIDSET_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A set of pids, one bit each, sized from Z_PROCS_MAX: one word at 32,
 * two at 64, eight at 256. Everything in the kernel that keeps a per-pid
 * set uses this rather than a bare uint32_t and `1u << pid`, so that
 * raising Z_PROCS_MAX is a change to that constant and nothing here.
 * The places that still assume 32 or fewer pids are listed in
 * docs/kernel.md, "Raising Z_PROCS_MAX past 32".
 *
 * Z_PROCS_MAX must be defined before this is included (kernel.h does).
 * Header-only, so sw/os/tests/test_pidset.c can check it on the build
 * machine at several sizes.
 */

#include <stdint.h>
#include <stdbool.h>

#define K_PIDSET_WORDS	((Z_PROCS_MAX + 31) / 32)

typedef struct { uint32_t w[K_PIDSET_WORDS]; } k_pidset_t;

static inline void k_pidset_put(volatile k_pidset_t *s, uint32_t pid, bool on) {
	uint32_t bit = 1u << (pid & 31u);
	if (on) s->w[pid >> 5] |= bit;
	else s->w[pid >> 5] &= ~bit;
}

static inline bool k_pidset_has(const volatile k_pidset_t *s, uint32_t pid) {
	return (s->w[pid >> 5] >> (pid & 31u)) & 1u;
}

static inline void k_pidset_clear(volatile k_pidset_t *s) {
	for (uint32_t i = 0; i < K_PIDSET_WORDS; i++) s->w[i] = 0;
}

static inline void k_pidset_and(k_pidset_t *d, const volatile k_pidset_t *a,
	const volatile k_pidset_t *b) {
	for (uint32_t i = 0; i < K_PIDSET_WORDS; i++) d->w[i] = a->w[i] & b->w[i];
}

static inline uint32_t k_pidset_count(const volatile k_pidset_t *s) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < K_PIDSET_WORDS; i++) {
		uint32_t m = s->w[i];
		while (m) { m &= m - 1u; n++; }
	}
	return n;
}

// The first pid after `cur`, wrapping round, that is in `a` or `b`
// (cur itself last); Z_PROCS_MAX if neither has any. Round-robin
// order, exactly as the slot walk it replaces. One word at a time, so
// at Z_PROCS_MAX 32 it is a mask and a count of trailing zeros.
static inline uint32_t k_pidset_next2(uint32_t cur, const volatile k_pidset_t *a,
	const volatile k_pidset_t *b) {
	uint32_t start = (cur + 1u < Z_PROCS_MAX) ? cur + 1u : 0;
	for (uint32_t n = 0; n <= K_PIDSET_WORDS; n++) {
		uint32_t wi = ((start >> 5) + n) % K_PIDSET_WORDS;
		uint32_t m = a->w[wi] | b->w[wi];
		// The first word looked at: only bits at or above `start`. The
		// pass that comes back round to it (n == WORDS) takes the rest.
		if (n == 0) m &= ~((1u << (start & 31u)) - 1u);
		if (m) {
			uint32_t pid = (wi << 5) + (uint32_t)__builtin_ctz(m);
			if (pid < Z_PROCS_MAX) return pid;
		}
	}
	return Z_PROCS_MAX;
}

#endif
