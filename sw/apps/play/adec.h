#ifndef ADEC_H
#define ADEC_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * adec -- the codec half of sw/apps/play. Parses a container header
 * and turns bytes from a file into interleaved 16-bit stereo frames.
 *
 * -- what this file is allowed to touch --
 *
 * NOTHING. No hardware, no filesystem, no sw/common, no malloc, no
 * stdio. Same discipline sw/apps/track/modplay.c follows and for the
 * same reason: every arithmetic decision in here can then be checked
 * with the HOST compiler, on a machine where a wrong answer prints as
 * a number instead of sounding slightly off. That is not a hypothetical
 * benefit -- see the bug list in docs/play_app.md.
 *
 * -- everything decodes to 16-bit stereo --
 *
 * A mono source is duplicated into both channels here rather than
 * carried as mono and split later. It costs one extra store per frame
 * in the scratch buffer and it means the resampler, the FIFO push
 * loop, the scope and the volume control each exist ONCE rather than
 * in a mono and a stereo variant. Two code paths that must agree about
 * pitch is exactly the kind of thing that produces a bug audible only
 * on half the files.
 *
 * -- volume is applied HERE, not in the resampler --
 *
 * This looks like the wrong layer and it is the right one. Volume
 * costs one multiply per SOURCE sample here, versus one per OUTPUT
 * sample in the resampler -- identical when the rates match, cheaper
 * whenever the file is below the DAC rate, which is the common case
 * (a 22kHz file on a 44.1kHz DAC halves it). And for every 8-bit
 * codec -- u8, u-law, A-law -- it folds into the 256-entry expansion
 * table and costs NOTHING AT ALL.
 *
 * The price is that a volume change takes effect one scratch buffer
 * later, about 6ms. Nobody can hear 6ms of latency on a volume knob.
 *
 * -- adding a codec --
 *
 * Three places, in this order: an ADEC_* enum value, a case in
 * adec_decode(), and whatever recognises it in adec_parse(). If it is
 * an 8-bit companded format it needs a table builder next to
 * build_ulaw()/build_alaw() and nothing else -- adec_decode()'s
 * ADEC_TABLE8 case already handles any of them.
 *
 * What a codec must NOT be to live here: anything that needs more
 * state than fits in this struct, anything that needs to look
 * backwards more than one block, and anything that cannot decode from
 * an arbitrary block boundary. Those constraints are what make seeking
 * work (adec_seek_align()) and what keep the ring buffer a plain byte
 * ring. MP3 and FLAC both fail all three, which is one of several
 * reasons they are not here; docs/play_app.md has the rest.
 */

#include <stdint.h>
#include <stdbool.h>

enum {
    ADEC_NONE = 0,
    ADEC_PCM_U8,        /* WAV 8-bit, unsigned, 0x80 == silence */
    ADEC_PCM_S16LE,     /* WAV 16-bit signed little endian */
    ADEC_PCM_S16BE,     /* .au 16-bit signed big endian */
    ADEC_ULAW,          /* G.711 u-law, WAV tag 7 / .au encoding 1 */
    ADEC_ALAW,          /* G.711 A-law, WAV tag 6 / .au encoding 27 */
    ADEC_IMA_WAV,       /* IMA/DVI ADPCM, WAV tag 0x11 */
    ADEC_CODEC_COUNT
};

/* adec_parse() results. Everything except ADEC_OK is a refusal with a
 * specific reason, because "unsupported file" on its own sends you
 * looking in the wrong place -- a 24-bit WAV and a truncated one want
 * completely different responses. */
enum {
    ADEC_OK = 0,
    ADEC_ERR_UNKNOWN,     /* extension and magic both unrecognised */
    ADEC_ERR_CODEC,       /* container understood, codec inside is not */
    ADEC_ERR_DEPTH,       /* e.g. 24- or 32-bit PCM */
    ADEC_ERR_CHANNELS,    /* more than 2 */
    ADEC_ERR_HEADER,      /* malformed, or data chunk beyond the probe */
    ADEC_ERR_RATE         /* rate 0, or absurd */
};

/* Unity gain. 0 is silence; values above this amplify and WILL clip,
 * which is why adec_set_gain() clamps. */
#define ADEC_GAIN_UNITY 64
#define ADEC_GAIN_MAX   64

/* How much of the start of a file adec_parse() is shown.
 *
 * A WAV's `data` chunk is normally within a hundred bytes of the
 * start, but nothing in RIFF requires that -- a LIST/INFO block
 * carrying tags sits before it and can be any size. 1KB covers
 * everything encountered in practice and everything mkaudio.py
 * produces; beyond it this reports ADEC_ERR_HEADER rather than
 * guessing, and mkaudio.py will rewrite the file without the tags.
 *
 * The alternative -- letting the parser ask for seeks -- would make
 * this file depend on the filesystem, which is the one thing it must
 * not do. */
