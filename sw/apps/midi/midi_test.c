/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host-side tests for smf.c and synth.c.
 *
 *   make test
 *
 * Built with the HOST compiler. Neither file includes anything from
 * sw/common or touches hardware, which is what makes this possible and
 * is the reason they are split out of midi.c at all -- the same
 * discipline as sw/apps/track/modplay.c and sw/apps/play/adec.c.
 *
 * Fixtures are BUILT HERE, byte by byte, rather than shipped. A real
 * MIDI file is somebody's composition, and a fixture whose correct
 * output nobody can state is not a test. Every file below is a few
 * events long and every expected result is arithmetic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "smf.h"
#include "synth.h"

static int tests = 0, fails = 0;

static void check(int cond, const char *name) {
    tests++;
    if (!cond) { fails++; printf("  FAIL  %s\n", name); }
    else printf("  ok    %s\n", name);
}

static void check_eq(long got, long want, const char *name) {
    tests++;
    if (got != want) {
        fails++;
        printf("  FAIL  %s (got %ld want %ld)\n", name, got, want);
    } else {
        printf("  ok    %s = %ld\n", name, got);
    }
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
 * fixture builder
 * ------------------------------------------------------------------ */

static uint8_t fx[4096];
static uint32_t fxlen;

static void put(uint8_t b) { fx[fxlen++] = b; }
static void put32(uint32_t v) {
    put(v >> 24); put(v >> 16); put(v >> 8); put(v);
}
static void put16(uint32_t v) { put(v >> 8); put(v); }

static void put_vlq(uint32_t v) {
    uint8_t b[4];
    int n = 0;
    do { b[n++] = v & 0x7f; v >>= 7; } while (v);
    while (n > 0) {
        n--;
        put((uint8_t)(b[n] | (n ? 0x80 : 0)));
    }
}

static void hdr(int format, int ntrk, int division) {
    fxlen = 0;
    memcpy(fx, "MThd", 4); fxlen = 4;
    put32(6);
    put16(format);
    put16(ntrk);
    put16(division);
}

static uint32_t trk_start;
static void trk_begin(void) {
    memcpy(fx + fxlen, "MTrk", 4); fxlen += 4;
    put32(0);                 /* patched by trk_end */
    trk_start = fxlen;
}
static void trk_end(void) {
    uint32_t len;
    put_vlq(0); put(0xFF); put(0x2F); put(0);   /* end of track */
    len = fxlen - trk_start;
    fx[trk_start - 4] = len >> 24;
    fx[trk_start - 3] = len >> 16;
    fx[trk_start - 2] = len >> 8;
    fx[trk_start - 1] = len;
}

/* ------------------------------------------------------------------
 * 1: header parsing and refusals
 * ------------------------------------------------------------------ */

static void t_header(void) {

    smf_t s;

    printf("\n[1] header\n");

    hdr(0, 1, 96); trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_OK, "format 0 opens");
    check_eq(s.division, 96, "division");
    check_eq(s.ntracks, 1, "one track");
    check_eq(s.tempo_us, 500000, "default tempo is 120bpm");

    hdr(1, 3, 480);
    trk_begin(); trk_end();
    trk_begin(); trk_end();
    trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_OK, "format 1 opens");
    check_eq(s.ntracks, 3, "three tracks");

    /* Format 2's tracks are independent sequences; "play the file" has
     * no single meaning, so it must be refused rather than guessed. */
    hdr(2, 1, 96); trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_ERR_FORMAT, "format 2 refused");

    /* SMPTE division needs a different clock entirely. Treating it as
     * PPQN would play at an arbitrary wrong tempo, which reads as a
     * sequencer bug rather than an unsupported mode. */
    hdr(0, 1, 0xE278); trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_ERR_DIVISION, "SMPTE refused");

    hdr(0, 1, 0); trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_ERR_DIVISION, "zero division refused");

    {
        uint8_t junk[32];
        memset(junk, 0xAB, sizeof(junk));
        check_eq(smf_open(&s, junk, sizeof(junk)), SMF_ERR_MAGIC,
            "garbage refused");
    }

    /* An unknown chunk between the header and the track must be
     * SKIPPED, not treated as an error -- the spec says so and real
     * files carry them. */
    hdr(0, 1, 96);
    memcpy(fx + fxlen, "XFIR", 4); fxlen += 4; put32(5);
    put(1); put(2); put(3); put(4); put(5);
    trk_begin(); trk_end();
    check_eq(smf_open(&s, fx, fxlen), SMF_OK, "unknown chunk skipped");
}

