/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * synth -- see synth.h.
 */

#include <string.h>

#include "synth.h"

/* ------------------------------------------------------------------
 * waveform bank
 * ------------------------------------------------------------------ */

/*
 * Harmonic limit per mip level.
 *
 * Chosen against what actually fits: at 46875Hz output, middle C has
 * room for 89 harmonics, C6 for 22 and C7 for 11. Three levels of
 * 32 / 12 / 4 bracket the musically-used range, and note_mip() picks
 * the highest level whose limit still fits below Nyquist -- erring
 * toward duller rather than brighter, because being one level too dull
 * costs a little sparkle and being one level too bright puts
 * inharmonic noise back.
 */
static const int mip_harm[SYNTH_MIPS] = { 32, 12, 4 };

int synth_mip_harmonics(int mip) {
    if (mip < 0) mip = 0;
    if (mip >= SYNTH_MIPS) mip = SYNTH_MIPS - 1;
    return mip_harm[mip];
}

uint32_t synth_wave_length(int wave, int bits) {
    uint32_t n = (wave == SYNTH_W_NOISE) ? SYNTH_NOISE_LEN : SYNTH_WAVE_LEN;
    return n * (uint32_t)(bits / 8);
}

uint32_t synth_wave_offset(int wave, int mip, int bits) {
    uint32_t w = (uint32_t)(bits / 8);
    if (wave >= SYNTH_W_NOISE)
        return (uint32_t)SYNTH_PERIODIC * SYNTH_MIPS * SYNTH_WAVE_LEN * w;
    if (mip < 0) mip = 0;
    if (mip >= SYNTH_MIPS) mip = SYNTH_MIPS - 1;
    return ((uint32_t)wave * SYNTH_MIPS + (uint32_t)mip)
        * SYNTH_WAVE_LEN * w;
}

/* ------------------------------------------------------------------
 * waveform bank
 * ------------------------------------------------------------------ */

/*
 * A quarter-period sine table, in Q7.
 *
 * 64 entries reflected into 256 rather than 256 stored: a quarter of
 * the data for identical output, and the reflection is two compares.
 * Written out rather than computed because there is no sin() here --
 * this file has no libm and wants none. It is also the only primitive
 * the additive synthesis below needs.
 */
static const uint8_t sine_q[65] = {
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,
     43,  46,  49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,
     81,  83,  85,  88,  90,  92,  94,  96,  98, 100, 102, 104, 106, 108,
    109, 111, 112, 114, 115, 117, 118, 119, 120, 121, 122, 123, 124, 124,
    125, 126, 126, 127, 127, 127, 127, 127, 127
};

static int sine_at(int i) {
    i &= (SYNTH_WAVE_LEN - 1);
    if (i < 64) return (int)sine_q[i];
    if (i < 128) return (int)sine_q[128 - i];
    if (i < 192) return -(int)sine_q[i - 128];
    return -(int)sine_q[256 - i];
}

/*
 * Band-limited sawtooth, by additive synthesis.
 *
 * sum over n = 1..H of sin(n * x) / n, which is the Fourier series of
 * a saw with everything above the Hth harmonic simply absent. Scaled
 * to fill the output range afterwards rather than normalised
 * analytically, because the peak of a truncated series overshoots
 * (Gibbs) by an amount that depends on H.
 *
 * Accumulated in Q12 so that the 1/n terms of the high harmonics do
 * not vanish into rounding before they are summed -- at H = 32 the
 * last term is 1/32 of the first, which in Q7 would be three counts.
 */
static void build_saw(int32_t *out, int harm) {
    int i, n;
    for (i = 0; i < SYNTH_WAVE_LEN; i++) out[i] = 0;
    for (n = 1; n <= harm; n++)
        for (i = 0; i < SYNTH_WAVE_LEN; i++)
            out[i] += (sine_at(i * n) << 5) / n;
}

/* Band-limited triangle: odd harmonics only, falling as 1/n^2, with
 * alternating sign. Much weaker high content than a saw, which is why
 * a naive triangle aliases far less -- but it is free to do properly
 * once the machinery exists. */
