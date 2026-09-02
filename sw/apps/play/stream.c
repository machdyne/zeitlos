/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * stream -- see stream.h.
 */

#include <string.h>

#include "stream.h"
#include "prof.h"

/*
 * Bounce buffer for the one block per lap of the ring that straddles
 * the wrap. In .bss rather than on the stack: an app's C stack and its
 * malloc() heap share one 16KB tier (Z_PROC_STACK_SIZE_DEFAULT,
 * sw/os/kernel.h) and a 2KB frame appearing partway down a call chain
 * is exactly the kind of thing that overflows it on the unlucky path
 * rather than the tested one. .bss is reserved at process creation --
 * it either fits or the process never starts.
 */
static uint8_t bounce[STREAM_MAX_BLOCK];

bool stream_init(stream_t *s, uint8_t *ring, uint32_t cap,
    adec_t *dec, uint32_t out_hz)
{
    memset(s, 0, sizeof(*s));

    if (!cap || (cap & (cap - 1))) return false;      /* power of two */
    if (!dec || !dec->bframes) return false;
    if (dec->bframes > STREAM_SCRATCH_FRAMES) return false;
    if (dec->block > STREAM_MAX_BLOCK) return false;
    if (!out_hz || !dec->rate) return false;

    s->ring = ring;
    s->cap = cap;
    s->mask = cap - 1;
    s->dec = dec;
    s->interp = true;

    /*
     * step = source frames per output frame, in 18.14.
     *
     * No 64-bit arithmetic, on a 32-bit core with no hardware divide
     * for it -- a uint64 divide here is a libgcc call in the setup
     * path of every track. rate << 14 overflows above 262143Hz, and
     * adec_parse() already refuses anything above 200000, so the shift
     * is safe by construction rather than by hope. The check is
     * repeated here anyway because "safe by construction" that depends
     * on a constant in another file is one edit away from not being.
     */
    if (dec->rate > 200000u) return false;
    s->step = (dec->rate << STREAM_FRAC_BITS) / out_hz;
    if (!s->step) s->step = 1;

    return true;
}

void stream_reset(stream_t *s) {
    s->head = s->tail = 0;
    s->scr_n = s->scr_i = 0;
    s->frac = 0;
    s->s0l = s->s0r = s->s1l = s->s1r = 0;
    s->primed = false;
    s->eof = false;
    s->ended = false;
    s->src_frames = 0;
    s->out_frames = 0;
    s->starved = 0;
}

void stream_set_interp(stream_t *s, bool on) { s->interp = on; }

uint32_t stream_avail(const stream_t *s) { return s->head - s->tail; }

uint32_t stream_space(const stream_t *s) {
    return s->cap - (s->head - s->tail);
}

uint32_t stream_write_ptr(stream_t *s, uint8_t **p) {

    uint32_t used = s->head - s->tail;
    uint32_t free_bytes = s->cap - used;
    uint32_t idx = s->head & s->mask;
    uint32_t to_end = s->cap - idx;

    if (!free_bytes) { *p = 0; return 0; }
    *p = s->ring + idx;
    return (free_bytes < to_end) ? free_bytes : to_end;
}

void stream_commit(stream_t *s, uint32_t n) { s->head += n; }

void stream_set_eof(stream_t *s, bool eof) { s->eof = eof; }

bool stream_drained(const stream_t *s) { return s->ended; }

/*
 * Refill the scratch from the ring.
 *
 * Decodes as many whole blocks as fit, from ONE contiguous run of the
 * ring. Stopping at the wrap rather than stitching across it means the
 * decoder never sees a split block and needs no bounce buffer; the
 * cost is that one call in every cap/chunk calls comes back short,
 * and the next call picks up the rest. At a 32KB ring and 2KB chunks
 * that is one short call in sixteen, and a short call is not a
 * failure -- it just returns fewer frames.
 *
 * The one case that would deadlock is a block that STRADDLES the wrap
 * with fewer than a block's bytes before the end: then the contiguous
 * run is too short forever. Handled by bouncing exactly that block
 * through a small stack buffer, which happens at most once per lap of
 * the ring.
 */
static bool scratch_fill(stream_t *s) {

    adec_t *d = s->dec;
    uint32_t used = s->head - s->tail;
    uint32_t idx, to_end, run, consumed, got;

    s->scr_n = 0;
    s->scr_i = 0;

    if (used < d->block) return false;

    idx = s->tail & s->mask;
    to_end = s->cap - idx;
    run = (used < to_end) ? used : to_end;

    if (run < d->block) {
        /* Straddling block: copy it out whole, once. `block` is at
         * most the encoder's block size, which stream_init() has
         * already bounded against the scratch. */
        uint32_t i;
        for (i = 0; i < d->block; i++)
            bounce[i] = s->ring[(s->tail + i) & s->mask];
        PROF_BEGIN(P_DECODE);
        got = adec_decode(d, bounce, d->block, s->scr,
            STREAM_SCRATCH_FRAMES, &consumed);
        PROF_END(P_DECODE);
        if (!got) return false;
        s->tail += consumed;
        s->scr_n = got;
        return true;
    }

    {
        PROF_BEGIN(P_DECODE);
        got = adec_decode(d, s->ring + idx, run, s->scr,
            STREAM_SCRATCH_FRAMES, &consumed);
        PROF_END(P_DECODE);
    }
    if (!got) return false;

    s->tail += consumed;
    s->scr_n = got;
    return true;
}

