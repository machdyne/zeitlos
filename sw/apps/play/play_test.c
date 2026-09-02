/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host-side tests for adec.c and stream.c.
 *
 *   make test        run them
 *   make test-wav    ...and dump each case as a .wav to listen to
 *
 * Built with the HOST compiler. adec.c and stream.c include nothing
 * from sw/common and touch no hardware, which is what makes this
 * possible and is the reason they are split out of play.c at all.
 *
 * -- fixtures are SYNTHESISED, not shipped --
 *
 * Same rule sw/apps/track/modplay_test.c states: a real audio file is
 * a copyrighted artifact, and a fixture whose correct output nobody
 * can state is not a test. Every input here is a sine wave of known
 * frequency and amplitude at a known rate, so every assertion is
 * against arithmetic rather than against a previous run.
 *
 * The pitch tests count ZERO CROSSINGS rather than comparing samples.
 * A sample-by-sample comparison against a reference would pass for a
 * resampler that is subtly wrong in exactly the way the reference was
 * generated; counting crossings measures the thing that is actually
 * being claimed -- that a 440Hz tone comes out at 440Hz after passing
 * through a rate conversion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "adec.h"
#include "stream.h"

static int tests = 0, fails = 0;
static int dump_wav = 0;

static void check(int cond, const char *name) {
    tests++;
    if (!cond) { fails++; printf("  FAIL  %s\n", name); }
    else printf("  ok    %s\n", name);
}

static void check_near(double got, double want, double tol,
    const char *name)
{
    tests++;
    if (fabs(got - want) > tol) {
        fails++;
        printf("  FAIL  %s (got %.4f want %.4f +/- %.4f)\n",
            name, got, want, tol);
    } else {
        printf("  ok    %s (%.4f)\n", name, got);
    }
}

/* ------------------------------------------------------------------
 * fixture builders
 * ------------------------------------------------------------------ */

static void put16(unsigned char *p, unsigned v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
}
static void put32(unsigned char *p, unsigned v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

/* RIFF/WAVE with an arbitrary fmt body and data payload. */
static unsigned char *make_wav(unsigned tag, unsigned ch, unsigned rate,
    unsigned bits, unsigned block_align, const unsigned char *data,
    unsigned datalen, unsigned *outlen, int with_list)
{
    unsigned fmtlen = (tag == 0x11) ? 20 : 16;
    unsigned listlen = with_list ? 30 : 0;
    unsigned total = 12 + 8 + fmtlen + (with_list ? 8 + listlen : 0)
        + 8 + datalen;
    unsigned char *b = calloc(1, total);
    unsigned o;

    memcpy(b, "RIFF", 4);
    put32(b + 4, total - 8);
    memcpy(b + 8, "WAVE", 4);

    o = 12;
    memcpy(b + o, "fmt ", 4);
    put32(b + o + 4, fmtlen);
    put16(b + o + 8, tag);
    put16(b + o + 10, ch);
    put32(b + o + 12, rate);
    put32(b + o + 16, rate * block_align);      /* byte rate, unused */
    put16(b + o + 20, block_align);
    put16(b + o + 22, bits);
    if (tag == 0x11) {
        put16(b + o + 24, 2);                   /* cbSize */
        put16(b + o + 26, 0);                   /* wSamplesPerBlock */
    }
    o += 8 + fmtlen;

    /* An odd-sized LIST chunk, deliberately: RIFF pads chunk bodies to
     * an even length and a walker that forgets the pad lands one byte
     * out at exactly this point. That bug reads the next chunk id as
     * garbage and reports "bad header" on a perfectly ordinary tagged
     * file, so there is a fixture for it. */
    if (with_list) {
        memcpy(b + o, "LIST", 4);
        put32(b + o + 4, listlen - 1);          /* odd -> needs a pad */
        memcpy(b + o + 8, "INFOISFT", 8);
        o += 8 + listlen;
    }

    memcpy(b + o, "data", 4);
    put32(b + o + 4, datalen);
    memcpy(b + o + 8, data, datalen);

    *outlen = total;
    return b;
}

/* interleaved signed 16-bit sine, `frames` long */
static unsigned char *sine_s16(unsigned frames, unsigned ch, unsigned rate,
    double hz, double amp, unsigned *len)
{
    unsigned char *b = malloc(frames * ch * 2);
    unsigned i, c;
    for (i = 0; i < frames; i++) {
        int v = (int)(amp * 32767.0 * sin(2.0 * M_PI * hz * i / rate));
        for (c = 0; c < ch; c++) put16(b + (i * ch + c) * 2, (unsigned)(v & 0xffff));
    }
    *len = frames * ch * 2;
    return b;
}

/*
 * IMA ADPCM encoder, only good enough to make a fixture.
 *
 * Deliberately NOT shared with the decoder's tables -- it carries its
 * own copies. A test that encodes with the decoder's own step table
 * proves the decoder is self-consistent, which is not the claim being
 * made. These are transcribed from the IMA specification separately,
 * so a typo in one is caught by disagreement with the other.
 */
static const int enc_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,
    66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,
    371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
    1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
    5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
    16818,18500,20350,22385,24623,27086,29794,32767
};
static const int enc_idx[16] = {
    -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
};

