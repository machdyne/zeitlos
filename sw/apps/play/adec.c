/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * adec -- container parsing and sample decoding for sw/apps/play.
 * See adec.h for the interface and the rules this file lives under
 * (no hardware, no filesystem, no sw/common, no allocation).
 */

#include <string.h>

#include "adec.h"

/* ------------------------------------------------------------------
 * little helpers
 *
 * Byte order is done by hand rather than by casting to a uint16_t*.
 * RISC-V allows unaligned loads on this core but a WAV chunk body is
 * not guaranteed aligned, the host build must produce identical
 * results, and a shift-and-or costs nothing at -Os. Casting here is
 * the classic way to get a test that passes on x86 and a player that
 * misparses on the target.
 * ------------------------------------------------------------------ */

static uint32_t rd16le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int32_t apply_gain(int32_t s, int gain) {
    return (s * gain) >> 6;
}

/* ------------------------------------------------------------------
 * G.711 expansion
 *
 * Written from the ITU-T G.711 definition rather than copied: the
 * segment/quantisation split is the whole of the standard and it is
 * shorter to state than to cite.
 *
 * Both are built into a->tab8 once, at adec_set_gain() time, with the
 * volume already multiplied in. So the decode loop for a u-law file is
 * one table lookup and two stores per frame, which is why u-law is
 * CHEAPER to play than 16-bit PCM despite carrying the same audio.
 * ------------------------------------------------------------------ */