/* ------------------------------------------------------------------
 * 2: events
 * ------------------------------------------------------------------ */

static void t_events(void) {

    smf_t s;
    smf_event_t ev;

    printf("\n[2] events\n");

    hdr(0, 1, 96);
    trk_begin();
    put_vlq(0);   put(0xC0); put(48);            /* program change */
    put_vlq(0);   put(0x90); put(60); put(100);  /* note on */
    put_vlq(0);              put(64); put(100);  /* RUNNING STATUS */
    put_vlq(96);             put(60); put(0);    /* note on vel 0 */
    put_vlq(0);   put(0x80); put(64); put(64);   /* note off */
    trk_end();

    check_eq(smf_open(&s, fx, fxlen), SMF_OK, "opens");

    check_eq(smf_step(&s, 0, &ev), SMF_EV_PROGRAM, "program change");
    check_eq(ev.a, 48, "program number");

    check_eq(smf_step(&s, 0, &ev), SMF_EV_NOTE_ON, "note on");
    check_eq(ev.a, 60, "note 60");
    check_eq(ev.b, 100, "velocity");

    /* Running status: the second note-on omits its status byte. Every
     * real file does this and missing it turns the rest of a track
     * into garbage. */
    check_eq(smf_step(&s, 0, &ev), SMF_EV_NOTE_ON, "running status note on");
    check_eq(ev.a, 64, "running status note number");

    check_eq(smf_step(&s, 0, &ev), SMF_EV_NONE, "nothing else due yet");

    /* One quarter note at 96 PPQN and 120bpm is 500ms. */
    check_eq(smf_step(&s, 500000, &ev), SMF_EV_NOTE_OFF,
        "note-on velocity 0 IS a note off");
    check_eq(ev.a, 60, "which note");

    check_eq(smf_step(&s, 0, &ev), SMF_EV_NOTE_OFF, "explicit note off");
    check_eq(smf_step(&s, 0, &ev), SMF_EV_END, "end of file");
}

/* ------------------------------------------------------------------
 * 3: timing
 * ------------------------------------------------------------------ */

static void t_timing(void) {

    smf_t s;
    smf_event_t ev;
    int i, n;

    printf("\n[3] timing\n");

    /* Four quarter notes at 96 PPQN. */
    hdr(0, 1, 96);
    trk_begin();
    for (i = 0; i < 4; i++) {
        put_vlq(i ? 96 : 0); put(0x90); put(60 + i); put(100);
    }
    trk_end();
    smf_open(&s, fx, fxlen);

    /* Advance in 1ms steps and count how long until the fourth note.
     * Stepping in small increments is the point: it is where a
     * sequencer that truncates its fractional tick instead of
     * carrying it drifts, and truncation is invisible at coarse
     * granularity. */
    /* Notes fall at 0, 500, 1000 and 1500ms. Advance a millisecond at
     * a time and record when the fourth arrives. */
    n = 0;
    for (i = 0; i < 4000 && n < 4; i++) {
        int got = smf_step(&s, 1000, &ev);
        while (got != SMF_EV_NONE && got != SMF_EV_END) {
            if (got == SMF_EV_NOTE_ON) n++;
            got = smf_step(&s, 0, &ev);
        }
    }
    check_near((double)(i - 1), 1500.0, 20.0, "fourth note lands at 1500ms");

    /* Tempo change: 240bpm halves everything. */
    hdr(0, 1, 96);
    trk_begin();
    put_vlq(0); put(0xFF); put(0x51); put(3);
    put(250000 >> 16); put((250000 >> 8) & 0xff); put(250000 & 0xff);
    put_vlq(96); put(0x90); put(60); put(100);
    trk_end();
    smf_open(&s, fx, fxlen);

    n = 0;
    for (i = 0; i < 2000; i++) {
        int got = smf_step(&s, 1000, &ev);
        while (got != SMF_EV_NONE && got != SMF_EV_END) {
            if (got == SMF_EV_NOTE_ON) { n = 1; break; }
            got = smf_step(&s, 0, &ev);
        }
        if (n) break;
    }
    check_near((double)i, 250.0, 15.0, "tempo 240bpm -> note at 250ms");

    check_eq(s.tempo_us, 250000, "tempo was read");
}