static void build_tri(int32_t *out, int harm) {
    int i, n, k = 0;
    for (i = 0; i < SYNTH_WAVE_LEN; i++) out[i] = 0;
    for (n = 1; n <= harm; n += 2, k++) {
        int sign = (k & 1) ? -1 : 1;
        for (i = 0; i < SYNTH_WAVE_LEN; i++)
            out[i] += sign * ((sine_at(i * n) << 5) / (n * n));
    }
}

/*
 * Band-limited pulse, as the difference of two phase-shifted saws.
 *
 * The classic trick, and the reason it is used here rather than
 * summing a pulse series directly: the difference of two copies of the
 * SAME band-limited waveform is automatically band-limited to the same
 * harmonic, and automatically free of DC.
 *
 * That second property matters more than it sounds. The first version
 * of this file built pulses as literal high/low levels, and a 25%
 * pulse swinging equally either way spends three quarters of its
 * period negative -- a large DC offset. On one chip voice that is
 * harmless; here EIGHT sum into one accumulator and one DAC, so the
 * offsets add. Deriving the pulse from a saw makes the problem
 * impossible rather than merely fixed.
 */
static void build_pulse(int32_t *out, const int32_t *saw, int duty256) {
    int i;
    /* Shifted MINUS unshifted, in that order. The Fourier sum
     * sum sin(nx)/n is a FALLING ramp, so subtracting the other way
     * round yields a pulse whose duty is 1 - d: asking for 25% got
     * 75%. Sonically identical, since a pulse and its inverse have the
     * same harmonic content -- but a table named PULSE25 that is 75%
     * is a trap for whoever tunes the GM mapping next. */
    for (i = 0; i < SYNTH_WAVE_LEN; i++)
        out[i] = saw[(i + duty256) & (SYNTH_WAVE_LEN - 1)] - saw[i];
}

/* Scale to fill the sample width and write it out. */
static void emit(void *bank, uint32_t off_bytes, const int32_t *src,
    int n, int bits)
{
    int32_t peak = 1;
    int i;
    int32_t full = (bits == 16) ? 32000 : 125;

    for (i = 0; i < n; i++) {
        int32_t a = src[i] < 0 ? -src[i] : src[i];
        if (a > peak) peak = a;
    }

    if (bits == 16) {
        int16_t *d = (int16_t *)((uint8_t *)bank + off_bytes);
        for (i = 0; i < n; i++) d[i] = (int16_t)((src[i] * full) / peak);
    } else {
        int8_t *d = (int8_t *)((uint8_t *)bank + off_bytes);
        for (i = 0; i < n; i++) d[i] = (int8_t)((src[i] * full) / peak);
    }
}

void synth_build_bank(void *bank, int bits) {

    static int32_t tmp[SYNTH_WAVE_LEN];
    static int32_t saw[SYNTH_WAVE_LEN];
    int mip, i;
    uint32_t lfsr = 0xACE1u;

    if (bits != 16) bits = 8;

    for (mip = 0; mip < SYNTH_MIPS; mip++) {

        int harm = mip_harm[mip];

        /* sine: one harmonic, whatever the mip -- there is nothing to
         * limit. The levels are kept so the indexing stays uniform. */
        for (i = 0; i < SYNTH_WAVE_LEN; i++) tmp[i] = sine_at(i);
        emit(bank, synth_wave_offset(SYNTH_W_SINE, mip, bits), tmp,
            SYNTH_WAVE_LEN, bits);

        build_tri(tmp, harm);
        emit(bank, synth_wave_offset(SYNTH_W_TRI, mip, bits), tmp,
            SYNTH_WAVE_LEN, bits);

        build_saw(saw, harm);
        emit(bank, synth_wave_offset(SYNTH_W_SAW, mip, bits), saw,
            SYNTH_WAVE_LEN, bits);

        /*
         * Three duty cycles. Duty is the cheapest timbre control there
         * is, and 12.5% against 50% is the difference between a reedy
         * lead and a hollow one.
         */
        build_pulse(tmp, saw, SYNTH_WAVE_LEN / 2);
        emit(bank, synth_wave_offset(SYNTH_W_PULSE50, mip, bits), tmp,
            SYNTH_WAVE_LEN, bits);
        build_pulse(tmp, saw, SYNTH_WAVE_LEN / 4);
        emit(bank, synth_wave_offset(SYNTH_W_PULSE25, mip, bits), tmp,
            SYNTH_WAVE_LEN, bits);
        build_pulse(tmp, saw, SYNTH_WAVE_LEN / 8);
        emit(bank, synth_wave_offset(SYNTH_W_PULSE12, mip, bits), tmp,
            SYNTH_WAVE_LEN, bits);
    }

    /*
     * Noise, from a 16-bit Galois LFSR. No band-limiting: noise has no
     * harmonic structure to fold, and a filtered noise table is just a
     * quieter noise table.
     *
     * Deterministic on purpose -- a drum track that sounds different
     * every run is impossible to compare against itself while tuning
     * the envelopes.
     */
    {
        uint32_t off = synth_wave_offset(SYNTH_W_NOISE, 0, bits);
        for (i = 0; i < SYNTH_NOISE_LEN; i++) {
            int b;
            for (b = 0; b < 8; b++) {
                uint32_t lsb = lfsr & 1u;
                lfsr >>= 1;
                if (lsb) lfsr ^= 0xB400u;
            }
            if (bits == 16) {
                int16_t *d = (int16_t *)((uint8_t *)bank + off);
                d[i] = (int16_t)((int)((lfsr & 0xffff)) - 32768);
            } else {
                int8_t *d = (int8_t *)((uint8_t *)bank + off);
                d[i] = (int8_t)((int)(lfsr & 0xff) - 128);
            }
        }
    }
}