static int enc_nibble(int sample, int *pred, int *idx) {
    int step = enc_step[*idx];
    int diff = sample - *pred;
    int nib = 0, vpdiff = step >> 3;
    if (diff < 0) { nib = 8; diff = -diff; }
    if (diff >= step) { nib |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nib |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { nib |= 1; vpdiff += step; }
    if (nib & 8) *pred -= vpdiff; else *pred += vpdiff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += enc_idx[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return nib;
}

static unsigned char *ima_encode(const short *pcm, unsigned frames,
    unsigned ch, unsigned block_align, unsigned *outlen,
    unsigned *frames_out)
{
    unsigned hdr = 4 * ch;
    unsigned per_block = 1 + ((block_align - hdr) * 2) / ch;
    unsigned nblocks = frames / per_block;
    unsigned char *b = calloc(1, nblocks * block_align);
    unsigned bi, c;

    for (bi = 0; bi < nblocks; bi++) {
        unsigned char *o = b + bi * block_align;
        unsigned base = bi * per_block;
        int pred[2], idx[2];
        unsigned f;

        for (c = 0; c < ch; c++) {
            pred[c] = pcm[(base) * ch + c];
            idx[c] = 0;
            put16(o + c * 4, (unsigned)(pred[c] & 0xffff));
            o[c * 4 + 2] = 0;
            o[c * 4 + 3] = 0;
        }

        f = 1;
        if (ch == 1) {
            unsigned char *d = o + 4;
            unsigned k = 0;
            while (f < per_block) {
                int n = enc_nibble(pcm[base + f], &pred[0], &idx[0]);
                if (k & 1) d[k >> 1] |= (unsigned char)(n << 4);
                else d[k >> 1] = (unsigned char)n;
                k++; f++;
            }
        } else {
            unsigned char *d = o + 8;
            unsigned g = 0;
            while (f < per_block) {
                for (c = 0; c < 2; c++) {
                    unsigned char *gp = d + g * 8 + c * 4;
                    unsigned k;
                    for (k = 0; k < 8 && f + k < per_block; k++) {
                        int n = enc_nibble(pcm[(base + f + k) * 2 + c],
                            &pred[c], &idx[c]);
                        if (k & 1) gp[k >> 1] |= (unsigned char)(n << 4);
                        else gp[k >> 1] = (unsigned char)n;
                    }
                }
                f += 8;
                g++;
            }
        }
    }

    *outlen = nblocks * block_align;
    *frames_out = nblocks * per_block;
    return b;
}

/* ------------------------------------------------------------------
 * harness: push a whole file through the ring and render it
 * ------------------------------------------------------------------ */

#define RING_CAP 4096

static short *render_all(const unsigned char *file, unsigned filelen,
    const char *name, unsigned out_hz, int interp,
    unsigned *nframes_out, adec_t *dec_out, unsigned chunk)
{
    static unsigned char ring[RING_CAP];
    adec_t dec;
    stream_t st;
    unsigned pos;
    unsigned cap = 0;
    short *out;
    unsigned n = 0;
    int rc;

    rc = adec_parse(file, filelen < ADEC_PROBE_BYTES ? filelen
        : ADEC_PROBE_BYTES, filelen, name, &dec);
    if (rc != ADEC_OK) {
        printf("  (parse failed: %s)\n", adec_error_name(rc));
        *nframes_out = 0;
        if (dec_out) *dec_out = dec;
        return NULL;
    }
    if (dec_out) *dec_out = dec;

    if (!stream_init(&st, ring, RING_CAP, &dec, out_hz)) {
        printf("  (stream_init failed)\n");
        *nframes_out = 0;
        return NULL;
    }
    stream_reset(&st);
    stream_set_interp(&st, interp);

    /* Generous: the output can be longer than the input when
     * upsampling. */
    cap = (unsigned)(((double)adec_total_frames(&dec) * out_hz)
        / dec.rate) + 64;
    out = malloc((size_t)cap * 2 * sizeof(short));

    pos = dec.data_off;

    for (;;) {
        unsigned char *p;
        unsigned room = stream_write_ptr(&st, &p);
        unsigned want = room < chunk ? room : chunk;
        unsigned left = dec.data_off + dec.data_len - pos;
        unsigned got;

        if (want > left) want = left;
        if (want) {
            memcpy(p, file + pos, want);
            stream_commit(&st, want);
            pos += want;
        }
        if (pos >= dec.data_off + dec.data_len) stream_set_eof(&st, 1);

        if (n >= cap) break;
        got = stream_render(&st, out + n * 2, 128 < (cap - n) ? 128 : (cap - n));
        n += got;

        if (!got && stream_drained(&st)) break;
        if (!got && !want) break;
    }

    *nframes_out = n;
    return out;
}

/* zero crossings -> frequency */
static double measure_hz(const short *f, unsigned n, int ch_off,
    unsigned rate)
{
    unsigned i, cross = 0;
    int prev = f[ch_off];
    for (i = 1; i < n; i++) {
        int cur = f[i * 2 + ch_off];
        if ((prev < 0 && cur >= 0) || (prev >= 0 && cur < 0)) cross++;
        prev = cur;
    }
    if (n < 2) return 0.0;
    return (double)cross * rate / (2.0 * n);
}

static double measure_peak(const short *f, unsigned n, int ch_off) {
    unsigned i;
    int mx = 0;
    for (i = 0; i < n; i++) {
        int v = f[i * 2 + ch_off];
        if (v < 0) v = -v;
        if (v > mx) mx = v;
    }
    return mx / 32767.0;
}

static void dump(const char *fn, const short *f, unsigned n, unsigned rate) {
    unsigned char h[44];
    FILE *fp;
    if (!dump_wav) return;
    fp = fopen(fn, "wb");
    if (!fp) return;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + n * 4);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate);
    put32(h + 28, rate * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, n * 4);
    fwrite(h, 1, 44, fp);
    fwrite(f, 4, n, fp);
    fclose(fp);
    printf("        -> %s\n", fn);
}

