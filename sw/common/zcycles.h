#ifndef ZCYCLES_H
#define ZCYCLES_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The free-running cycle counter, in one place.
 *
 * rdcycle counts sys_clk and keeps counting through context switches,
 * so it is a WALL clock rather than a per-process one. That is exactly
 * what the bit-bang libraries want -- a bus edge happens in real time
 * whether or not this process is on the CPU -- and exactly what a
 * profiler has to compensate for. See sw/common/zprof.h's own header
 * for the profiling side of that distinction.
 *
 * -- Why this header exists --
 *
 * There were three identical copies of this function in the tree
 * (sw/common/zrng.c, sw/os/kernel.c, sw/os/sh.c) before the bit-bang
 * libraries wanted a fourth. Those three are left alone deliberately:
 * two of them are kernel-side and one is inside an entropy source
 * whose whole design depends on reading the counter in a very
 * particular way, and consolidating working code is not what a new
 * feature should be doing. This is the place for the fourth and any
 * later one.
 *
 * -- IF ANY BOARD EVER SETS ENABLE_COUNTERS(0), THIS TRAPS --
 *
 * rdcycle becomes an illegal instruction, which on this SOC is not a
 * clean fault but IRQ 1, which nothing handles. There is no way to
 * probe for that from software without having already executed it.
 *
 * Currently safe on every board and not a live risk: picorv32 defaults
 * ENABLE_COUNTERS and ENABLE_COUNTERS64 to 1 and rtl/sysctl.v
 * overrides neither, and rtl/cpu/zeitlos32 implements the counters
 * explicitly. This is a note for whoever is one day tempted to save
 * the LUTs.
 *
 * -- The non-RISC-V branch --
 *
 * Not portability for its own sake. It is what lets sw/common/zi2c.c
 * and sw/common/zspi.c -- which are protocol state machines, and the
 * most worth unit-testing of anything in sw/common -- be compiled and
 * run on the build machine against a simulated bus.
 *
 * The alternative, and the convention this tree had until now (see
 * sw/common/tests/test_gfx_region.c), is to LIFT the code under test
 * into the test file. That works for a page of rectangle arithmetic.
 * It does not work for an I2C master: the copy would be large, it
 * would drift, and a test that passes against a stale copy of the
 * state machine is worse than no test.
 *
 * So the host branch returns a counter that simply advances on every
 * read. Delays become meaningless off-target -- there is no bus and
 * nothing to be slow for -- but every TIMEOUT path stays reachable,
 * which is the part a test actually needs to exercise.
 *
 * It carries no test hook. An earlier version did -- a weak function
 * the host branch would call, so a test could model a device on the
 * other end of the pins -- and it turned out not to be needed:
 * sw/common/tests/test_bitbang.c traps the MMIO stores themselves with
 * mprotect and a single-step handler, which is both more faithful (it
 * sees every store, in order, rather than only those bracketed by a
 * delay) and leaves this file with nothing in it that exists for a
 * test.
 */

#include <stdint.h>

#if defined(__riscv)

static inline uint32_t z_cycles(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

#else

// Host builds only -- see this file's header. The step is arbitrary;
// it just has to be non-zero so that a bounded wait terminates.
static inline uint32_t z_cycles(void) {
	static uint32_t t;
	t += 64;
	return t;
}

#endif

#endif