/* ------------------------------------------------------------------
 * pitch
 * ------------------------------------------------------------------ */

/*
 * Frequency of C8..B8 in Q8 Hz -- MIDI notes 108..119.
 *
 * Every other note is this table shifted by octaves, which is exact:
 * an octave is a factor of two and a factor of two is a shift. Storing
 * all 128 would be four times the data to say the same thing less
 * accurately at the bottom.
 */
static const uint32_t note_q8[12] = {
    1071618,  /* C8   4186.01 Hz */
    1135340,  /* C#8  4434.92 Hz */
    1202851,  /* D8   4698.64 Hz */
    1274376,  /* D#8  4978.03 Hz */
    1350154,  /* E8   5274.04 Hz */
    1430439,  /* F8   5587.65 Hz */
    1515497,  /* F#8  5919.91 Hz */
    1605613,  /* G8   6271.93 Hz */
    1701088,  /* G#8  6644.88 Hz */
    1802240,  /* A8   7040.00 Hz */
    1909407,  /* A#8  7458.62 Hz */
    2022946   /* B8   7902.13 Hz */
};

/* Semitone ratios in Q12, for pitch bend within +/- 2 semitones. */
static const uint16_t bend_q12[25] = {
    3649, 3760, 3877, 3996, 4119, 4246, 4377, 4512, 4651, 4794, 4942,
    5094, 5251, 5413, 5580, 5752, 5929, 6112, 6301, 6495, 6695, 6902,
    7115, 7334, 7561
};

static uint32_t note_freq_q8(int note) {
    int oct, idx;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    oct = note / 12;
    idx = note % 12;
    /* The table is octave 9 in this numbering (note 108 = 9 * 12). */
    if (oct <= 9) return note_q8[idx] >> (9 - oct);
    return note_q8[idx] << (oct - 9);
}

/*
 * CH_STEP for a note, in the mixer's 18.14 bytes-per-output-frame.
 *
 * step = WAVE_LEN * f / out_hz, and with WAVE_LEN a power of two the
 * whole thing folds into one multiply against a constant computed
 * once at init:
 *
 *     step = f_q8 * step_k >> 16,   step_k = (WAVE_LEN << 14) * 256 / out_hz
 *
 * Done that way round for a specific reason: the direct form,
 * (WAVE_LEN * f << 14) / out_hz, OVERFLOWS 32 bits above about
 * 2 kHz -- which is the top two octaves of a piano, not some exotic
 * corner. It would have worked in every test written with middle C.
 */