/* ------------------------------------------------------------------
 * 4: format 1 track merging
 * ------------------------------------------------------------------ */

static void t_merge(void) {

    smf_t s;
    smf_event_t ev;
    int got, seen = 0;

    printf("\n[4] format 1 merging\n");

    /* Two tracks whose notes interleave in time. A sequencer that
     * played tracks one after another rather than merging them would
     * pass every single-track test and fail here -- which is most
     * files. */
    hdr(1, 2, 96);
    trk_begin();
    put_vlq(0);  put(0x90); put(60); put(100);
    put_vlq(192); put(0x90); put(62); put(100);
    trk_end();
    trk_begin();
    put_vlq(96); put(0x91); put(70); put(100);
    trk_end();

    check_eq(smf_open(&s, fx, fxlen), SMF_OK, "opens");
    check_eq(s.ntracks, 2, "two tracks");

    got = smf_step(&s, 0, &ev);
    check(got == SMF_EV_NOTE_ON && ev.a == 60, "t0: track 0 note 60");

    got = smf_step(&s, 500000, &ev);
    check(got == SMF_EV_NOTE_ON && ev.a == 70 && ev.channel == 1,
        "t=500ms: track 1 note 70 on channel 1");

    got = smf_step(&s, 500000, &ev);
    check(got == SMF_EV_NOTE_ON && ev.a == 62, "t=1000ms: track 0 note 62");
    (void)seen;
}

/* ------------------------------------------------------------------
 * 5: pitch
 * ------------------------------------------------------------------ */

static void t_pitch(void) {

    synth_t sy;
    smf_event_t ev;
    static uint8_t bank[SYNTH_BANK_BYTES];
    struct { int note; double hz; const char *name; } cases[] = {
        { 69, 440.00, "A4 = 440Hz" },
        { 60, 261.63, "C4 = 261.63Hz" },
        { 21,  27.50, "A0 = 27.5Hz" },
        { 108, 4186.01, "C8 = 4186Hz" },
        { 127, 12543.85, "G9 = 12543Hz" }
    };
    unsigned k;

    printf("\n[5] pitch\n");

    synth_build_bank(bank, 16);
    synth_init(&sy, 46875, 16);

    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {

        double hz;
        int i, v = -1;

        synth_all_off(&sy);
        memset(&ev, 0, sizeof(ev));
        ev.type = SMF_EV_NOTE_ON;
        ev.channel = 0;
        ev.a = (uint8_t)cases[k].note;
        ev.b = 100;
        synth_event(&sy, &ev);

        for (i = 0; i < SYNTH_VOICES; i++)
            if (sy.voice[i].stage != ENV_OFF) { v = i; break; }
        if (v < 0) { check(0, cases[k].name); continue; }

        /* step is bytes per output frame in 18.14 over a 256-byte
         * table, so the tone is step * out_hz / (256 * 2^14). */
        /* step counts BYTES, and a 16-bit table is two bytes per
         * sample -- so the tone is step * out_hz / (2 * 256 * 2^14). */
        hz = (double)sy.voice[v].step * 46875.0
            / (SYNTH_WAVE_LEN * 2.0 * 16384.0);
        check_near(hz, cases[k].hz, cases[k].hz * 0.004, cases[k].name);
    }

    /* An octave is exactly a doubling; anything else means the octave
     * shift and the table disagree. */
    {
        uint32_t a, b;
        int i;
        synth_all_off(&sy);
        memset(&ev, 0, sizeof(ev));
        ev.type = SMF_EV_NOTE_ON; ev.a = 60; ev.b = 100;
        synth_event(&sy, &ev);
        for (i = 0; i < SYNTH_VOICES; i++)
            if (sy.voice[i].stage != ENV_OFF) break;
        a = sy.voice[i].step;
        synth_all_off(&sy);
        ev.a = 72;
        synth_event(&sy, &ev);
        for (i = 0; i < SYNTH_VOICES; i++)
            if (sy.voice[i].stage != ENV_OFF) break;
        b = sy.voice[i].step;
        check_near((double)b / (double)a, 2.0, 0.005, "an octave doubles step");
    }
}

/* ------------------------------------------------------------------
 * 6: waveform bank
 * ------------------------------------------------------------------ */

