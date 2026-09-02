#ifndef PROF_H
#define PROF_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * prof -- cycle-level phase timing for sw/apps/play.
 *
 * Built only when PLAY_PROF=1. Everything below compiles to nothing
 * otherwise, including the call sites, so a production build carries
 * no cost and no risk of the instrument perturbing the thing it is
 * measuring.
 *
 * -- why this exists, and why the previous instrument was useless --
 *
 * play's first diagnostics timed each phase with z_uptime_ticks().
 * Two things are wrong with that, and both of them matter more than
 * they sound:
 *
 *   1. It is a SYSCALL. z_uptime_ticks() (sw/common/zeitlos.c) builds
 *      a z_obj_t and calls through the reg_kernel trampoline into the
 *      kernel's dispatch table. It costs hundreds of cycles to ask a
 *      question about something that may itself cost hundreds of
 *      cycles, and play called it nine times per loop pass.
 *
 *   2. Its resolution is ONE KTIMER TICK -- 1.37ms, 65664 cycles at
 *      48MHz. Every phase worth optimising here is shorter than that.
 *      A reading of "9" means "somewhere between 8 and 10 ticks of
 *      WALL time", which for a preempted process is mostly a
 *      measurement of the other two processes.
 *
 * So the previous numbers could rank phases and nothing more.
 *
 * -- what replaces it --
 *
 * `rdcycle` and `rdinstret`. One instruction each, no syscall, no bus
 * transaction, 1-cycle resolution, and -- the part that matters given
 * where this project's budget is tight -- **they cost no BRAM and no
 * LUTs, because they are already built.**
 *
 *   picorv32:  ENABLE_COUNTERS and ENABLE_COUNTERS64 both default to 1
 *              (rtl/cpu/picorv32/picorv32.v) and rtl/sysctl.v does not
 *              override either.
 *   zeitlos32: rdcycle/rdcycleh/rdinstret/rdinstreth are implemented
 *              explicitly; see that file's own header.
 *
 * They are emitted as raw instruction words rather than as the
 * `csrr` mnemonic -- see the note above prof_cyc() for why, which is
 * a binutils-version story rather than a preference.
 *
 * ** IF ANY BOARD EVER SETS ENABLE_COUNTERS(0), rdcycle becomes an
 *    ILLEGAL INSTRUCTION and this traps. ** There is no way to probe
 *    for that from software without already having executed it. That
 *    is the entire reason this is behind a build flag rather than
 *    always on.
 *
 * -- rdcycle counts the WORLD, not this process --
 *
 * It is a free-running counter on sys_clk. It keeps counting while the
 * scheduler is running wm, or sh, or the idle loop. So the elapsed
 * cycles across a phase include however much of somebody else's
 * timeslice happened to land in the middle of it.
 *
 * That is not fixable from userspace and it does not need to be.
 * A KTIMER slice is 65664 cycles and most of these phases are far
 * shorter, so the majority of samples are clean and the contaminated
 * ones are contaminated by a LOT. Hence:
 *
 *   min   the cheapest observed call. This is the real, uninterrupted
 *         cost of the phase, and it is the number to optimise
 *         against.
 *   avg   the same measurement with preemption folded in. Useful only
 *         next to min: a large avg/min ratio means the phase is being
 *         interrupted often, not that it is expensive.
 *   ipc   instructions retired per cycle, from rdinstret over the same
 *         window.
 *
 * ipc is the one that decides what KIND of fix is needed, and it is
 * the reason rdinstret is sampled at all:
 *
 *   ipc near 0.17  -- normal for this SOC (docs/muldiv.md measured
 *                     exactly that on the real core). The phase is
 *                     executing a lot of instructions. Fix by doing
 *                     less work: a cheaper algorithm.
 *   ipc well below -- the phase is STALLED, not busy. Waiting on the
 *                     bus, on the blitter, on a peripheral ack. No
 *                     amount of algorithmic cleverness helps; the fix
 *                     is hardware or fewer transactions.
 *
 * Telling those two apart is exactly the question "should this be
 * solved in gateware", and no wall-clock timer can answer it.
 */

#include <stdint.h>

/* Phases. Keep in step with prof_names[] in prof.c. */
enum {
    P_RENDER = 0,   /* stream_render() as a whole (INCLUDES P_DECODE) */
    P_DECODE,       /* adec_decode() inside it -- nested, subtract */
    P_PUSH,         /* the z_audio_push_unchecked() loop */
    P_PUMP,         /* fs_read_chunk() */
    P_MSG,          /* drain_messages() */
    P_TEXT,         /* ui_step(), i.e. one row of glyphs */
    P_SCOPE,        /* scope_step(), i.e. three blitter fills */
    P_WAIT,         /* z_proc_wait() -- how long a yield really costs */
    P_DUMP,         /* this profiler's own printf, measured honestly */
    P_COUNT
};

#if PLAY_PROF

typedef struct {
    uint32_t cyc;       /* summed elapsed cycles */
    uint32_t instr;     /* summed retired instructions */
    uint32_t calls;
    uint32_t min;       /* cheapest call -- the uncontaminated cost */
    uint32_t max;
} prof_slot_t;