/* ------------------------------------------------------------------
 * groups
 * ------------------------------------------------------------------ */

/* 1: parsing and rejection */
static void t_parse(void) {

    unsigned char data[64] = {0};
    unsigned len;
    unsigned char *w;
    adec_t a;
    int rc;

    printf("\n[1] parsing\n");

    w = make_wav(1, 2, 22050, 16, 4, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.codec == ADEC_PCM_S16LE && a.channels == 2
        && a.rate == 22050, "16-bit stereo WAV");
    check(a.block == 4 && a.bframes == 1, "PCM block geometry");
    check(a.data_len == sizeof(data), "data length");
    free(w);

    w = make_wav(1, 1, 8000, 8, 1, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.codec == ADEC_PCM_U8, "8-bit mono WAV");
    free(w);

    w = make_wav(7, 1, 8000, 8, 1, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.codec == ADEC_ULAW, "u-law WAV");
    free(w);

    w = make_wav(6, 1, 8000, 8, 1, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.codec == ADEC_ALAW, "A-law WAV");
    free(w);

    /* the odd-length LIST chunk -- see make_wav() */
    w = make_wav(1, 2, 44100, 16, 4, data, sizeof(data), &len, 1);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.rate == 44100,
        "chunk walk past an odd-sized LIST");
    free(w);

    w = make_wav(1, 2, 44100, 24, 6, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_ERR_DEPTH, "24-bit refused with the right reason");
    free(w);

    w = make_wav(3, 2, 44100, 32, 8, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_ERR_CODEC, "float refused");
    free(w);

    w = make_wav(1, 6, 44100, 16, 12, data, sizeof(data), &len, 0);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_ERR_CHANNELS, "6 channels refused");
    free(w);

    {
        unsigned char junk[64];
        memset(junk, 0xAB, sizeof(junk));
        rc = adec_parse(junk, sizeof(junk), sizeof(junk), "X.WAV", &a);
        check(rc != ADEC_OK, "garbage refused");
    }

    /* An oversized data size, as left behind by anything that was cut
     * short. Must clamp to the file, not trust the header. */
    w = make_wav(1, 2, 22050, 16, 4, data, sizeof(data), &len, 0);
    put32(w + len - sizeof(data) - 4, 0x00100000);
    rc = adec_parse(w, len, len, "X.WAV", &a);
    check(rc == ADEC_OK && a.data_len == sizeof(data),
        "oversized data size clamped to the file");
    free(w);

    /* .au */
    {
        unsigned char au[24 + 32];
        memset(au, 0, sizeof(au));
        memcpy(au, ".snd", 4);
        au[4]=0; au[5]=0; au[6]=0; au[7]=24;           /* data offset */
        au[8]=0; au[9]=0; au[10]=0; au[11]=32;         /* size */
        au[12]=0; au[13]=0; au[14]=0; au[15]=1;        /* u-law */
        au[16]=0; au[17]=0; au[18]=0x1f; au[19]=0x40;  /* 8000 */
        au[20]=0; au[21]=0; au[22]=0; au[23]=1;        /* mono */
        rc = adec_parse(au, sizeof(au), sizeof(au), "X.AU", &a);
        check(rc == ADEC_OK && a.codec == ADEC_ULAW && a.rate == 8000
            && a.channels == 1, ".au u-law 8k mono");
    }
}