static uint32_t note_step(const synth_t *sy, int note, int16_t bend) {

    uint32_t f = note_freq_q8(note);

    if (bend) {
        /* +/- 2 semitones, the GM default range. Quantised to
         * quarter-tones via a 25-entry table: a continuous bend would
         * need a per-frame update the mixer cannot take anyway, since
         * CH_STEP only changes when the CPU writes it. */
        int idx = 12 + ((int)bend * 12) / 8192;
        if (idx < 0) idx = 0;
        if (idx > 24) idx = 24;
        f = (f >> 6) * bend_q12[idx] >> 6;
    }

    /*
     * step = f_q8 * 16384 / out_hz, split so the numerator never
     * overflows.
     *
     * The direct product is f * 256 * 16384, which at the top of the
     * MIDI range is 5.3e10 -- it does not fit, and it would have
     * worked in any test written around middle C. Splitting into
     * quotient and remainder keeps every intermediate under 8e8 and
     * is EXACT, not approximate: no 64-bit arithmetic, and no
     * precision lost at the bottom of the range where a rounded step
     * is audibly out of tune.
     */
    {
        /* CH_STEP counts BYTES per output frame, so a 16-bit table
         * advances twice as far per sample as an 8-bit one. Forgetting
         * this plays everything an octave low, which is the kind of
         * mistake that sounds like a broken tuning table rather than a
         * units error. */
        uint32_t w = (uint32_t)(sy->bits / 8);
        uint32_t s = (f / sy->out_hz) * 16384u
            + ((f % sy->out_hz) * 16384u) / sy->out_hz;
        return s * w;
    }
}

/* ------------------------------------------------------------------
 * General MIDI mapping
 *
 * One entry per GM family of eight programs. Sixteen families is not
 * a lot, but it is what keeps a bass line, a lead and a pad from
 * sounding identical -- which is the difference between a
 * transcription and mush.
 *
 * Attack, decay and release are in milliseconds; sustain is the
 * fraction of peak held while the key is down, in 1/256ths.
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t  wave;
    uint16_t attack_ms;
    uint16_t decay_ms;
    uint8_t  sustain;      /* 0..255 */
    uint16_t release_ms;
    const char *name;
} gm_family_t;

static const gm_family_t gm_family[16] = {
    /*   0 piano      */ { SYNTH_W_TRI,     2,  900,  40,  150, "piano" },
    /*   8 chrom perc */ { SYNTH_W_SINE,    1,  500,   0,  200, "chime" },
    /*  16 organ      */ { SYNTH_W_PULSE50, 8,   40, 230,   60, "organ" },
    /*  24 guitar     */ { SYNTH_W_SAW,     3,  700,  60,  180, "guitar" },
    /*  32 bass       */ { SYNTH_W_TRI,     4,  600, 110,  120, "bass" },
    /*  40 strings    */ { SYNTH_W_SAW,   120,  300, 200,  300, "strings" },
    /*  48 ensemble   */ { SYNTH_W_SAW,   160,  400, 205,  350, "ensemble" },
    /*  56 brass      */ { SYNTH_W_PULSE25,40,  250, 190,  180, "brass" },
    /*  64 reed       */ { SYNTH_W_PULSE25,30,  200, 200,  160, "reed" },
    /*  72 pipe       */ { SYNTH_W_SINE,   25,  200, 210,  150, "pipe" },
    /*  80 synth lead */ { SYNTH_W_PULSE12, 5,  300, 170,  120, "lead" },
    /*  88 synth pad  */ { SYNTH_W_SAW,   250,  500, 200,  500, "pad" },
    /*  96 synth fx   */ { SYNTH_W_SINE,  150,  600, 120,  400, "fx" },
    /* 104 ethnic     */ { SYNTH_W_SAW,     4,  600,  70,  150, "ethnic" },
    /* 112 percussive */ { SYNTH_W_TRI,     1,  350,   0,  120, "perc" },
    /* 120 sound fx   */ { SYNTH_W_NOISE,   1,  400,   0,  150, "sfx" }
};

/*
 * GM percussion, channel 10.
 *
 * Not a full 47-note map. What matters is that the four sounds
 * carrying most rhythm parts -- kick, snare, closed hat, open hat --
 * are distinguishable, and that everything else lands somewhere
 * plausible rather than silent. `pitch` is the MIDI note the waveform
 * is played AT, which for noise is a playback rate rather than a
 * pitch.
 */
typedef struct {
    uint8_t lo, hi;        /* inclusive GM note range */
    uint8_t wave;
    uint8_t pitch;
    uint16_t decay_ms;
} gm_drum_t;