/* Next source frame, or false if there is nothing left to decode. */
static bool next_src(stream_t *s, int32_t *l, int32_t *r) {

    if (s->scr_i >= s->scr_n) {
        if (!scratch_fill(s)) return false;
    }

    *l = s->scr[s->scr_i * 2];
    *r = s->scr[s->scr_i * 2 + 1];
    s->scr_i++;
    s->src_frames++;
    PROF_ADD_SRC(1);
    return true;
}

uint32_t stream_render(stream_t *s, int16_t *out, uint32_t nframes) {

    uint32_t n = 0;
    uint32_t step = s->step;
    uint32_t frac;
    int32_t s0l, s0r, s1l, s1r;
    bool interp = s->interp;

    /*
     * Once the file has genuinely ended there is nothing to produce,
     * and this early return is load bearing rather than an
     * optimisation.
     *
     * The first version had no such flag and relied on next_src()
     * failing to stop the loop. It does stop it -- for that call --
     * and then the NEXT call finds s0 and s1 still holding the last
     * two real samples, happily interpolates between them, advances
     * the phase, fails again, and returns two more frames. Forever.
     *
     * A player that reaches the end of a track and then emits a
     * held sample at whatever DC the last frame happened to sit at,
     * indefinitely, does not sound like a bug in an end-of-file
     * check. It sounds like a stuck DAC. The host test suite caught
     * it as a loop that never terminated, which is a considerably
     * better place to find out.
     */
    if (s->ended) return 0;

    /*
     * Prime with two source frames before producing anything.
     *
     * The interpolator always reads s0 and s1, so both must exist
     * before the first output frame -- and must be REAL frames, not
     * zeros. Starting from zeros produces a one-frame ramp at every
     * track change and every seek, which is a click, and a click at
     * exactly the moment the user pressed a button gets blamed on the
     * button.
     */
    if (!s->primed) {
        if (!next_src(s, &s->s0l, &s->s0r)) {
            /* Running out at EOF is the file ending, not a fault. Only
             * running out with more file still to come is starvation,
             * and conflating the two puts a permanent 1 on the status
             * line for every track that plays perfectly -- which makes
             * the one number this phase exists to watch useless. */
            if (s->eof) s->ended = true; else s->starved++;
            return 0;
        }
        if (!next_src(s, &s->s1l, &s->s1r)) {
            /* A file shorter than two frames is legal and pointless;
             * duplicate rather than special-case it downstream. */
            s->s1l = s->s0l;
            s->s1r = s->s0r;
        }
        s->primed = true;
        s->frac = 0;
    }

    frac = s->frac;
    s0l = s->s0l; s0r = s->s0r;
    s1l = s->s1l; s1r = s->s1r;

    /*
     * Advance FIRST, emit second.
     *
     * The obvious arrangement -- emit, then advance -- cannot be
     * resumed cleanly after a starve: it has to abandon the loop
     * partway through the advance, with s0 already stepped and s1 not
     * yet replaced, and the next call then emits one frame built from
     * a pair that never existed in the source. Checking the phase at
     * the top means a starve returns with s0, s1 and frac describing
     * exactly one consistent position, and the next call resumes at
     * it with no frame duplicated and none lost.
     */
    while (n < nframes) {

        while (frac >= STREAM_FRAC_ONE) {
            int32_t nl, nr;
            if (!next_src(s, &nl, &nr)) {
                if (s->eof) s->ended = true;
                s->frac = frac;
                s->s0l = s0l; s->s0r = s0r;
                s->s1l = s1l; s->s1r = s1r;
                s->out_frames += n;
                if (!s->eof && n < nframes) s->starved++;
                return n;
            }
            frac -= STREAM_FRAC_ONE;
            s0l = s1l; s0r = s1r;
            s1l = nl;  s1r = nr;
        }

        if (interp) {
            /* frac is 14 bits and the samples are 16-bit signed, so
             * the product is at most 2^30 -- a signed 32 holds it with
             * a bit to spare and no widening is needed. */
            int32_t f = (int32_t)frac;
            out[n * 2]     = (int16_t)(s0l + (((s1l - s0l) * f) >> STREAM_FRAC_BITS));
            out[n * 2 + 1] = (int16_t)(s0r + (((s1r - s0r) * f) >> STREAM_FRAC_BITS));
        } else {
            out[n * 2]     = (int16_t)s0l;
            out[n * 2 + 1] = (int16_t)s0r;
        }
        n++;
        frac += step;
    }

    s->frac = frac;
    s->s0l = s0l; s->s0r = s0r;
    s->s1l = s1l; s->s1r = s1r;
    s->out_frames += n;
    return n;
}