/* 2: the 8-bit expansion tables */
static void t_tables(void) {

    adec_t a;
    unsigned char data[8] = {0};
    unsigned len;
    unsigned char *w;

    printf("\n[2] companding\n");

    w = make_wav(7, 1, 8000, 8, 1, data, sizeof(data), &len, 0);
    adec_parse(w, len, len, "X.WAV", &a);

    /* G.711 u-law has TWO codes for zero -- 0xFF and 0x7F -- and both
     * expand to exactly 0, not to a small positive and a small
     * negative value. The first version of this test asserted the
     * latter and failed against a correct table, which is worth
     * leaving a note about: "nearest zero" is 0xFE and 0x7E.
     *
     * The sign of those two is the assertion that matters. The
     * encoding is inverted, and getting the inversion backwards
     * produces audio that sounds entirely correct and is
     * phase-flipped -- which nobody notices until it is summed with
     * something else. */
    check(a.tab8[0xFF] == 0 && a.tab8[0x7F] == 0, "u-law both zero codes");
    check(a.tab8[0xFE] > 0 && a.tab8[0xFE] < 64, "u-law 0xFE is small +");
    check(a.tab8[0x7E] < 0 && a.tab8[0x7E] > -64, "u-law 0x7E is small -");
    check(a.tab8[0x00] < -30000, "u-law 0x00 ~ -full");
    check(a.tab8[0x80] > 30000, "u-law 0x80 ~ +full");

    /* Monotonic within each sign half. A transcription error in the
     * segment shift shows up here and nowhere else. */
    {
        int i, mono = 1;
        for (i = 0x80; i < 0xFF; i++)
            if (a.tab8[i] < a.tab8[i + 1]) mono = 0;
        check(mono, "u-law positive half is monotonic");
    }

    /* Volume folds into the table: half gain, half amplitude. */
    {
        int full = a.tab8[0x80];
        adec_set_gain(&a, ADEC_GAIN_UNITY / 2);
        check_near(a.tab8[0x80] / (double)full, 0.5, 0.02,
            "gain folded into the table");
        adec_set_gain(&a, ADEC_GAIN_UNITY);
    }
    free(w);

    w = make_wav(1, 1, 8000, 8, 1, data, sizeof(data), &len, 0);
    adec_parse(w, len, len, "X.WAV", &a);
    /* WAV 8-bit is UNSIGNED. 0x80 is silence; if this comes out at
     * full scale the file plays with a DC offset. */
    check(a.tab8[0x80] == 0, "WAV 8-bit 0x80 is silence");
    check(a.tab8[0x00] < -30000 && a.tab8[0xFF] > 30000,
        "WAV 8-bit spans full scale");
    free(w);
}