static void t_bank(void) {

    static uint8_t bank[SYNTH_BANK_BYTES];
    int mip, w, i;
    long sum;

    printf("\n[6] waveform bank\n");

    synth_build_bank(bank, 16);

    /*
     * Every periodic table must average about zero at every mip.
     *
     * A DC offset on a looped waveform is a click at every loop point,
     * and here EIGHT voices sum into one accumulator and one DAC so
     * the offsets add into a permanent pull on the output. The first
     * version of this file built pulses as literal high/low levels and
     * had exactly that problem; deriving them from a saw difference
     * makes it impossible rather than merely fixed.
     */
    for (mip = 0; mip < SYNTH_MIPS; mip++) {
        for (w = 0; w < SYNTH_W_NOISE; w++) {
            const int16_t *p = (const int16_t *)
                (bank + synth_wave_offset(w, mip, 16));
            char name[64];
            sum = 0;
            for (i = 0; i < SYNTH_WAVE_LEN; i++) sum += p[i];
            snprintf(name, sizeof(name), "mip %d wave %d has no DC offset",
                mip, w);
            check(labs(sum) < 200L * SYNTH_WAVE_LEN, name);
        }
    }

    /* Sine: one positive and one negative half per period. */
    {
        const int16_t *p = (const int16_t *)
            (bank + synth_wave_offset(SYNTH_W_SINE, 0, 16));
        int zc = 0;
        for (i = 0; i < SYNTH_WAVE_LEN; i++) {
            int a = p[i], b = p[(i + 1) & (SYNTH_WAVE_LEN - 1)];
            if ((a >= 0) != (b >= 0)) zc++;
        }
        check_eq(zc, 2, "sine crosses zero twice per period");
    }

    /*
     * THE BAND-LIMITING CHECK, and the reason this group exists.
     *
     * A duller mip must have strictly less high-frequency content than
     * a brighter one. Measured as the mean absolute first difference:
     * a hard-edged saw jumps the full range once per period, while a
     * 4-harmonic one is nearly a sine. If this ever stops being true,
     * the tables are aliasing again and the output has broadband noise
     * in it that tracks the music.
     */
    {
        /*
         * Measured as the LARGEST single-sample jump, not the mean.
         *
         * The mean absolute difference was the first metric here and
         * it is nearly useless: the total variation of any normalised
         * single-peaked waveform is about twice its peak-to-peak
         * whatever its harmonic content, so a 32-harmonic saw and a
         * 4-harmonic one measured within 20% of each other and the
         * check would have passed on tables that were not
         * band-limited at all. The largest jump is what a hard edge
         * actually is.
         */
        long rough[SYNTH_MIPS];
        for (mip = 0; mip < SYNTH_MIPS; mip++) {
            const int16_t *p = (const int16_t *)
                (bank + synth_wave_offset(SYNTH_W_SAW, mip, 16));
            rough[mip] = 0;
            for (i = 0; i < SYNTH_WAVE_LEN; i++) {
                long d = labs((long)p[(i + 1) & 255] - (long)p[i]);
                if (d > rough[mip]) rough[mip] = d;
            }
        }
        check(rough[0] > rough[1] * 2, "saw mip 1 is far smoother than mip 0");
        check(rough[1] > rough[2] * 2, "saw mip 2 is far smoother than mip 1");
        printf("        largest jump %ld / %ld / %ld\n",
            rough[0], rough[1], rough[2]);
    }

    /* The three pulse duties must still be distinguishable after
     * band-limiting, or three of the seven waveforms are one sound. */
    {
        int hi[3], k;
        int waves[3] = { SYNTH_W_PULSE50, SYNTH_W_PULSE25, SYNTH_W_PULSE12 };
        for (k = 0; k < 3; k++) {
            const int16_t *p = (const int16_t *)
                (bank + synth_wave_offset(waves[k], 0, 16));
            hi[k] = 0;
            for (i = 0; i < SYNTH_WAVE_LEN; i++) if (p[i] > 0) hi[k]++;
        }
        check(hi[0] > hi[1] && hi[1] > hi[2], "pulse duties differ");
        printf("        duty %d%% / %d%% / %d%%\n",
            hi[0] * 100 / 256, hi[1] * 100 / 256, hi[2] * 100 / 256);
    }

    /* Amplitude must fill the sample width, or the band-limited tables
     * are quieter than the noise and the mix is unbalanced. */
    {
        const int16_t *p = (const int16_t *)
            (bank + synth_wave_offset(SYNTH_W_SAW, 0, 16));
        int peak = 0;
        for (i = 0; i < SYNTH_WAVE_LEN; i++) {
            int a = p[i] < 0 ? -p[i] : p[i];
            if (a > peak) peak = a;
        }
        check(peak > 28000, "band-limited saw fills the 16-bit range");
    }

    /*
     * Noise must not be periodic at any short period. A short looped
     * noise table does not sound like noise; it sounds like a buzzy
     * tone at the loop frequency, and a snare built from a buzzy tone
     * is not a snare.
     */
    {
        const int16_t *n = (const int16_t *)
            (bank + synth_wave_offset(SYNTH_W_NOISE, 0, 16));
        int period, worst = 0;
        for (period = 1; period <= 256; period++) {
            int match = 0, j;
            for (j = 0; j + period < 1024; j++)
                if (n[j] == n[j + period]) match++;
            if (match * 100 / j > worst) worst = match * 100 / j;
        }
        check(worst < 20, "noise has no short period");
    }

    /* An 8-bit bank must still build, for a bitstream whose mixer
     * predates CH_CTRL[18]. */
    {
        synth_build_bank(bank, 8);
        {
            const int8_t *p = (const int8_t *)
                (bank + synth_wave_offset(SYNTH_W_SAW, 0, 8));
            int peak = 0;
            for (i = 0; i < SYNTH_WAVE_LEN; i++) {
                int a = p[i] < 0 ? -p[i] : p[i];
                if (a > peak) peak = a;
            }
            check(peak > 110, "8-bit fallback bank fills its range");
        }
    }
}

