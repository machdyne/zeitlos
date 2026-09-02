/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * prof -- see prof.h. Built only when PLAY_PROF=1.
 */

#include "prof.h"

#if PLAY_PROF

#include <stdio.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"

prof_slot_t prof_slot[P_COUNT];
uint32_t prof_out_frames;
uint32_t prof_src_frames;
uint32_t prof_passes;
const char *prof_path_name = "?";

static const char *prof_names[P_COUNT] = {
    "render",   /* includes decode */
    " decode",  /* nested inside render -- leading space says so */
    "push",
    "pump",
    "msg",
    "text",
    "scope",
    "wait",
    "dump"
};

void prof_reset(void) {
    memset(prof_slot, 0, sizeof(prof_slot));
    prof_out_frames = 0;
    prof_src_frames = 0;
    prof_passes = 0;
}

/*
 * This process's share of the CPU over the interval, in tenths of a
 * percent.
 *
 * From z_proc_list()'s cpu_ticks, which the KTIMER handler increments
 * for whichever process it interrupted (sw/os/kernel.h). Sampled at
 * 732Hz, so it is meaningless over 100ms and reliable over a second --
 * which is exactly the interval this report covers.
 *
 * Without it the cycle counts below cannot be turned into anything
 * actionable. rdcycle says how many cycles a phase spanned; this says
 * how many cycles this process was ever ALLOWED, and the ratio of the
 * two is the only honest statement of how close to the ceiling we are.
 */
#define PROF_PROCS 12
static uint32_t prof_prev_cpu;
static int prof_have_prev;

static uint32_t cpu_share_permille(uint32_t elapsed_ticks) {

    z_proc_info_t procs[PROF_PROCS];
    uint32_t n, i, trunc = 0, self = z_getpid();
    uint32_t now = 0, d;

    if (!elapsed_ticks) return 0;

    n = z_proc_list(procs, PROF_PROCS, &trunc);
    for (i = 0; i < n; i++)
        if (procs[i].pid == self) { now = procs[i].cpu_ticks; break; }
    if (i == n) return 0;

    if (!prof_have_prev) {
        prof_prev_cpu = now;
        prof_have_prev = 1;
        return 0;
    }

    d = now - prof_prev_cpu;      /* unsigned, correct across wrap */
    prof_prev_cpu = now;

    if (d > elapsed_ticks) d = elapsed_ticks;
    return (d * 1000u) / elapsed_ticks;
}

/*
 * One line per phase.
 *
 *   min      cheapest observed call, in cycles. THE number. This is
 *            the phase's real cost with no preemption in it.
 *   avg      mean call, preemption included. Only meaningful beside
 *            min: avg >> min means "often interrupted", not
 *            "expensive".
 *   c/of     min-cost cycles attributable to one OUTPUT frame --
 *            min * calls / output frames. Everything in this app
 *            scales with the output rate, so this is the column that
 *            makes phases comparable to each other and to the budget.
 *   ipc      instructions retired per cycle, x100. ~17 is normal for
 *            this SOC (docs/muldiv.md measured 0.172 on the real
 *            core). Much below that means STALLED, not busy -- and
 *            that distinction decides whether the fix is software or
 *            gateware.
 */