/* 3: pitch through the resampler */
static void t_pitch(void) {

    struct { unsigned src; unsigned out; } cases[] = {
        { 44100, 44118 },     /* the default board rate */
        { 22050, 44118 },     /* 2x up */
        { 8000,  44118 },     /* awkward ratio up */
        { 48000, 44118 },     /* down */
        { 44100, 46875 },     /* an S/PDIF board */
        { 11025, 22059 },
    };
    unsigned i;

    printf("\n[3] pitch\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned dlen, flen, n;
        unsigned char *pcm = sine_s16(cases[i].src * 2, 2, cases[i].src,
            440.0, 0.8, &dlen);
        unsigned char *w = make_wav(1, 2, cases[i].src, 16, 4, pcm, dlen,
            &flen, 0);
        short *out = render_all(w, flen, "T.WAV", cases[i].out, 1, &n,
            NULL, 512);
        char name[64];

        if (out && n > 1000) {
            double hz = measure_hz(out, n, 0, cases[i].out);
            snprintf(name, sizeof(name), "%u -> %u Hz keeps 440Hz",
                cases[i].src, cases[i].out);
            check_near(hz, 440.0, 6.0, name);

            snprintf(name, sizeof(name), "%u -> %u duration",
                cases[i].src, cases[i].out);
            check_near((double)n / cases[i].out, 2.0, 0.02, name);

            snprintf(name, sizeof(name), "res-%u-%u.wav",
                cases[i].src, cases[i].out);
            dump(name, out, n, cases[i].out);
        } else {
            check(0, "render produced nothing");
        }
        free(out); free(w); free(pcm);
    }
}

/* 4: amplitude and volume */
static void t_level(void) {

    unsigned dlen, flen, n;
    unsigned char *pcm = sine_s16(22050, 2, 22050, 440.0, 0.5, &dlen);
    unsigned char *w = make_wav(1, 2, 22050, 16, 4, pcm, dlen, &flen, 0);
    adec_t dec;
    short *out;

    printf("\n[4] level\n");

    out = render_all(w, flen, "T.WAV", 44118, 1, &n, &dec, 512);
    if (out && n) {
        check_near(measure_peak(out, n, 0), 0.5, 0.02, "unity gain");
        free(out);
    } else check(0, "render");

    /* Same file at half volume. Rendered through the whole chain, not
     * by inspecting the table, so this covers the 16-bit path where
     * the gain is a multiply rather than a table. */
    {
        static unsigned char ring[RING_CAP];
        stream_t st;
        unsigned pos, i;
        short blk[256];
        double pk = 0;

        adec_parse(w, flen, flen, "T.WAV", &dec);
        adec_set_gain(&dec, ADEC_GAIN_UNITY / 4);
        stream_init(&st, ring, RING_CAP, &dec, 44118);
        stream_reset(&st);
        pos = dec.data_off;

        for (;;) {
            unsigned char *p;
            unsigned room = stream_write_ptr(&st, &p);
            unsigned left = dec.data_off + dec.data_len - pos;
            unsigned want = room < 512 ? room : 512;
            unsigned got;
            if (want > left) want = left;
            if (want) { memcpy(p, w + pos, want); stream_commit(&st, want); pos += want; }
            if (pos >= dec.data_off + dec.data_len) stream_set_eof(&st, 1);
            got = stream_render(&st, blk, 128);
            for (i = 0; i < got; i++) {
                double v = fabs(blk[i * 2] / 32767.0);
                if (v > pk) pk = v;
            }
            if (!got && stream_drained(&st)) break;
            if (!got && !want) break;
        }
        check_near(pk, 0.125, 0.01, "quarter gain on the 16-bit path");
    }

    free(w); free(pcm);
}

