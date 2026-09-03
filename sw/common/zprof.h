#ifndef ZPROF_H
#define ZPROF_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zprof -- cycle-level phase timing.
 *
 * This is sw/apps/play/prof.h's mechanism, lifted into sw/common so
 * that sw/common/zimg.c can be instrumented too. play's copy is left
 * alone deliberately: it carries audio-specific accounting (output
 * frames, source frames, the MIXER/FIFO path label) and its own
 * report format, and rewriting a working profiler in a working app to
 * share a header with a new one is a change with no upside and a real
 * chance of breaking play's numbers. If the two are ever consolidated
 * this is the copy to keep, but that is not this change.
 *
 * Built only when Z_PROF is 1. Everything below compiles to nothing
 * otherwise, including the call sites, so a production build carries
 * no cost and no risk of the instrument perturbing what it measures.
 *
 * -- why not z_uptime_ticks() --
 *
 * Because it cannot work, for two reasons play's header sets out in
 * full and which are repeated here only in summary: it is a SYSCALL
 * (a z_obj_t built and dispatched through the reg_kernel trampoline,
 * hundreds of cycles to ask a question about something that may cost
 * hundreds of cycles), and its resolution is one KTIMER tick --
 * 1.37ms, 65664 cycles at 48MHz. Every phase worth optimising is far
 * shorter than that.
 *
 * -- the instrument is not free, so bracket coarsely --
 *
 * A BEGIN/END pair is four counter reads plus a read-modify-write of
 * the slot. On this SOC that read-modify-write is the expensive part:
 * rtl/cache.v caches INSTRUCTION FETCHES ONLY, so every access to the
 * slot array in .bss goes to main memory at ~11 cycles per word on
 * SDRAM and ~63 on PSRAM.
 *
 * So bracket a ROW or a BLOCK, never a pixel. Around a 640-pixel
 * dither row the instrument amortises to well under a cycle per
 * pixel; around a single pixel it would cost more than the arithmetic
 * it is timing, and the resulting profile would be a picture of the
 * profiler.
 *
 * -- rdcycle counts the WORLD, not this process --
 *
 * It is a free-running counter on sys_clk and keeps counting while
 * the scheduler runs wm, or sh, or the idle loop, so elapsed cycles
 * across a phase include any of someone else's timeslice that landed
 * in the middle. Not fixable from userspace and not worth fixing: a
 * KTIMER slice is 65664 cycles, most phases here are far shorter, so
 * most samples are clean and the contaminated ones are contaminated
 * by a lot. Hence `min` alongside `cyc`:
 *
 *   min   the cheapest observed call -- the real, uninterrupted cost,
 *         and the number to optimise against.
 *   avg   the same with preemption folded in. A large avg/min ratio
 *         means the phase is interrupted often, not that it is
 *         expensive.
 *   ipc   instructions retired per cycle, over the same window.
 *
 * ipc is what decides WHICH KIND of fix a phase needs, and is the
 * reason rdinstret is sampled at all:
 *
 *   ipc near 0.17   normal for this SOC (docs/muldiv.md measured
 *                   exactly that). The phase is executing a lot of
 *                   instructions. Fix by doing less work.
 *   ipc well below  the phase is STALLED, not busy -- waiting on the
 *                   bus, the blitter, or a peripheral. Algorithmic
 *                   cleverness will not help; fewer memory
 *                   transactions or gateware will.
 *
 * That distinction is exactly the question "should this go in
 * hardware", and no wall-clock timer can answer it.
 *
 * ** IF ANY BOARD EVER SETS ENABLE_COUNTERS(0), rdcycle becomes an
 *    ILLEGAL INSTRUCTION and this traps. ** There is no way to probe
 *    for that from software without having already executed it, which
 *    is why this sits behind a build flag rather than being always
 *    on. picorv32 defaults ENABLE_COUNTERS/ENABLE_COUNTERS64 to 1 and
 *    rtl/sysctl.v does not override them; zeitlos32 implements the
 *    counters explicitly.
 */