static int32_t ulaw_expand(uint8_t u) {
    int32_t t;
    u = (uint8_t)~u;
    t = (int32_t)((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

static int32_t alaw_expand(uint8_t a) {
    int32_t t;
    int seg;
    a ^= 0x55;
    t = (int32_t)(a & 0x0f) << 4;
    seg = (a & 0x70) >> 4;
    if (seg == 0) {
        t += 8;
    } else if (seg == 1) {
        t += 0x108;
    } else {
        t += 0x108;
        t <<= (seg - 1);
    }
    return (a & 0x80) ? t : -t;
}

static void build_tab8(adec_t *a) {
    int i;
    for (i = 0; i < 256; i++) {
        int32_t v;
        switch (a->codec) {
            case ADEC_ULAW:    v = ulaw_expand((uint8_t)i); break;
            case ADEC_ALAW:    v = alaw_expand((uint8_t)i); break;
            /* WAV 8-bit PCM is UNSIGNED with 0x80 as silence, unlike
             * every other depth WAV uses, which are signed. Getting
             * this wrong does not produce silence or noise -- it
             * produces the correct audio with a large DC offset, which
             * on a sigma-delta output is inaudible on small speakers
             * and a warm resistor on headphones. */
            case ADEC_PCM_U8:  v = ((int32_t)i - 128) << 8; break;
            default:           v = 0; break;
        }
        a->tab8[i] = clamp16(apply_gain(v, a->gain));
    }
}

void adec_set_gain(adec_t *a, int gain) {
    if (gain < 0) gain = 0;
    if (gain > ADEC_GAIN_MAX) gain = ADEC_GAIN_MAX;
    a->gain = gain;
    if (a->codec == ADEC_ULAW || a->codec == ADEC_ALAW
        || a->codec == ADEC_PCM_U8)
        build_tab8(a);
}

/* ------------------------------------------------------------------
 * IMA / DVI ADPCM
 *
 * 4 bits per sample, ~12-bit effective resolution, and the single
 * most useful codec on this machine -- not because storage is short
 * but because the SD read costs CPU. sdmm.c moves one byte per
 * MMIO write/poll/read cycle, roughly 48 cycles a byte, so halving
 * the bytes halves a cost that is measured in CPU rather than in
 * milliseconds of waiting. 4:1 is what makes 44.1kHz stereo
 * comfortable; see docs/play_app.md for the arithmetic.
 * ------------------------------------------------------------------ */

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t ima_idxadj[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

/*
 * One IMA block, decoded into `out` as interleaved 16-bit stereo.
 *
 * -- this function is the whole app's performance story --
 *
 * Measured on hardware with PLAY_PROF=1, the first version of this
 * cost about 2300 cycles per SOURCE frame against a budget of 690. It
 * was 90% of everything the app did. Three things were wrong with it
 * and all three are the same mistake in different clothes: touching
 * main memory where a register would do, on a core with no data cache
 * whose measured IPC is 0.08.
 *
 *   1. The nibble step took `int32_t *pred, int32_t *idx` and was
 *      called per sample. Passing the predictor state BY POINTER
 *      forces a load and a store through &a->ima_pred[c] for every
 *      single nibble -- four main-memory round trips per sample, to
 *      carry two values that want to live in registers for the whole
 *      block. The state is now hoisted into locals on entry and
 *      written back once at the end.
 *
 *   2. Output was addressed as out[ff * 2 + c], recomputing a scaled
 *      index per store. Both channels now walk their own pointer.
 *
 *   3. clamp16() ran after every gain multiply. It cannot fire: the
 *      IMA predictor is already clamped to int16 range by the
 *      algorithm, and gain is at most ADEC_GAIN_UNITY (64), so
 *      (s * gain) >> 6 cannot exceed what s already was. Two compares
 *      and two branches per sample, guarding an impossibility.
 *
 * Block layout, mono: 4-byte header {int16 predictor, uint8 index,
 * uint8 reserved}, then nibble pairs, LOW NIBBLE FIRST.
 *
 * Stereo: two 4-byte headers, then the data alternates in 4-byte
 * GROUPS -- four bytes of left (eight samples), four of right, and so
 * on. NOT nibble-interleaved. Getting this wrong is one of the more
 * entertaining failure modes available: the audio is recognisable,
 * both channels play, and it sounds like it is being chewed.
 *
 * The predictor in the header is the FIRST SAMPLE of the block, not a
 * prediction of the sample before it, so it is emitted as-is and the
 * nibbles produce the rest. A block holds 1 + (block - 4*ch) * 2 / ch
 * frames.
 */

/* The nibble step, as a macro rather than a function taking pointers.
 * `pr` and `ix` are the caller's LOCALS and stay in registers across
 * the whole block -- see point 1 above. */
#define IMA_STEP(pr, ix, nib) do { \
    int32_t _st = ima_step[ix]; \
    int32_t _d = _st >> 3; \
    if ((nib) & 1) _d += _st >> 2; \
    if ((nib) & 2) _d += _st >> 1; \
    if ((nib) & 4) _d += _st; \
    if ((nib) & 8) (pr) -= _d; else (pr) += _d; \
    if ((pr) > 32767) (pr) = 32767; \
    if ((pr) < -32768) (pr) = -32768; \
    (ix) += ima_idxadj[nib]; \
    if ((ix) < 0) (ix) = 0; \
    if ((ix) > 88) (ix) = 88; \
} while (0)

static uint32_t ima_decode_block(adec_t *a, const uint8_t *in,
    int16_t *out, uint32_t maxframes)
{
    uint32_t frames = a->bframes;
    int gain = a->gain;
    const uint8_t *p;

    if (frames > maxframes) return 0;

    if (a->channels == 1) {

        int32_t pred = (int32_t)(int16_t)(uint16_t)rd16le(in);
        int32_t idx = in[2];
        int16_t *o = out;
        uint32_t f;

        if (idx > 88) idx = 88;

        /* Frame 0 is the header predictor itself. */
        o[0] = o[1] = (int16_t)((pred * gain) >> 6);
        o += 2;

        p = in + 4;
        f = 1;
        while (f < frames) {
            uint8_t b = *p++;
            int16_t v;
            IMA_STEP(pred, idx, (b & 0x0f));
            v = (int16_t)((pred * gain) >> 6);
            o[0] = v; o[1] = v; o += 2;
            if (++f >= frames) break;
            IMA_STEP(pred, idx, (b >> 4));
            v = (int16_t)((pred * gain) >> 6);
            o[0] = v; o[1] = v; o += 2;
            f++;
        }

        a->ima_pred[0] = pred;
        a->ima_idx[0] = idx;
        return frames;
    }

    /* -- stereo -- */
    {
        int32_t pred0 = (int32_t)(int16_t)(uint16_t)rd16le(in);
        int32_t idx0 = in[2];
        int32_t pred1 = (int32_t)(int16_t)(uint16_t)rd16le(in + 4);
        int32_t idx1 = in[6];
        uint32_t base;

        if (idx0 > 88) idx0 = 88;
        if (idx1 > 88) idx1 = 88;

        out[0] = (int16_t)((pred0 * gain) >> 6);
        out[1] = (int16_t)((pred1 * gain) >> 6);

        p = in + 8;
        base = 1;

        /* One 8-byte group per pass: four bytes of left, four of
         * right, eight frames out. Each channel walks its own output
         * pointer with a stride of two. */
        while (base < frames) {

            int16_t *ol = out + base * 2;
            int16_t *orr = ol + 1;
            uint32_t n = frames - base;
            uint32_t k;

            if (n > 8) n = 8;

            for (k = 0; k < n; k++) {
                uint8_t b = p[k >> 1];
                uint8_t nib = (k & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0f);
                IMA_STEP(pred0, idx0, nib);
                *ol = (int16_t)((pred0 * gain) >> 6);
                ol += 2;
            }
            for (k = 0; k < n; k++) {
                uint8_t b = p[4 + (k >> 1)];
                uint8_t nib = (k & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0f);
                IMA_STEP(pred1, idx1, nib);
                *orr = (int16_t)((pred1 * gain) >> 6);
                orr += 2;
            }

            p += 8;
            base += 8;
        }

        a->ima_pred[0] = pred0;
        a->ima_idx[0] = idx0;
        a->ima_pred[1] = pred1;
        a->ima_idx[1] = idx1;
        return frames;
    }
}

/* ------------------------------------------------------------------
 * parsing
 * ------------------------------------------------------------------ */

static int ext_is(const char *path, const char *ext) {

    const char *dot = 0;
    const char *p;
    int i;

    for (p = path; *p; p++)
        if (*p == '.') dot = p;
    if (!dot) return 0;

    dot++;
    for (i = 0; ext[i]; i++) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return dot[i] == 0;
}

/*
 * Frames per block, and the block size itself, for whatever codec was
 * just identified. One place, because these two numbers must agree
 * with each other or seeking lands mid-block and the time display
 * drifts -- and they are easy to get subtly right for PCM and subtly
 * wrong for ADPCM.
 */
static int set_block_geometry(adec_t *a, uint32_t wav_block_align) {

    switch (a->codec) {

        case ADEC_PCM_U8:
        case ADEC_ULAW:
        case ADEC_ALAW:
            a->block = (uint32_t)a->channels;
            a->bframes = 1;
            return ADEC_OK;

        case ADEC_PCM_S16LE:
        case ADEC_PCM_S16BE:
            a->block = (uint32_t)a->channels * 2u;
            a->bframes = 1;
            return ADEC_OK;

        case ADEC_IMA_WAV: {
            uint32_t hdr = 4u * (uint32_t)a->channels;
            if (wav_block_align <= hdr) return ADEC_ERR_HEADER;
            /* Trust nBlockAlign over the optional wSamplesPerBlock
             * field: the block size is what the file is physically
             * laid out in, and an encoder that disagrees with itself
             * is better caught here than heard later. */
            a->block = wav_block_align;
            a->bframes = 1u
                + ((wav_block_align - hdr) * 2u) / (uint32_t)a->channels;
            return ADEC_OK;
        }

        default:
            return ADEC_ERR_CODEC;
    }
}

/*
 * RIFF/WAVE.
 *
 * Walks the chunk list rather than assuming fmt-then-data at fixed
 * offsets. Files with a LIST/INFO block between them are common enough
 * (anything that has been through a tag editor) that assuming the
 * layout means rejecting ordinary files.
 *
 * Chunk sizes are padded to even, and forgetting that pad is the
 * classic RIFF walker bug -- it puts the walker one byte out at the
 * first odd-sized chunk and every subsequent chunk id reads as
 * garbage.
 */
static int parse_wav(const uint8_t *p, uint32_t len, uint32_t filesize,
    adec_t *a)
{
    uint32_t off = 12;
    int have_fmt = 0;
    uint32_t wav_block_align = 0;

    if (len < 44) return ADEC_ERR_HEADER;
    if (memcmp(p + 8, "WAVE", 4) != 0) return ADEC_ERR_HEADER;

    while (off + 8 <= len) {

        const uint8_t *ck = p + off;
        uint32_t cksz = rd32le(ck + 4);
        uint32_t body = off + 8;

        if (memcmp(ck, "fmt ", 4) == 0) {

            uint32_t tag, ch, rate, bits;

            if (cksz < 16 || body + 16 > len) return ADEC_ERR_HEADER;

            tag  = rd16le(p + body + 0);
            ch   = rd16le(p + body + 2);
            rate = rd32le(p + body + 4);
            wav_block_align = rd16le(p + body + 12);
            bits = rd16le(p + body + 14);

            if (ch < 1 || ch > 2) return ADEC_ERR_CHANNELS;
            if (rate < 1000 || rate > 200000) return ADEC_ERR_RATE;

            a->channels = (int)ch;
            a->rate = rate;

            switch (tag) {
                case 0x0001:    /* WAVE_FORMAT_PCM */
                    if (bits == 8) a->codec = ADEC_PCM_U8;
                    else if (bits == 16) a->codec = ADEC_PCM_S16LE;
                    else return ADEC_ERR_DEPTH;
                    break;
                case 0x0006:    /* WAVE_FORMAT_ALAW */
                    a->codec = ADEC_ALAW;
                    break;
                case 0x0007:    /* WAVE_FORMAT_MULAW */
                    a->codec = ADEC_ULAW;
                    break;
                case 0x0011:    /* WAVE_FORMAT_DVI_ADPCM */
                    if (bits != 4) return ADEC_ERR_DEPTH;
                    a->codec = ADEC_IMA_WAV;
                    break;
                /* 0x0003 is IEEE float and 0xFFFE is EXTENSIBLE. Both
                 * are refused rather than half-supported: float needs
                 * an FPU this core does not have, and EXTENSIBLE's
                 * real format lives in a GUID whose only common values
                 * are the two above, so "support" would be a lookup
                 * table pretending to be a parser. mkaudio.py writes
                 * plain tag 1 or 0x11. */
                default:
                    return ADEC_ERR_CODEC;
            }

            have_fmt = 1;

        } else if (memcmp(ck, "data", 4) == 0) {

            uint32_t avail;

            if (!have_fmt) return ADEC_ERR_HEADER;

            a->data_off = body;

            /* A `data` size larger than the file is not rare -- it is
             * what every stream that was cut short leaves behind, and
             * what a writer that crashed before rewinding to patch the
             * header leaves behind. Clamp instead of refusing: the
             * audio that IS there plays fine. */
            avail = (filesize > body) ? (filesize - body) : 0;
            a->data_len = (cksz && cksz <= avail) ? cksz : avail;

            return set_block_geometry(a, wav_block_align);
        }

        off = body + cksz + (cksz & 1u);      /* chunks pad to even */
        if (off <= body) return ADEC_ERR_HEADER;   /* cksz overflow */
    }

    /* fmt was found but data was past the probe window, or the file is
     * malformed. Distinguishable and worth distinguishing. */
    return ADEC_ERR_HEADER;
}

/*
 * Sun/NeXT .au. Big endian throughout, and the header is fixed-layout
 * with no chunk walking, which makes it the format to reach for when
 * something about WAV parsing is under suspicion.
 */
static int parse_au(const uint8_t *p, uint32_t len, uint32_t filesize,
    adec_t *a)
{
    uint32_t off, sz, enc, rate, ch, avail;

    if (len < 24) return ADEC_ERR_HEADER;

    off  = rd32be(p + 4);
    sz   = rd32be(p + 8);
    enc  = rd32be(p + 12);
    rate = rd32be(p + 16);
    ch   = rd32be(p + 20);

    if (off < 24 || off > filesize) return ADEC_ERR_HEADER;
    if (ch < 1 || ch > 2) return ADEC_ERR_CHANNELS;
    if (rate < 1000 || rate > 200000) return ADEC_ERR_RATE;

    switch (enc) {
        case 1:  a->codec = ADEC_ULAW; break;
        case 2:  a->codec = ADEC_PCM_U8; a->bias8 = 128; break;
        case 3:  a->codec = ADEC_PCM_S16BE; break;
        case 27: a->codec = ADEC_ALAW; break;
        default: return ADEC_ERR_CODEC;
    }

    /* .au encoding 2 is 8-bit SIGNED, where WAV 8-bit is unsigned.
     * Rather than a second codec for a one-bit difference, the table
     * is the same one with the index biased by 128 -- set above and
     * applied in adec_decode(). */
    a->channels = (int)ch;
    a->rate = rate;
    a->data_off = off;

    avail = filesize - off;
    a->data_len = (sz && sz != 0xFFFFFFFFu && sz <= avail) ? sz : avail;

    return set_block_geometry(a, 0);
}

int adec_parse(const uint8_t *probe, uint32_t probelen, uint32_t filesize,
    const char *path, adec_t *a)
{
    int rc = ADEC_ERR_UNKNOWN;

    memset(a, 0, sizeof(*a));
    a->gain = ADEC_GAIN_UNITY;
    a->channels = 2;

    /* Magic first, extension second -- despite the extension being
     * what the file list filters on. A .WAV that is really an .au is a
     * renamed file and should still play; a .WAV that is really a
     * 24-bit WAV must be refused with the right reason, which only the
     * header can give. The extension's job is deciding what to OFFER,
     * not what to believe. */
    if (probelen >= 12 && memcmp(probe, "RIFF", 4) == 0)
        rc = parse_wav(probe, probelen, filesize, a);
    else if (probelen >= 24 && memcmp(probe, ".snd", 4) == 0)
        rc = parse_au(probe, probelen, filesize, a);
    else if (ext_is(path, "raw") || ext_is(path, "pcm")) {
        /* Headerless. Everything about it is a build-time assumption,
         * which is why it is last and why the app says so on screen --
         * a raw file played with the wrong assumptions is not silent,
         * it is fast, slow, or noise, and none of those announce which
         * assumption was wrong. */
        a->codec = PLAY_RAW_CODEC;
        a->channels = PLAY_RAW_CHANNELS;
        a->rate = PLAY_RAW_RATE;
        a->data_off = 0;
        a->data_len = filesize;
        rc = set_block_geometry(a, 0);
    }

    if (rc != ADEC_OK) return rc;

    /* Round data_len down to a whole number of blocks. A trailing
     * partial block cannot be decoded and, left in, makes the ring's
     * "have I got a whole block?" test permanently false at end of
     * file -- the player stalls one block short of the end with a full
     * ring, which looks exactly like an underrun. */
    if (a->block) a->data_len -= (a->data_len % a->block);

    adec_set_gain(a, ADEC_GAIN_UNITY);
    return ADEC_OK;
}

/* ------------------------------------------------------------------
 * decode
 * ------------------------------------------------------------------ */

uint32_t adec_decode(adec_t *a, const uint8_t *in, uint32_t inlen,
    int16_t *out, uint32_t maxframes, uint32_t *consumed)
{
    uint32_t frames = 0;
    uint32_t used = 0;

    *consumed = 0;

    switch (a->codec) {

        /* -- one table lookup per sample, gain already inside it -- */
        case ADEC_PCM_U8:
        case ADEC_ULAW:
        case ADEC_ALAW: {
            int bias8 = a->bias8;
            uint32_t n = inlen / a->block;
            if (n > maxframes) n = maxframes;
            if (a->channels == 1) {
                uint32_t i;
                for (i = 0; i < n; i++) {
                    int16_t v = a->tab8[(in[i] + bias8) & 0xff];
                    out[i * 2] = v;
                    out[i * 2 + 1] = v;
                }
            } else {
                uint32_t i;
                for (i = 0; i < n; i++) {
                    out[i * 2] = a->tab8[(in[i * 2] + bias8) & 0xff];
                    out[i * 2 + 1] = a->tab8[(in[i * 2 + 1] + bias8) & 0xff];
                }
            }
            frames = n;
            used = n * a->block;
            break;
        }

        case ADEC_PCM_S16LE: {
            uint32_t n = inlen / a->block;
            uint32_t i;
            int g = a->gain;
            if (n > maxframes) n = maxframes;
            if (a->channels == 1) {
                for (i = 0; i < n; i++) {
                    int32_t s = (int32_t)(int16_t)(uint16_t)
                        rd16le(in + i * 2);
                    int16_t v = clamp16(apply_gain(s, g));
                    out[i * 2] = v;
                    out[i * 2 + 1] = v;
                }
            } else {
                for (i = 0; i < n; i++) {
                    int32_t l = (int32_t)(int16_t)(uint16_t)
                        rd16le(in + i * 4);
                    int32_t r = (int32_t)(int16_t)(uint16_t)
                        rd16le(in + i * 4 + 2);
                    out[i * 2] = clamp16(apply_gain(l, g));
                    out[i * 2 + 1] = clamp16(apply_gain(r, g));
                }
            }
            frames = n;
            used = n * a->block;
            break;
        }

        case ADEC_PCM_S16BE: {
            uint32_t n = inlen / a->block;
            uint32_t i;
            int g = a->gain;
            if (n > maxframes) n = maxframes;
            for (i = 0; i < n; i++) {
                if (a->channels == 1) {
                    int32_t s = (int32_t)(int16_t)(uint16_t)
                        ((in[i * 2] << 8) | in[i * 2 + 1]);
                    int16_t v = clamp16(apply_gain(s, g));
                    out[i * 2] = v;
                    out[i * 2 + 1] = v;
                } else {
                    int32_t l = (int32_t)(int16_t)(uint16_t)
                        ((in[i * 4] << 8) | in[i * 4 + 1]);
                    int32_t r = (int32_t)(int16_t)(uint16_t)
                        ((in[i * 4 + 2] << 8) | in[i * 4 + 3]);
                    out[i * 2] = clamp16(apply_gain(l, g));
                    out[i * 2 + 1] = clamp16(apply_gain(r, g));
                }
            }
            frames = n;
            used = n * a->block;
            break;
        }

        case ADEC_IMA_WAV: {
            while (inlen - used >= a->block
                && frames + a->bframes <= maxframes) {
                uint32_t got = ima_decode_block(a, in + used,
                    out + frames * 2, maxframes - frames);
                if (!got) break;
                frames += got;
                used += a->block;
            }
            break;
        }

        default:
            break;
    }

    *consumed = used;
    return frames;
}

uint32_t adec_total_frames(const adec_t *a) {
    if (!a->block || !a->data_len) return 0;
    return (a->data_len / a->block) * a->bframes;
}

uint32_t adec_seek_offset(const adec_t *a, uint32_t frame) {
    uint32_t blk, maxblk;
    if (!a->bframes || !a->block) return a->data_off;
    blk = frame / a->bframes;
    maxblk = a->data_len / a->block;
    if (maxblk && blk >= maxblk) blk = maxblk - 1;
    return a->data_off + blk * a->block;
}

uint32_t adec_frame_at_offset(const adec_t *a, uint32_t offset) {
    if (!a->block || offset < a->data_off) return 0;
    return ((offset - a->data_off) / a->block) * a->bframes;
}

const char *adec_codec_name(int codec) {
    switch (codec) {
        case ADEC_PCM_U8:    return "PCM8";
        case ADEC_PCM_S16LE: return "PCM16";
        case ADEC_PCM_S16BE: return "PCM16BE";
        case ADEC_ULAW:      return "u-law";
        case ADEC_ALAW:      return "A-law";
        case ADEC_IMA_WAV:   return "IMA ADPCM";
        default:             return "?";
    }
}

const char *adec_error_name(int err) {
    switch (err) {
        case ADEC_OK:           return "ok";
        case ADEC_ERR_CODEC:    return "unsupported codec";
        case ADEC_ERR_DEPTH:    return "unsupported bit depth";
        case ADEC_ERR_CHANNELS: return "too many channels";
        case ADEC_ERR_HEADER:   return "bad or oversized header";
        case ADEC_ERR_RATE:     return "bad sample rate";
        default:                return "unrecognised file";
    }
}