/* 5: IMA ADPCM round trip */
static void t_ima(void) {

    unsigned ch;

    printf("\n[5] IMA ADPCM\n");

    for (ch = 1; ch <= 2; ch++) {

        unsigned frames = 22050;
        unsigned i, c, alen, flen, n, encframes;
        short *pcm = malloc(frames * ch * sizeof(short));
        unsigned char *adpcm, *w;
        short *out;
        adec_t dec;
        char name[64];

        for (i = 0; i < frames; i++) {
            int v = (int)(0.7 * 32767.0
                * sin(2.0 * M_PI * 440.0 * i / 22050.0));
            for (c = 0; c < ch; c++) pcm[i * ch + c] = (short)v;
        }

        adpcm = ima_encode(pcm, frames, ch, 512, &alen, &encframes);
        w = make_wav(0x11, ch, 22050, 4, 512, adpcm, alen, &flen, 0);

        out = render_all(w, flen, "T.WAV", 44118, 1, &n, &dec, 512);

        snprintf(name, sizeof(name), "%uch parsed as IMA", ch);
        check(dec.codec == ADEC_IMA_WAV && dec.block == 512, name);

        snprintf(name, sizeof(name), "%uch block holds %u frames", ch,
            (unsigned)dec.bframes);
        check(dec.bframes == 1u + ((512u - 4u * ch) * 2u) / ch, name);

        if (out && n > 1000) {
            snprintf(name, sizeof(name), "%uch IMA pitch", ch);
            check_near(measure_hz(out, n, 0, 44118), 440.0, 6.0, name);

            /* IMA is lossy; the peak should be close but need not be
             * exact. A LARGE error here is the signature of stereo
             * de-interleaving done wrong -- the audio survives and the
             * envelope does not. */
            snprintf(name, sizeof(name), "%uch IMA level", ch);
            check_near(measure_peak(out, n, 0), 0.7, 0.08, name);

            /* Both channels carry the same tone in this fixture, so
             * they must match closely. This is the assertion that
             * actually catches 4-byte-group interleaving errors. */
            if (ch == 2) {
                unsigned k;
                double err = 0;
                for (k = 100; k < n && k < 5000; k++)
                    err += fabs((double)out[k * 2] - (double)out[k * 2 + 1]);
                err /= (double)(n < 5000 ? n - 100 : 4900);
                check(err < 64.0, "IMA stereo channels agree");
            }

            snprintf(name, sizeof(name), "ima-%uch.wav", ch);
            dump(name, out, n, 44118);
        } else check(0, "IMA render produced nothing");

        free(out); free(w); free(adpcm); free(pcm);
    }
}

/* 6: the ring, at chunk sizes that force wraps and straddles */
static void t_ring(void) {

    unsigned chunks[] = { 3, 7, 64, 512, 1000, 4096 };
    unsigned i;

    printf("\n[6] ring behaviour\n");

    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        unsigned dlen, flen, n;
        unsigned char *pcm = sine_s16(22050, 2, 22050, 440.0, 0.8, &dlen);
        unsigned char *w = make_wav(1, 2, 22050, 16, 4, pcm, dlen, &flen, 0);
        short *out = render_all(w, flen, "T.WAV", 44118, 1, &n, NULL,
            chunks[i]);
        char name[64];

        snprintf(name, sizeof(name), "chunk %u: full duration", chunks[i]);
        if (out) check_near((double)n / 44118.0, 1.0, 0.01, name);
        else check(0, name);

        snprintf(name, sizeof(name), "chunk %u: pitch", chunks[i]);
        if (out && n > 1000)
            check_near(measure_hz(out, n, 0, 44118), 440.0, 6.0, name);
        else check(0, name);

        free(out); free(w); free(pcm);
    }

    /* An IMA block that straddles the ring wrap, forced by feeding in
     * chunks that are coprime with the block size. This is the path
     * scratch_fill()'s bounce buffer exists for, and without it the
     * player stalls silently with a full ring -- which looks exactly
     * like an SD problem. */
    {
        unsigned frames = 22050, i2, alen, flen, n, encframes;
        short *pcm = malloc(frames * sizeof(short));
        unsigned char *adpcm, *w;
        short *out;
        for (i2 = 0; i2 < frames; i2++)
            pcm[i2] = (short)(0.7 * 32767.0
                * sin(2.0 * M_PI * 440.0 * i2 / 22050.0));
        adpcm = ima_encode(pcm, frames, 1, 512, &alen, &encframes);
        w = make_wav(0x11, 1, 22050, 4, 512, adpcm, alen, &flen, 0);
        out = render_all(w, flen, "T.WAV", 44118, 1, &n, NULL, 300);
        check(out && n > 40000, "IMA block straddling the ring wrap");
        free(out); free(w); free(adpcm); free(pcm);
    }
}