#include <stdint.h>

#ifndef Z_PROF
#define Z_PROF 0
#endif

#if Z_PROF

#define Z_PROF_MAX 12

typedef struct {
	uint32_t	cyc;	// summed elapsed cycles
	uint32_t	instr;	// summed retired instructions
	uint32_t	calls;
	uint32_t	min;	// cheapest call -- the uncontaminated cost
	uint32_t	max;
} z_prof_slot_t;

extern z_prof_slot_t z_prof_slot[Z_PROF_MAX];

/*
 * The counters are read as RAW INSTRUCTION WORDS rather than as the
 * `csrr` mnemonic. This is not obscurantism; it is the only form that
 * assembles on every toolchain this tree supports.
 *
 * binutils 2.36 split the CSR instructions out of the RISC-V base ISA
 * into the Zicsr extension, so on anything that recent -march=rv32im
 * no longer permits `csrr`:
 *
 *   Error: unrecognized opcode `csrr a0,0xC00', extension `zicsr'
 *   required
 *
 * The obvious fix, -march=$(ARCH)_zicsr, is rejected as an unknown
 * extension by the 2018-era toolchain sw/common/arch.mk still
 * supports -- so it would have to be probed for, and arch.mk's whole
 * point is that the ISA string is decided in ONE place and must match
 * rtl/boards.vh. A profiler is a poor reason to put a second,
 * conditionally-different ISA string next to it.
 *
 * .word needs neither. The encoding is fixed by the ISA:
 *
 *   csrrs rd, csr, x0   [31:20] csr  [19:15] rs1=0
 *                       [14:12] 010  [11:7] rd  [6:0] 1110011
 *
 *   rdcycle   a0  ->  csr 0xC00, rd = x10  ->  0xC0002573
 *   rdinstret a0  ->  csr 0xC02, rd = x10  ->  0xC0202573
 *
 * rd is baked into the word, so the destination cannot be left to the
 * register allocator -- hence the local register variable pinning it
 * to a0, the same construct every libc uses for its syscall stubs.
 *
 * Check it landed with:  grep -c rdcycle sw/apps/view/view.dasm
 */
static inline uint32_t z_prof_cyc(void) {
	register uint32_t v __asm__("a0");
	__asm__ volatile (".word 0xC0002573" : "=r"(v));
	return v;
}

static inline uint32_t z_prof_ins(void) {
	register uint32_t v __asm__("a0");
	__asm__ volatile (".word 0xC0202573" : "=r"(v));
	return v;
}

/*
 * Begin/end are a macro pair with their own locals in a block scope,
 * so nesting one phase inside another works without a stack. The two
 * halves must be in the same scope; that is the only rule.
 *
 * Order matters: read instret BEFORE cycles on entry and AFTER on
 * exit, so the pair brackets the region rather than overlapping it.
 * The instructions belonging to the read itself land outside the
 * window in both directions, which keeps a short phase from measuring
 * mostly the measurement.
 */
#define Z_PROF_BEGIN(p) \
	uint32_t zprof_i0_##p = z_prof_ins(); \
	uint32_t zprof_c0_##p = z_prof_cyc()

#define Z_PROF_END(p) do { \
	uint32_t _c = z_prof_cyc() - zprof_c0_##p; \
	uint32_t _i = z_prof_ins() - zprof_i0_##p; \
	z_prof_slot_t *_s = &z_prof_slot[p]; \
	_s->cyc += _c; _s->instr += _i; _s->calls++; \
	if (!_s->min || _c < _s->min) _s->min = _c; \
	if (_c > _s->max) _s->max = _c; \
} while (0)

void z_prof_reset(void);

#else	/* !Z_PROF -- compiles to nothing */

#define Z_PROF_BEGIN(p)  ((void)0)
#define Z_PROF_END(p)    ((void)0)
#define z_prof_reset()   ((void)0)

#endif

#endif