static const gm_drum_t gm_drum[] = {
    { 35, 36, SYNTH_W_SINE,  33,  140 },   /* kick */
    { 37, 40, SYNTH_W_NOISE, 62,  120 },   /* snare / rim */
    { 41, 48, SYNTH_W_TRI,   45,  180 },   /* toms */
    { 42, 42, SYNTH_W_NOISE, 84,   40 },   /* closed hat */
    { 44, 44, SYNTH_W_NOISE, 84,   50 },   /* pedal hat */
    { 46, 46, SYNTH_W_NOISE, 82,  320 },   /* open hat */
    { 49, 53, SYNTH_W_NOISE, 76,  600 },   /* crash / ride */
    { 54, 81, SYNTH_W_NOISE, 88,   90 }    /* everything else */
};

static const gm_drum_t *drum_for(uint8_t note) {
    unsigned i;
    for (i = 0; i < sizeof(gm_drum) / sizeof(gm_drum[0]); i++)
        if (note >= gm_drum[i].lo && note <= gm_drum[i].hi)
            return &gm_drum[i];
    return &gm_drum[sizeof(gm_drum) / sizeof(gm_drum[0]) - 1];
}

const char *synth_family_name(uint8_t program) {
    return gm_family[(program >> 3) & 15].name;
}

/* ------------------------------------------------------------------
 * voices
 * ------------------------------------------------------------------ */

/*
 * Which mip level a note should use.
 *
 * The number of harmonics that fit below the output Nyquist is
 * out_hz / (2 * f). Pick the highest-numbered level whose limit is
 * still within that, i.e. the DULLEST table that is not brighter than
 * the note can carry -- because being one level too dull costs a
 * little sparkle, and being one level too bright puts the fold-back
 * noise straight back.
 */
static int note_mip(const synth_t *sy, int note) {
    uint32_t f = note_freq_q8(note) >> 8;      /* Hz */
    uint32_t fits;
    int m;
    if (!f) f = 1;
    fits = sy->out_hz / (2u * f);
    for (m = 0; m < SYNTH_MIPS; m++)
        if ((uint32_t)mip_harm[m] <= fits) return m;
    return SYNTH_MIPS - 1;
}

void synth_init(synth_t *sy, uint32_t out_hz, int bits) {
    memset(sy, 0, sizeof(*sy));
    sy->out_hz = out_hz ? out_hz : 44100;
    sy->bits = (bits == 16) ? 16 : 8;
    sy->master = 48;
    synth_reset(sy);
}

void synth_reset(synth_t *sy) {
    int i;
    memset(sy->voice, 0, sizeof(sy->voice));
    for (i = 0; i < SMF_CHANNELS; i++) {
        sy->program[i] = 0;
        sy->volume[i] = 100;
        sy->expression[i] = 127;
        sy->pan[i] = 64;
        sy->bend[i] = 0;
    }
    sy->age = 0;
    sy->stolen = 0;
}

void synth_set_master(synth_t *sy, uint8_t v) {
    sy->master = (v > 64) ? 64 : v;
}

void synth_all_off(synth_t *sy) {
    int i;
    for (i = 0; i < SYNTH_VOICES; i++) {
        sy->voice[i].active = false;
        sy->voice[i].stage = ENV_OFF;
        sy->voice[i].level = 0;
        sy->voice[i].gain_l = 0;
        sy->voice[i].gain_r = 0;
    }
}

int synth_active(const synth_t *sy) {
    int i, n = 0;
    for (i = 0; i < SYNTH_VOICES; i++)
        if (sy->voice[i].stage != ENV_OFF) n++;
    return n;
}

/*
 * Pick a voice for a new note.
 *
 * Order matters and it is the difference between "some notes are
 * missing" and "the melody keeps dropping out":
 *
 *   1. a free voice
 *   2. the quietest voice already in RELEASE -- it is on its way out
 *      anyway, and cutting a decaying tail is nearly inaudible
 *   3. the quietest sounding voice, oldest breaking the tie
 *
 * Stealing the OLDEST sounding voice instead is the obvious policy and
 * is worse: in a held chord under a moving melody, the oldest voice is
 * usually a chord tone that is still needed, while the quietest is
 * whatever has already decayed furthest.
 */