extern prof_slot_t prof_slot[P_COUNT];
extern uint32_t prof_out_frames;   /* output frames produced */
extern uint32_t prof_src_frames;   /* source frames consumed */
extern uint32_t prof_passes;       /* main loop iterations */

/* "MIXER" or "FIFO" -- printed as the first thing in every report.
 * See prof.c on why this is not left to be inferred. */
extern const char *prof_path_name;

/*
 * The counters are read as RAW INSTRUCTION WORDS rather than as the
 * `csrr` mnemonic, and that is not obscurantism -- it is the only
 * form that assembles on every toolchain this tree supports.
 *
 * binutils 2.36 split the CSR instructions out of the RISC-V base ISA
 * into the Zicsr extension. On anything that recent, -march=rv32im no
 * longer permits `csrr` and the assembler says so:
 *
 *   Error: unrecognized opcode `csrr a0,0xC00', extension `zicsr'
 *   required
 *
 * The obvious fix is -march=$(ARCH)_zicsr. It works, and it was not
 * taken, for two reasons. The 2018-era toolchain picorv32's own build
 * instructions pin -- which sw/common/arch.mk explicitly still
 * supports -- rejects `_zicsr` as an unknown extension, so the flag
 * has to be probed for rather than set, and the probe belongs in
 * arch.mk if it belongs anywhere. And arch.mk's whole point is that
 * the ISA string is decided in ONE place and must match
 * rtl/boards.vh's `CPU_MUL/`CPU_DIV; a profiler is a poor reason to
 * put a second, conditionally-different ISA string next to it.
 *
 * .word needs neither. The encoding is fixed by the ISA and cannot
 * drift:
 *
 *   csrrs rd, csr, x0     [31:20] csr  [19:15] rs1=0
 *                         [14:12] 010  [11:7] rd  [6:0] 1110011
 *
 *   rdcycle   a0  ->  csr 0xC00, rd = x10  ->  0xC0002573
 *   rdinstret a0  ->  csr 0xC02, rd = x10  ->  0xC0202573
 *
 * rd is baked into the word, so the destination cannot be left to the
 * register allocator -- hence the local register variable pinning it
 * to a0. That is the same construct every libc uses for its syscall
 * stubs and is a documented GCC feature, not a trick.
 *
 * Check it landed with:  grep -c rdcycle sw/apps/play/play.dasm
 * objdump disassembles the word back to the mnemonic, so a correct
 * build shows `rdcycle` and `rdinstret` even though neither appears
 * in the source.
 */
#define PROF_RDCYCLE_WORD   0xC0002573u   /* rdcycle   a0 */
#define PROF_RDINSTRET_WORD 0xC0202573u   /* rdinstret a0 */

static inline uint32_t prof_cyc(void) {
    register uint32_t v __asm__("a0");
    __asm__ volatile (".word 0xC0002573" : "=r"(v));
    return v;
}

static inline uint32_t prof_ins(void) {
    register uint32_t v __asm__("a0");
    __asm__ volatile (".word 0xC0202573" : "=r"(v));
    return v;
}

/*
 * Begin/end are a macro pair with their own locals in a block scope,
 * so nesting P_DECODE inside P_RENDER works without a stack. The two
 * halves must be in the same scope; that is the only rule.
 *
 * Order matters: read instret BEFORE cycles on entry and AFTER on
 * exit, so the counter pair brackets the region rather than
 * overlapping it. The instructions belonging to the read itself land
 * outside the window in both directions, which keeps a short phase
 * from measuring mostly the measurement.
 */
#define PROF_BEGIN(p) \
    uint32_t prof_i0_##p = prof_ins(); \
    uint32_t prof_c0_##p = prof_cyc()

#define PROF_END(p) do { \
    uint32_t _c = prof_cyc() - prof_c0_##p; \
    uint32_t _i = prof_ins() - prof_i0_##p; \
    prof_slot_t *_s = &prof_slot[p]; \
    _s->cyc += _c; _s->instr += _i; _s->calls++; \
    if (!_s->min || _c < _s->min) _s->min = _c; \
    if (_c > _s->max) _s->max = _c; \
} while (0)

#define PROF_ADD_OUT(n)  (prof_out_frames += (n))
#define PROF_ADD_SRC(n)  (prof_src_frames += (n))
#define PROF_PASS()      (prof_passes++)
#define PROF_PATH(s)     (prof_path_name = (s))

/* Print one interval's report to the console and clear the counters.
 * `elapsed_ticks` is the KTIMER interval it covers, `out_hz` the DAC
 * rate, so everything can be normalised per output frame. */
void prof_report(uint32_t elapsed_ticks, uint32_t out_hz);

void prof_reset(void);

#else   /* !PLAY_PROF -- compiles to nothing */

#define PROF_BEGIN(p)    ((void)0)
#define PROF_END(p)      ((void)0)
#define PROF_ADD_OUT(n)  ((void)0)
#define PROF_ADD_SRC(n)  ((void)0)
#define PROF_PASS()      ((void)0)
#define PROF_PATH(s)     ((void)0)
#define prof_report(a,b) ((void)0)
#define prof_reset()     ((void)0)

#endif

#endif