/* 7: interpolation on vs off */
static void t_interp(void) {

    unsigned dlen, flen, n1, n2;
    unsigned char *pcm = sine_s16(8000 * 2, 2, 8000, 440.0, 0.8, &dlen);
    unsigned char *w = make_wav(1, 2, 8000, 16, 4, pcm, dlen, &flen, 0);
    short *a, *b;

    printf("\n[7] interpolation\n");

    a = render_all(w, flen, "T.WAV", 44118, 1, &n1, NULL, 512);
    b = render_all(w, flen, "T.WAV", 44118, 0, &n2, NULL, 512);

    check(n1 == n2, "same frame count either way");

    /* Both must be in tune -- interpolation is about noise, not
     * pitch, and a version that changed pitch would mean the phase
     * accumulator had been touched by the switch. */
    if (a && b && n1 > 1000 && n2 > 1000) {
        check_near(measure_hz(a, n1, 0, 44118), 440.0, 6.0,
            "interpolated pitch");
        check_near(measure_hz(b, n2, 0, 44118), 440.0, 6.0,
            "nearest-neighbour pitch");
        dump("interp-on.wav", a, n1, 44118);
        dump("interp-off.wav", b, n2, 44118);
    } else check(0, "interp render");

    free(a); free(b); free(w); free(pcm);
}

/* 8: seek arithmetic */
static void t_seek(void) {

    unsigned dlen, flen;
    unsigned char *pcm = sine_s16(44100, 2, 44100, 440.0, 0.5, &dlen);
    unsigned char *w = make_wav(1, 2, 44100, 16, 4, pcm, dlen, &flen, 0);
    adec_t a;

    printf("\n[8] seeking\n");

    adec_parse(w, flen, flen, "T.WAV", &a);
    check(adec_total_frames(&a) == 44100, "total frames");
    check(adec_seek_offset(&a, 0) == a.data_off, "seek to 0");
    check(adec_seek_offset(&a, 22050) == a.data_off + 22050 * 4,
        "seek to the middle");
    check(adec_frame_at_offset(&a, adec_seek_offset(&a, 12345)) == 12345,
        "seek round trip");

    /* Past the end clamps INSIDE the file rather than to its end --
     * see adec_seek_offset()'s comment on why rounding down matters. */
    check(adec_seek_offset(&a, 999999) < a.data_off + a.data_len,
        "seek past the end clamps inside");
    free(w); free(pcm);

    /* IMA: every seek must land on a block boundary or the predictor
     * comes from the wrong place and the block decodes as a burst of
     * noise. */
    {
        unsigned frames = 22050, i, alen, flen2, encframes;
        short *p = calloc(frames, sizeof(short));
        unsigned char *adpcm, *w2;
        adec_t b;
        for (i = 0; i < frames; i++) p[i] = (short)(i * 7);
        adpcm = ima_encode(p, frames, 1, 512, &alen, &encframes);
        w2 = make_wav(0x11, 1, 22050, 4, 512, adpcm, alen, &flen2, 0);
        adec_parse(w2, flen2, flen2, "T.WAV", &b);
        check(((adec_seek_offset(&b, 5000) - b.data_off) % b.block) == 0,
            "IMA seek lands on a block boundary");
        free(w2); free(adpcm); free(p);
    }
}

/* 9: starvation is reported, not papered over */
static void t_starve(void) {

    static unsigned char ring[RING_CAP];
    unsigned dlen, flen;
    unsigned char *pcm = sine_s16(22050, 2, 22050, 440.0, 0.8, &dlen);
    unsigned char *w = make_wav(1, 2, 22050, 16, 4, pcm, dlen, &flen, 0);
    adec_t dec;
    stream_t st;
    short blk[512];
    unsigned got;

    printf("\n[9] starvation\n");

    adec_parse(w, flen, flen, "T.WAV", &dec);
    stream_init(&st, ring, RING_CAP, &dec, 44118);
    stream_reset(&st);

    /* Ask for output with an empty ring. Must return 0 and say so,
     * rather than emitting zeros (a click) or the last frame (a
     * tone). */
    got = stream_render(&st, blk, 256);
    check(got == 0 && st.starved == 1, "empty ring renders nothing");
    check(!stream_drained(&st), "an empty ring is not end-of-file");

    /* Feed a little, ask for a lot. */
    {
        unsigned char *p;
        unsigned room = stream_write_ptr(&st, &p);
        unsigned want = room < 400 ? room : 400;
        memcpy(p, w + dec.data_off, want);
        stream_commit(&st, want);
        got = stream_render(&st, blk, 256);
        check(got > 0 && got < 256, "partial render on a short ring");
        check(st.starved == 2, "starvation counted");
    }

    free(w); free(pcm);
}

int main(int argc, char **argv) {

    int i;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-w")) dump_wav = 1;

    printf("play engine tests\n");

    t_parse();
    t_tables();
    t_pitch();
    t_level();
    t_ima();
    t_ring();
    t_interp();
    t_seek();
    t_starve();

    printf("\n%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
