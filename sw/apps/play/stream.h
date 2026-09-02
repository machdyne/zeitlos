#ifndef STREAM_H
#define STREAM_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * stream -- the buffering and rate-conversion half of sw/apps/play.
 * Sits between the SD card and the audio FIFO:
 *
 *   fs_read_chunk() --> [ byte ring ] --> adec --> [ scratch ] -->
 *       resample --> caller's block --> z_audio_push()
 *
 * Like adec.c this file touches NO hardware and NO filesystem, so the
 * whole chain above can be exercised on the host with a synthesised
 * file and checked for pitch, amplitude and drift. The app supplies
 * the two ends.
 *
 * -- why there are two buffers and not one --
 *
 * The ring holds FILE BYTES, undecoded. The scratch holds decoded
 * SOURCE frames. They cannot be the same buffer because the ring's job
 * is to absorb SD latency, which is measured in bytes-of-file, and the
 * scratch's job is to give the resampler something contiguous to
 * interpolate across, which is measured in frames.
 *
 * Buffering decoded frames instead would be the obvious simplification
 * and it is a real loss: an IMA ADPCM file expands 4x on decode, so a
 * 32KB ring of decoded audio absorbs a quarter as much SD jitter as
 * the same 32KB of file bytes. The ring is exactly where the jitter
 * has to be absorbed, so it holds the compact form.
 *
 * -- the resampler --
 *
 * 32-bit phase accumulator, 14 fractional bits. The same split
 * sw/apps/track/modplay.c uses, for the same reasons stated there, and
 * deliberately the same so that anyone who has read one has read both.
 *
 * The DAC rate is whatever the board came up with (44117.6Hz typically,
 * 46875 on an S/PDIF board) and the file's rate is whatever it is.
 * Those are never equal -- not even for a 44100Hz file, which is 0.04%
 * off -- so there is no "rates match, just copy" fast path to take and
 * pretending otherwise would introduce a drift of one frame every 2500.
 * Everything goes through the accumulator.
 *
 * Interpolation is switchable at runtime (stream_set_interp) because
 * it is the largest single term in the per-output-frame cost and
 * whether it is affordable is a question about the board, not about
 * the code. See docs/play_app.md for measured numbers.
 */

#include <stdint.h>
#include <stdbool.h>

#include "adec.h"

/*
 * Decoded source frames held between the decoder and the resampler.
 *
 * 256 frames is about 6ms at 44.1kHz. Big enough that adec_decode()'s
 * per-call overhead is amortised over a useful run, small enough that
 * a volume change (which is applied at decode time -- see adec.h) is
 * inaudibly delayed, and small enough that an IMA block, whose frames
 * must all land in one call, always fits: the largest block size any
 * encoder uses in practice is 2048 bytes, which is 4089 frames mono...
 * which does NOT fit, and so stream_init() refuses a block that big
 * rather than deadlocking. mkaudio.py emits 512-byte blocks (1017
 * frames mono, 505 stereo). 1024 frames covers every sane encoder.
 */
#define STREAM_SCRATCH_FRAMES 1024

/*
 * Largest decodable unit this build accepts, in bytes.
 *
 * Only IMA ADPCM has a block bigger than a frame. 2048 covers every
 * encoder setting seen in practice; mkaudio.py writes 512. A file
 * beyond this is refused by stream_init() with a message rather than
 * mis-decoded.
 */
#define STREAM_MAX_BLOCK 2048

#define STREAM_FRAC_BITS 14
#define STREAM_FRAC_ONE  (1u << STREAM_FRAC_BITS)

typedef struct {

    /* -- byte ring, caller-owned, capacity MUST be a power of two --
     * so the wrap is a mask rather than a compare-and-subtract in the
     * innermost place this struct is touched. */
    uint8_t   *ring;
    uint32_t   cap;
    uint32_t   mask;
    uint32_t   head;         /* write position, monotonic */
    uint32_t   tail;         /* read position, monotonic */

    adec_t    *dec;

    /* -- decoded source frames -- */
    int16_t    scr[STREAM_SCRATCH_FRAMES * 2];
    uint32_t   scr_n;        /* valid frames in scr */
    uint32_t   scr_i;        /* next frame to consume */

    /* -- resampler -- */
    uint32_t   step;         /* source frames per output frame, 18.14 */
    uint32_t   frac;
    int32_t    s0l, s0r;     /* current source frame */
    int32_t    s1l, s1r;     /* the one after it */
    bool       primed;
    bool       interp;

    /* -- accounting -- */
    bool       eof;          /* the app will add no more bytes */
    bool       ended;        /* eof AND everything buffered is played */
    uint32_t   src_frames;   /* source frames consumed since reset */
    uint32_t   out_frames;   /* output frames produced since reset */
    uint32_t   starved;      /* renders that ran out of input */

} stream_t;

/*
 * Bind a caller-owned ring (capacity a power of two) and decoder.
 * `out_hz` is the DAC's actual rate from z_audio_rate_hz().
 *
 * Returns false if the ring capacity is not a power of two, or if the
 * decoder's block produces more frames than the scratch can hold --
 * both of which would otherwise fail as a silent stall rather than as
 * an error, which is much harder to recognise.
 */
bool stream_init(stream_t *s, uint8_t *ring, uint32_t cap,
    adec_t *dec, uint32_t out_hz);

/* Drop everything buffered and restart the accumulator. Call after a
 * seek or when changing file; the decoder's own state is reset by the
 * next block header it reads. */
void stream_reset(stream_t *s);

void stream_set_interp(stream_t *s, bool on);

/* Bytes currently queued, and room for more. */
uint32_t stream_avail(const stream_t *s);
uint32_t stream_space(const stream_t *s);

/*
 * Where to write the next bytes, and how many may be written there
 * CONTIGUOUSLY -- which is less than stream_space() whenever the free
 * region wraps. Returns 0 when full.
 *
 * Handing out a pointer rather than taking a copy is the point: the
 * app reads straight from the filesystem into the ring, so a 2KB chunk
 * costs one fs_read_chunk() and no memcpy at all.
 */
uint32_t stream_write_ptr(stream_t *s, uint8_t **p);

/* Publish `n` bytes written at the pointer above. */
void stream_commit(stream_t *s, uint32_t n);

/* Tell the stream no more bytes are coming (end of file reached).
 * Rendering continues until the ring and scratch are both dry. */
void stream_set_eof(stream_t *s, bool eof);

/*
 * True once eof is set AND everything buffered has been played out.
 *
 * Reports the stream's own `ended` flag -- i.e. the moment a render
 * actually ran out of input with eof set -- rather than inferring it
 * from the byte counts. The counts get it wrong at both ends: a ring
 * holding less than one block still has decoded frames in the scratch,
 * and a stream that has not been rendered since the last commit has
 * not finished no matter how empty the ring looks.
 */
bool stream_drained(const stream_t *s);

/*
 * Produce up to `nframes` interleaved 16-bit stereo output frames.
 *
 * Returns the number actually produced, which is short of `nframes`
 * only when the ring ran dry -- i.e. the SD side could not keep up.
 * That case increments `starved`, and `starved` is the honest measure
 * of whether this machine can stream this file: it counts the moments
 * the player had somewhere to put audio and nothing to put there.
 *
 * Distinct from the hardware's UNDERRUN bit, which says the FIFO ran
 * dry -- possible for other reasons (a long redraw, another process
 * hogging the CPU) and pointing at a different fix. Both are on the
 * status line for exactly that reason.
 */
uint32_t stream_render(stream_t *s, int16_t *out, uint32_t nframes);

#endif