#define ADEC_PROBE_BYTES 1024

/*
 * What a headerless .raw/.pcm file is assumed to be.
 *
 * Overridable from the Makefile. Defaults chosen to match what
 * tools/mkaudio.py emits for `--raw`, so the two cannot silently
 * disagree -- which they would, inaudibly and then very audibly, if
 * one of them were changed alone.
 */
#ifndef PLAY_RAW_CODEC
#define PLAY_RAW_CODEC     ADEC_PCM_S16LE
#endif
#ifndef PLAY_RAW_CHANNELS
#define PLAY_RAW_CHANNELS  2
#endif
#ifndef PLAY_RAW_RATE
#define PLAY_RAW_RATE      22050
#endif

typedef struct {

    int       codec;
    int       channels;      /* 1 or 2, as stored in the file */
    uint32_t  rate;          /* source sample rate in Hz */

    /* Added to an 8-bit sample before the tab8 lookup. 0 for WAV
     * (unsigned) and 128 for .au encoding 2 (signed) -- one field
     * instead of a second codec for a one-bit difference. */
    int       bias8;

    uint32_t  data_off;      /* byte offset of the first sample byte */
    uint32_t  data_len;      /* sample bytes; 0 means "to end of file" */

    /* The smallest number of bytes that can be decoded on its own.
     * For PCM that is one frame; for IMA it is a whole ADPCM block,
     * because the predictor state lives in the block header. Seeks
     * must land on a multiple of this from data_off -- see
     * adec_seek_align(). */
    uint32_t  block;
    uint32_t  bframes;       /* frames produced per `block` bytes */

    int       gain;          /* 0..ADEC_GAIN_MAX */

    /* Expansion table for the 8-bit codecs, with gain already folded
     * in. Rebuilt by adec_set_gain(). 512 bytes, and it is why u-law
     * playback costs less CPU than 16-bit PCM does. */
    int16_t   tab8[256];

    /* IMA ADPCM per-channel predictor state. Reset at every block
     * header, so it never has to survive a seek. */
    int32_t   ima_pred[2];
    int32_t   ima_idx[2];

} adec_t;

/*
 * Identify and parse. `probe` holds the first `probelen` bytes of the
 * file (up to ADEC_PROBE_BYTES), `filesize` its total length, `path`
 * its name -- the extension is the primary hint, as agreed, with the
 * container magic used to confirm or override it.
 *
 * Returns ADEC_OK or one of the ADEC_ERR_* above. On success every
 * field of *a is valid and gain is set to unity.
 */
int adec_parse(const uint8_t *probe, uint32_t probelen, uint32_t filesize,
    const char *path, adec_t *a);

/* 0..ADEC_GAIN_MAX, clamped. Rebuilds tab8 for the 8-bit codecs. */
void adec_set_gain(adec_t *a, int gain);

/*
 * Decode whole blocks from `in` into `out` as interleaved 16-bit
 * stereo.
 *
 * Stops at whichever comes first: `inlen` bytes consumed, `maxframes`
 * frames produced, or a partial block at the end of `in`. Writes the
 * number of INPUT bytes actually used to *consumed -- which is always
 * a multiple of a->block, so the caller's ring never ends up holding
 * half a block it cannot interpret.
 *
 * Returns frames written to `out`. A return of 0 with *consumed == 0
 * means "not enough input for one block yet", not an error.
 */
uint32_t adec_decode(adec_t *a, const uint8_t *in, uint32_t inlen,
    int16_t *out, uint32_t maxframes, uint32_t *consumed);

/*
 * Total playable frames, or 0 if the file did not say. Used only for
 * the time display, so 0 is survivable -- play shows elapsed without a
 * total rather than refusing the file.
 */
uint32_t adec_total_frames(const adec_t *a);

/*
 * Byte offset to seek to, for a given frame position -- already
 * rounded DOWN to a block boundary and clamped into the file.
 *
 * Rounding down rather than to nearest, deliberately: rounding up can
 * land past the last block on a seek to the very end, and a player
 * that seeks to 99% and reports EOF looks broken in a way that is
 * annoying to trace back to a rounding rule.
 */
uint32_t adec_seek_offset(const adec_t *a, uint32_t frame);

/* Frame position a byte offset corresponds to -- the inverse of the
 * above, for reporting where a seek actually landed. */
uint32_t adec_frame_at_offset(const adec_t *a, uint32_t offset);

/* Short display name: "PCM16", "IMA ADPCM", ... */
const char *adec_codec_name(int codec);

/* Human-readable reason for an ADEC_ERR_*. */
const char *adec_error_name(int err);

#endif