void prof_report(uint32_t elapsed_ticks, uint32_t out_hz) {

    uint32_t i;
    uint32_t share = cpu_share_permille(elapsed_ticks);
    uint32_t of = prof_out_frames ? prof_out_frames : 1;
    uint32_t clean_total = 0;
    uint32_t elapsed_cyc;

    /* Cycles this process could have used: 48MHz (or whatever the
     * audio block reports the system clock to be -- the one place a
     * real CLK_HZ is readable from an app) for its share of the
     * interval. */
    uint32_t clk = reg_audio_clkhz ? reg_audio_clkhz : 48000000u;
    uint32_t avail;

    /* elapsed_ticks / 732 seconds of wall time, in cycles. Done as
     * (clk/732)*ticks to stay inside 32 bits. */
    elapsed_cyc = (clk / 732u) * elapsed_ticks;
    avail = (elapsed_cyc / 1000u) * share;

    /* The playback path, first line, every report. A profile of the
     * software path and a profile of the mixer path are different
     * measurements of different programs, and telling them apart
     * afterwards from the phase list alone requires knowing which
     * phases belong to which -- which is exactly the deduction that
     * should not be left to the reader. */
    printf("\n-- play prof [%s]: %lu ticks, %lu passes, out %lu, src %lu --\n",
        prof_path_name,
        (unsigned long)elapsed_ticks, (unsigned long)prof_passes,
        (unsigned long)prof_out_frames, (unsigned long)prof_src_frames);
    /* Frames actually produced per second, against the DAC rate.
     * This is THE headline: 100 means real time, anything less is the
     * factor by which playback is slow. */
    printf("   realtime %lu%% of %luHz   cpu share %lu.%lu%%\n",
        (unsigned long)(out_hz
            ? ((prof_out_frames * 732u) / (elapsed_ticks ? elapsed_ticks : 1))
              * 100u / out_hz
            : 0),
        (unsigned long)out_hz,
        (unsigned long)(share / 10), (unsigned long)(share % 10));
    printf("   phase      calls      min      avg     c/of  ipc  %%avail\n");

    for (i = 0; i < P_COUNT; i++) {

        prof_slot_t *s = &prof_slot[i];
        uint32_t avg, cof, ipc, pct, clean;

        if (!s->calls) continue;

        avg = s->cyc / s->calls;

        /*
         * -- cost is SHARE-SCALED WALL TIME, not min * calls --
         *
         * The first version of this used the cheapest observed call
         * times the call count, on the theory that the minimum is the
         * one sample with no preemption in it. That theory has two
         * holes and both of them bit on real hardware.
         *
         * It assumes a phase is longer-lived than a timeslice never
         * happens. `decode` spans a hundred slices, so every sample
         * is contaminated and there is no clean one to find -- which
         * is how this reported 135% of available, an impossible
         * figure.
         *
         * Worse, it assumes CALLS ARE HOMOGENEOUS. They are not:
         * feed_fifo() renders whatever space the FIFO happens to have,
         * so stream_render() is called with anything from 0 to 256
         * frames. The minimum is a call that did NO WORK -- 646
         * cycles against an average of 843,688 -- and multiplying
         * that by the call count accounted for 14% of a budget that
         * was in fact 98% spent. It said the app was comfortably
         * inside budget while playing at a fifth of real time.
         *
         * Total wall cycles scaled by the measured CPU share has
         * neither problem. It over-attributes only to the extent that
         * the share itself is mis-sampled, and the totals now close
         * to within 2%.
         *
         * min and avg are still shown, because their RATIO is a
         * useful diagnostic -- a large one means either heavy
         * preemption or wildly uneven call sizes, and which of those
         * it is usually tells you something.
         */
        clean = share ? (s->cyc / 1000u) * share : 0;
        cof = clean / of;
        /* x100, without 64-bit arithmetic: instr/(cyc/100) rather
         * than instr*100/cyc, which overflows over a whole second. */
        ipc = (s->cyc >= 100u) ? (s->instr / (s->cyc / 100u)) : 0;
        pct = avail ? (clean / (avail / 100u ? avail / 100u : 1)) : 0;

        /* P_DECODE is inside P_RENDER, so it is not added to the
         * total -- counting it twice would put the total above 100%
         * and make the one summary number untrustworthy. */
        if (i != P_DECODE && i != P_WAIT) clean_total += clean;

        printf("   %-8s%c%6lu %8lu %8lu %8lu %4lu %6lu\n",
            prof_names[i], (avg > 4u * s->min) ? '~' : ' ',
            (unsigned long)s->calls,
            (unsigned long)s->min, (unsigned long)avg,
            (unsigned long)cof, (unsigned long)ipc,
            (unsigned long)pct);
    }

    printf("   c/of and %%avail are wall cycles scaled by CPU share\n");
    printf("   accounted %lu cyc of %lu available (%lu%%), %lu cyc/out frame\n",
        (unsigned long)clean_total, (unsigned long)avail,
        (unsigned long)(avail ? clean_total / (avail / 100u ? avail / 100u : 1) : 0),
        (unsigned long)(clean_total / of));

    /*
     * The budget, stated so it does not have to be worked out by hand
     * each time. To play in real time this app must produce out_hz
     * frames per second inside its own share of the core:
     *
     *     cycles available per output frame = clk * share / out_hz
     *
     * If the "cyc/out frame" figure above exceeds this, the app cannot
     * keep up no matter how the work is arranged, and the only
     * remaining moves are a lower output rate or moving work into
     * gateware.
     */
    if (out_hz && share)
        printf("   budget: %lu cyc/out frame  (~ = uneven or preempted)\n",
            (unsigned long)(((clk / 1000u) * share) / out_hz));

    prof_reset();
}

#endif