static synth_voice_t *alloc_voice(synth_t *sy) {

    int i, best = -1;
    int32_t best_level = 0;

    for (i = 0; i < SYNTH_VOICES; i++)
        if (sy->voice[i].stage == ENV_OFF) return &sy->voice[i];

    for (i = 0; i < SYNTH_VOICES; i++) {
        if (sy->voice[i].stage != ENV_RELEASE) continue;
        if (best < 0 || sy->voice[i].level < best_level) {
            best = i;
            best_level = sy->voice[i].level;
        }
    }
    if (best >= 0) return &sy->voice[best];

    for (i = 0; i < SYNTH_VOICES; i++) {
        if (best < 0 || sy->voice[i].level < best_level
            || (sy->voice[i].level == best_level
                && sy->voice[i].age < sy->voice[best].age)) {
            best = i;
            best_level = sy->voice[i].level;
        }
    }

    sy->stolen++;
    return &sy->voice[best];
}

static void note_on(synth_t *sy, uint8_t ch, uint8_t note, uint8_t vel) {

    synth_voice_t *v;
    int wave;
    uint32_t off;

    if (sy->muted[ch]) return;

    v = alloc_voice(sy);

    v->active = true;
    v->channel = ch;
    v->note = note;
    v->velocity = vel;
    v->age = ++sy->age;
    v->stage = ENV_ATTACK;
    v->level = 0;
    v->retrigger = true;

    if (ch == SMF_DRUM_CHANNEL) {
        const gm_drum_t *d = drum_for(note);
        wave = d->wave;
        v->mip = (uint8_t)note_mip(sy, d->pitch);
        v->step = note_step(sy, d->pitch, 0);
    } else {
        const gm_family_t *f = &gm_family[(sy->program[ch] >> 3) & 15];
        wave = f->wave;
        v->mip = (uint8_t)note_mip(sy, note);
        v->step = note_step(sy, note, sy->bend[ch]);
    }

    v->wave = (uint8_t)wave;
    off = synth_wave_offset(wave, v->mip, sy->bits);
    v->base = off;
    v->length = synth_wave_length(wave, sy->bits);

    /*
     * Everything loops, including the noise.
     *
     * A one-shot voice would stop when the table ran out, which for a
     * 256-byte table at middle C is about 6 milliseconds. The envelope
     * is what ends a note here, not the sample length -- so the table
     * loops forever and the release brings it down.
     */
    v->loop_start = 0;
    v->loop_len = v->length;
}

static void note_off(synth_t *sy, uint8_t ch, uint8_t note) {
    int i;
    for (i = 0; i < SYNTH_VOICES; i++) {
        synth_voice_t *v = &sy->voice[i];
        if (v->stage == ENV_OFF || v->stage == ENV_RELEASE) continue;
        if (v->channel == ch && v->note == note) {
            v->stage = ENV_RELEASE;
            /* Not `break`: the same note can legitimately be sounding
             * twice on one channel if the file retriggered it without
             * an intervening note-off, and leaving one of them on is
             * how a file ends with a note still droning. */
        }
    }
}

void synth_event(synth_t *sy, const smf_event_t *ev) {

    uint8_t ch = ev->channel & 15;

    switch (ev->type) {

        case SMF_EV_NOTE_ON:
            note_on(sy, ch, ev->a, ev->b);
            break;

        case SMF_EV_NOTE_OFF:
            note_off(sy, ch, ev->a);
            break;

        case SMF_EV_PROGRAM:
            sy->program[ch] = ev->a;
            break;

        case SMF_EV_PITCH:
            sy->bend[ch] = ev->bend;
            break;

        case SMF_EV_CONTROL:
            switch (ev->a) {
                case SMF_CC_VOLUME:     sy->volume[ch] = ev->b; break;
                case SMF_CC_EXPRESSION: sy->expression[ch] = ev->b; break;
                case SMF_CC_PAN:        sy->pan[ch] = ev->b; break;
                case SMF_CC_ALL_OFF:
                case SMF_CC_ALL_NOTES_OFF: {
                    int i;
                    for (i = 0; i < SYNTH_VOICES; i++)
                        if (sy->voice[i].channel == ch
                            && sy->voice[i].stage != ENV_OFF)
                            sy->voice[i].stage = ENV_RELEASE;
                    break;
                }
                default: break;
            }
            break;

        default:
            break;
    }
}

/* ms -> level increment per tick, for a stage that must cover the
 * full 0..65535 range in `ms` milliseconds. Never zero, or a stage
 * with a very long time never finishes. */