/* ------------------------------------------------------------------
 * 7: voices and envelopes
 * ------------------------------------------------------------------ */

static void t_voices(void) {

    synth_t sy;
    smf_event_t ev;
    static uint8_t bank[SYNTH_BANK_BYTES];
    int i;

    printf("\n[7] voices\n");

    synth_build_bank(bank, 16);
    synth_init(&sy, 46875, 16);

    memset(&ev, 0, sizeof(ev));
    ev.type = SMF_EV_PROGRAM; ev.channel = 0; ev.a = 16;  /* organ */
    synth_event(&sy, &ev);

    /* Nine notes into eight voices. */
    for (i = 0; i < 9; i++) {
        ev.type = SMF_EV_NOTE_ON;
        ev.a = (uint8_t)(60 + i);
        ev.b = 100;
        synth_event(&sy, &ev);
        synth_tick(&sy, 1);
    }
    check_eq(synth_active(&sy), SYNTH_VOICES, "never more than 8 voices");
    check(sy.stolen >= 1, "the ninth note stole a voice");

    /* An organ sustains: after a long time the level must still be up. */
    synth_all_off(&sy);
    ev.type = SMF_EV_NOTE_ON; ev.a = 60; ev.b = 127;
    synth_event(&sy, &ev);
    for (i = 0; i < 500; i++) synth_tick(&sy, 5);
    check(sy.voice[0].stage == ENV_SUSTAIN, "organ reaches sustain");
    check(sy.voice[0].gain_l > 0, "and is still audible");

    /* Note off releases it to nothing. */
    ev.type = SMF_EV_NOTE_OFF; ev.a = 60;
    synth_event(&sy, &ev);
    check_eq(sy.voice[0].stage, ENV_RELEASE, "note off starts release");
    for (i = 0; i < 500; i++) synth_tick(&sy, 5);
    check_eq(sy.voice[0].stage, ENV_OFF, "release reaches silence");
    check_eq(sy.voice[0].gain_l, 0, "and zero gain");

    /* A piano does NOT sustain forever -- it decays to nothing with
     * the key still down. If every family sustained, every instrument
     * would be an organ, which is the single biggest thing separating
     * this from a broken ringtone. */
    synth_all_off(&sy);
    ev.type = SMF_EV_PROGRAM; ev.a = 0; synth_event(&sy, &ev);
    ev.type = SMF_EV_NOTE_ON; ev.a = 60; ev.b = 127;
    synth_event(&sy, &ev);
    for (i = 0; i < 200; i++) synth_tick(&sy, 5);
    check(sy.voice[0].stage == ENV_SUSTAIN || sy.voice[0].stage == ENV_OFF,
        "piano decays past attack");
    check(sy.voice[0].level < 40000, "piano is quieter than its peak");

    /* Percussion ignores note-off and decays on its own. */
    synth_all_off(&sy);
    ev.type = SMF_EV_NOTE_ON; ev.channel = SMF_DRUM_CHANNEL;
    ev.a = 38; ev.b = 127;
    synth_event(&sy, &ev);
    synth_tick(&sy, 1);
    check(sy.voice[0].stage != ENV_OFF, "drum sounds");
    for (i = 0; i < 200; i++) synth_tick(&sy, 5);
    check_eq(sy.voice[0].stage, ENV_OFF, "drum decays without a note off");

    /*
     * Pan must be EXACTLY symmetric at centre.
     *
     * The first law here gave 63/64 on the left against 64/64 on the
     * right. At full gain that is 1.6%, but the two sides quantise at
     * different points all the way down an envelope -- at a gain of 4
     * the left got 3 and the right got 4 -- so the channels decayed
     * differently and the right was consistently the louder and more
     * exact of the two. Checked at several levels, because it is only
     * obviously wrong at the quiet end.
     */
    {
        int levels[] = { 127, 64, 32, 8, 4 };
        unsigned k;
        synth_all_off(&sy);
        ev.type = SMF_EV_PROGRAM; ev.channel = 0; ev.a = 16;
        synth_event(&sy, &ev);
        for (k = 0; k < sizeof(levels) / sizeof(levels[0]); k++) {
            char name[64];
            synth_all_off(&sy);
            ev.type = SMF_EV_NOTE_ON; ev.a = 60;
            ev.b = (uint8_t)levels[k];
            synth_event(&sy, &ev);
            for (i = 0; i < 200; i++) synth_tick(&sy, 5);
            snprintf(name, sizeof(name), "centre pan symmetric at vel %d",
                levels[k]);
            check_eq(sy.voice[0].gain_l, sy.voice[0].gain_r, name);
        }
    }

    /* Hard pan silences the far side and must NOT boost the near one
     * above what centre would have given -- the old law multiplied by
     * 127/64 and clipped. */
    {
        int centre;
        synth_all_off(&sy);
        ev.type = SMF_EV_NOTE_ON; ev.a = 60; ev.b = 127;
        synth_event(&sy, &ev);
        for (i = 0; i < 200; i++) synth_tick(&sy, 5);
        centre = sy.voice[0].gain_r;

        ev.type = SMF_EV_CONTROL; ev.a = SMF_CC_PAN; ev.b = 127;
        synth_event(&sy, &ev);
        synth_tick(&sy, 5);
        check_eq(sy.voice[0].gain_l, 0, "hard right silences the left");
        check(sy.voice[0].gain_r <= centre, "hard pan does not boost");

        ev.type = SMF_EV_CONTROL; ev.a = SMF_CC_PAN; ev.b = 0;
        synth_event(&sy, &ev);
        synth_tick(&sy, 5);
        check_eq(sy.voice[0].gain_r, 0, "hard left silences the right");

        ev.type = SMF_EV_CONTROL; ev.a = SMF_CC_PAN; ev.b = 64;
        synth_event(&sy, &ev);
        synth_tick(&sy, 5);
    }

    /* Channel volume and master both attenuate. */
    synth_all_off(&sy);
    ev.type = SMF_EV_PROGRAM; ev.channel = 0; ev.a = 16;
    synth_event(&sy, &ev);
    ev.type = SMF_EV_NOTE_ON; ev.channel = 0; ev.a = 60; ev.b = 127;
    synth_event(&sy, &ev);
    for (i = 0; i < 200; i++) synth_tick(&sy, 5);
    {
        int full = sy.voice[0].gain_l;
        ev.type = SMF_EV_CONTROL; ev.a = SMF_CC_VOLUME; ev.b = 64;
        synth_event(&sy, &ev);
        synth_tick(&sy, 5);
        check(sy.voice[0].gain_l < full,
            "channel volume attenuates a sounding note");
        synth_set_master(&sy, 0);
        synth_tick(&sy, 5);
        check_eq(sy.voice[0].gain_l, 0, "master zero is silent");
    }
}

int main(void) {

    printf("midi engine tests\n");

    t_header();
    t_events();
    t_timing();
    t_merge();
    t_pitch();
    t_bank();
    t_voices();

    printf("\n%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