static int32_t rate_for(uint32_t ms, uint32_t tick_ms) {
    int32_t r;
    if (!ms) return 65535;
    r = (int32_t)((65535u * tick_ms) / ms);
    return r ? r : 1;
}

void synth_tick(synth_t *sy, uint32_t ms) {

    int i;

    if (!ms) return;

    for (i = 0; i < SYNTH_VOICES; i++) {

        synth_voice_t *v = &sy->voice[i];
        uint32_t g;
        int32_t lvl;

        if (v->stage == ENV_OFF) continue;

        if (v->channel == SMF_DRUM_CHANNEL) {
            /*
             * Percussion has no sustain: a drum decays to nothing
             * whether or not the key is still down. Modelling it as a
             * one-stage decay rather than forcing it through ADSR is
             * both simpler and correct -- a snare that sustains while
             * a note-off is pending is a snare that rattles.
             */
            const gm_drum_t *d = drum_for(v->note);
            if (v->stage == ENV_ATTACK) {
                v->level = 65535;
                v->stage = ENV_DECAY;
            }
            v->level -= rate_for(d->decay_ms, ms);
            if (v->level <= 0) { v->level = 0; v->stage = ENV_OFF; }
        } else {
            const gm_family_t *f =
                &gm_family[(sy->program[v->channel] >> 3) & 15];
            uint32_t sustain = ((uint32_t)f->sustain * 65535u) / 255u;

            switch (v->stage) {
                case ENV_ATTACK:
                    v->level += rate_for(f->attack_ms, ms);
                    if (v->level >= 65535) {
                        v->level = 65535;
                        v->stage = ENV_DECAY;
                    }
                    break;
                case ENV_DECAY:
                    v->level -= rate_for(f->decay_ms, ms);
                    if (v->level <= (int32_t)sustain) {
                        v->level = (int32_t)sustain;
                        v->stage = sustain ? ENV_SUSTAIN : ENV_OFF;
                    }
                    break;
                case ENV_SUSTAIN:
                    break;
                case ENV_RELEASE:
                    v->level -= rate_for(f->release_ms, ms);
                    if (v->level <= 0) { v->level = 0; v->stage = ENV_OFF; }
                    break;
                default:
                    break;
            }
        }

        if (v->stage == ENV_OFF) {
            v->active = false;
            v->gain_l = 0;
            v->gain_r = 0;
            continue;
        }

        /*
         * Gain: envelope x velocity x channel volume x expression x
         * master, then panned.
         *
         * Folded down in 16-bit steps rather than one long product,
         * because five 7-bit factors multiplied out is 35 bits and
         * this core has no 64-bit multiply worth calling.
         */
        lvl = v->level;
        g = (uint32_t)lvl >> 8;                        /* 0..255 */
        g = (g * v->velocity) >> 7;
        g = (g * sy->volume[v->channel]) >> 7;
        g = (g * sy->expression[v->channel]) >> 7;
        g = (g * sy->master) >> 6;
        if (g > 255) g = 255;

        {
            /*
             * Pan.
             *
             * The first version was `l = g * (127 - pan) >> 6` against
             * `r = g * pan >> 6`, and at the default centre pan of 64
             * that is 63/64 on the left and 64/64 on the right -- the
             * two sides are NOT the same, and they quantise at
             * different points all the way down the envelope: at a
             * gain of 4 the left gets 3 and the right gets 4, a 25%
             * difference. It also boosted a hard-panned voice to 1.98x
             * and clipped it.
             *
             * This law is exactly symmetric at centre by construction:
             * the side a note is panned towards stays at full scale
             * and the other side is attenuated, so centre is full on
             * both and nothing is ever boosted above unity.
             */
            uint32_t p = sy->pan[v->channel];
            uint32_t wl = (p <= 64u) ? 127u : ((127u - p) * 127u) / 63u;
            uint32_t wr = (p >= 64u) ? 127u : (p * 127u) / 64u;
            uint32_t l = (g * wl) / 127u;
            uint32_t r = (g * wr) / 127u;
            if (l > 255) l = 255;
            if (r > 255) r = 255;
            v->gain_l = (uint8_t)l;
            v->gain_r = (uint8_t)r;
        }
    }
}
