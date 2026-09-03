/*
 * midi -- General MIDI file player
 *
 *   > run wm
 *   > run midi
 *   > run midi /MIDI/SONG.MID
 *
 * See docs/midi_app.md.
 *
 * -- why this app is easy and sw/apps/play was hard --
 *
 * A MIDI file is a SCORE, not audio. play had to put 46875 output
 * frames a second in front of a DAC, and this machine's measured IPC
 * of 0.08 left it about 29 instructions per frame to do it in -- which
 * is why it ended up handing playback to rtl/audio_mixer.v entirely.
 *
 * This app never touches an output frame at all. It reads a few tens
 * of KB off the card ONCE, then does nothing but hand note-on and
 * note-off events to eight mixer channels a few hundred times a
 * second. The synthesis is gateware. The CPU cost is somewhere around
 * one percent, and the interesting constraints are all musical rather
 * than computational.
 *
 * -- what it sounds like --
 *
 * Seven generated waveforms, sixteen General MIDI families, and eight
 * voices. Recognisable and musical; obviously not the instruments the
 * file was written for. sw/apps/midi/synth.h has the honest version of
 * this at length. The one limit no amount of software fixes is the
 * voice count: GM files routinely want twenty notes at once and there
 * are eight channels.
 *
 * -- controls --
 *
 *   space       play / pause
 *   s           stop (rewind)
 *   o           open a file
 *   n / p       next / previous file
 *   - / =       volume
 *   1..9, 0     mute/unmute MIDI channels 1..10
 *   d           mute/unmute the drum channel
 *   h           help
 *   q / ESC     quit
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"
#include "../../common/zfsapp.h"
#include "../../common/zkbd.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zwidget.h"
#include "../../common/zdialog.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"

#include "smf.h"
#include "synth.h"
#include "midi_icons.h"

/*
 * Largest .MID accepted, in bytes.
 *
 * Loaded WHOLE, with no streaming: a MIDI file is a score and scores
 * are small -- ten to a hundred KB is typical, and the format 1 track
 * merge needs random access to every track at once anyway. 256KB
 * covers everything short of a full orchestral export, and the file is
 * refused with a message rather than truncated, because half a MIDI
 * file plays perfectly right up until it stops.
 */
#ifndef MIDI_MAX_FILE
#define MIDI_MAX_FILE (256 * 1024)
#endif

#ifndef MIDI_VOLUME
#define MIDI_VOLUME 40          /* of 64 */
#endif

#ifndef MIDI_MAX_FILES
#define MIDI_MAX_FILES 64
#endif

#define MIDI_PATH_MAX 64

/* ------------------------------------------------------------------
 * state
 * ------------------------------------------------------------------ */

enum { ST_STOPPED = 0, ST_PLAYING, ST_PAUSED };

static uint8_t   filebuf[MIDI_MAX_FILE];

/*
 * The waveform bank the mixer fetches from.
 *
 * THE MIXER IS A BUS MASTER AND ISSUES PHYSICAL ADDRESSES. This
 * process sees its own memory through the MTU, so handing it
 * `wavebank` unchanged would point the sample fetches at whatever
 * physically lives at that offset -- which on this SOC is the BIOS and
 * the kernel. Same trap, same fix, as sw/apps/track's phys_of().
 *
 * 4-byte aligned because the mixer fetches whole words and selects a
 * byte from them.
 */
static uint8_t   wavebank[SYNTH_BANK_BYTES] __attribute__((aligned(4)));

/*
 * 8 or 16, decided at startup by what the mixer can fetch.
 *
 * 16-bit is not cosmetic here. The waveforms are BAND-LIMITED (see
 * synth.h), and a band-limited saw has a far lower peak-to-RMS ratio
 * than a hard ramp -- so quantising it to 8 bits throws away
 * proportionally much more of it than quantising a square would. The
 * two changes belong together: band-limiting is what removes the
 * aliasing noise, and 16 bits is what stops the cure being audible as
 * quantisation.
 */
static int       bank_bits = 8;

#define Z_APP_VIRT_BASE 0x80000000u
static uint32_t phys_of(const void *p) {
    return reg_mtu_base + ((uint32_t)p - Z_APP_VIRT_BASE);
}

static smf_t     sf;
static synth_t   sy;

static int       state = ST_STOPPED;
static bool      have_file;
static char      cur_path[MIDI_PATH_MAX];
static char      err_msg[64];
static uint32_t  out_hz;
static int       volume = MIDI_VOLUME;
static bool      want_quit;
static bool      mix_avail;

static char      files[MIDI_MAX_FILES][MIDI_PATH_MAX];
static int       nfiles;
static int       cur_file = -1;
static char      list_buf[4096];

static uint32_t  last_tick;
static uint32_t  env_accum;
static uint32_t  peak_voices;

static z_win_t   win;
static bool      windowed;

/* ------------------------------------------------------------------
 * layout
 * ------------------------------------------------------------------ */

#define WIN_W 336
#define WIN_H 186
#define ROW_H (z_font_5x8.h + 1)
#define ROWS  5

#define BTN_PLAY  0
#define BTN_STOP  1
#define WID_VOL   2
#define WIDGET_COUNT 3

static z_widget_t widgets[WIDGET_COUNT];
static z_widget_set_t wset;

static int meter_y, meter_h;

#define DIRTY_NAME (1u << 0)
#define DIRTY_FMT  (1u << 1)
#define DIRTY_TIME (1u << 2)
#define DIRTY_STAT (1u << 3)
#define DIRTY_HINT (1u << 4)
static uint32_t dirty;

/*
 * Force both mixer gains to the left value.
 *
 * A diagnostic, not a feature. If a fault is audible on one channel
 * only, this collapses the two sides to identical numbers -- so if it
 * persists with this on, nothing in the pan or gain arithmetic is
 * responsible and the difference is downstream, in the mixer or the
 * analogue output. One keystroke splits the search in half.
 */
static bool      force_mono;

static bool      show_help;
static int       help_row;

static const struct { const char *key; const char *what; } help_keys[] = {
    { "space",   "play / pause" },
    { "s",       "stop, rewind to the start" },
    { "o",       "open a file (or the titlebar icon)" },
    { "n / p",   "next / previous file" },
    { "- / =",   "volume down / up" },
    { "1..9 0",  "mute / unmute channels 1..10" },
    { "d",       "mute / unmute drums (channel 10)" },
    { "y",       "force mono (diagnostic: L and R identical)" },
    { "h",       "this help" },
    { "q / esc", "quit" }
};
#define HELP_ROWS ((int)(sizeof(help_keys) / sizeof(help_keys[0])))

/* ------------------------------------------------------------------
 * audio device
 * ------------------------------------------------------------------ */

static void audio_enable(bool on) {
    uint32_t c = reg_audio_ctrl & 0xFFu;
    if (on) c |= Z_AUDIO_CTRL_EN;
    else c &= ~Z_AUDIO_CTRL_EN;
    reg_audio_ctrl = c;
}

/*
 * Take the audio block.
 *
 * There is still no arbitration for it, so this stops sw/apps/track
 * and sw/apps/play dead. That is deliberate rather than careless: a
 * player that left MIXEN where it found it would program eight
 * channels nothing was listening to and produce silence with no
 * indication why. It also matters because the mixer is a bus master --
 * a channel left running by a killed process is still fetching from
 * memory the kernel has since handed to somebody else.
 */
static void audio_claim(void) {
    int c;
    mix_avail = z_audio_mixer_present();
    for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
    z_audio_mixer_enable(mix_avail);
}

/*
 * What was last written to each mixer channel, so an unchanged value
 * is not written again.
 *
 * This is not a micro-optimisation. The mixer's per-channel config
 * lives in DISTRIBUTED RAM (TRELLIS_DPR16X4), and its sequencer reads
 * ch_ctrl[seq] once per channel per frame -- 375,000 reads a second at
 * eight channels and 46875Hz. A write landing on the same cycle as a
 * read gives undefined read data.
 *
 * The first version of this app wrote all eight CH_CTRL words every
 * main-loop pass whether or not anything had changed: about 5,900
 * writes a second. sw/apps/track writes a couple of hundred and
 * sw/apps/play essentially none, which is why neither of them has ever
 * exercised this. Writing only on change takes it down to the rate the
 * envelopes actually move, which is a small fraction of that.
 */
static uint32_t last_ctrl[SYNTH_VOICES];
static uint32_t last_step[SYNTH_VOICES];

static void ch_ctrl_write(int c, uint32_t v) {
    if (last_ctrl[c] == v) return;
    last_ctrl[c] = v;
    Z_AUDIO_CH_CTRL(c) = v;
}

static void voices_silence(void) {
    int c;
    for (c = 0; c < SYNTH_VOICES; c++) {
        Z_AUDIO_CH_CTRL(c) = 0;
        last_ctrl[c] = 0;
        last_step[c] = 0;
    }
}

/*
 * Copy the synth's voice table into the mixer.
 *
 * The retrigger distinction is the important part. TRIG restarts a
 * channel from its offset field; EN alone changes gain without
 * touching the playback position. A gain update that also retriggered
 * would restart the waveform every envelope tick -- a few hundred
 * times a second -- which is not a note, it is a buzz at the tick
 * rate. zaudio.h calls this distinction out and this is the call site
 * that most needs it.
 */
static void voices_push(void) {

    int c;

    for (c = 0; c < SYNTH_VOICES; c++) {

        synth_voice_t *v = &sy.voice[c];

        if (v->stage == ENV_OFF) {
            ch_ctrl_write(c, 0);
            continue;
        }

        {
            uint8_t gl = v->gain_l;
            uint8_t gr = force_mono ? v->gain_l : v->gain_r;
            v->gain_r = gr;
            (void)gl;
        }

        if (v->retrigger) {
            Z_AUDIO_CH_BASE(c) = phys_of(wavebank) + v->base;
            Z_AUDIO_CH_LEN(c) = v->length;
            Z_AUDIO_CH_LOOPST(c) = v->loop_start;
            Z_AUDIO_CH_LOOPLEN(c) = v->loop_len;
            Z_AUDIO_CH_STEP(c) = v->step;
            /* A retrigger MUST be written even if the word is
             * identical to last time -- the same note played twice in
             * a row produces the same CH_CTRL, and skipping it would
             * silently drop the second note. */
            last_step[c] = v->step;
            last_ctrl[c] = z_audio_ch_ctrl_fmt(v->gain_l, v->gain_r,
                true, true, 0, bank_bits == 16);
            Z_AUDIO_CH_CTRL(c) = last_ctrl[c];
            v->retrigger = false;
        } else {
            /* Pitch bend moves STEP under a sounding note, so it has
             * to be able to change -- but only when it actually
             * does. */
            if (last_step[c] != v->step) {
                last_step[c] = v->step;
                Z_AUDIO_CH_STEP(c) = v->step;
            }
            ch_ctrl_write(c, z_audio_ch_ctrl_fmt(v->gain_l, v->gain_r,
                true, false, 0, bank_bits == 16));
        }
    }
}

/* ------------------------------------------------------------------
 * files
 * ------------------------------------------------------------------ */

static bool ext_is_mid(const char *name) {
    const char *dot = 0, *p;
    for (p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return false;
    dot++;
    if ((dot[0] == 'M' || dot[0] == 'm')
        && (dot[1] == 'I' || dot[1] == 'i')
        && (dot[2] == 'D' || dot[2] == 'd')) {
        /* .MID and .MIDI both, though FatFs is built with
         * FF_USE_LFN 0 so a four-letter extension cannot actually
         * appear on the card. Accepting it costs nothing and stops
         * this being a puzzle if that ever changes. */
        return dot[3] == 0 || ((dot[3] == 'I' || dot[3] == 'i')
            && dot[4] == 0);
    }
    return false;
}

static void scan_dir(const char *dir) {

    uint32_t count = 0, trunc = 0, i;
    const char *p;

    if (!fs_list_into(dir, list_buf, sizeof(list_buf), 0,
        MIDI_MAX_FILES * 2, &count, &trunc))
        return;

    p = list_buf;
    for (i = 0; i < count && nfiles < MIDI_MAX_FILES; i++) {
        size_t l = strlen(p);
        if (ext_is_mid(p) && l < MIDI_PATH_MAX) {
            strcpy(files[nfiles], p);
            nfiles++;
        }
        p += l + 1;
    }
}

static void stop_all(void) {
    synth_all_off(&sy);
    voices_silence();
}

static bool open_file(const char *path) {

    int fh, got, rc;
    uint32_t size;

    stop_all();
    have_file = false;
    err_msg[0] = 0;
    state = ST_STOPPED;

    size = (uint32_t)fs_size((char *)path);
    if (!size) { snprintf(err_msg, sizeof(err_msg), "not found or empty");
        return false; }
    if (size > MIDI_MAX_FILE) {
        /* Named rather than truncated: half a MIDI file plays
         * perfectly right up until it stops, which is a much harder
         * thing to diagnose than a refusal. */
        snprintf(err_msg, sizeof(err_msg), "too large (%luKB, max %luKB)",
            (unsigned long)(size / 1024),
            (unsigned long)(MIDI_MAX_FILE / 1024));
        return false;
    }

    fh = fs_open_read(path);
    if (fh < 0) { snprintf(err_msg, sizeof(err_msg), "cannot open");
        return false; }

    /* Read in chunks: fs_read_chunk() is bounded and a 256KB single
     * call would sit inside the filesystem for a long time with
     * interrupts free to preempt it -- see sw/os/fsapi.h on
     * re-entrancy. */
    {
        uint32_t off = 0;
        while (off < size) {
            uint32_t want = size - off;
            if (want > 4096) want = 4096;
            got = fs_read_chunk(fh, filebuf + off, (int)want);
            if (got <= 0) break;
            off += (uint32_t)got;
        }
        fs_close_handle(fh);
        if (off < size) {
            snprintf(err_msg, sizeof(err_msg), "read failed at %lu",
                (unsigned long)off);
            return false;
        }
    }

    rc = smf_open(&sf, filebuf, size);
    if (rc != SMF_OK) {
        snprintf(err_msg, sizeof(err_msg), "%s", smf_strerror(rc));
        return false;
    }

    smf_scan_length(&sf);
    smf_rewind(&sf);

    synth_reset(&sy);
    synth_set_master(&sy, (uint8_t)volume);

    strncpy(cur_path, path, sizeof(cur_path) - 1);
    cur_path[sizeof(cur_path) - 1] = 0;
    have_file = true;
    peak_voices = 0;

    last_tick = z_uptime_ticks();
    state = ST_PLAYING;
    dirty = DIRTY_NAME | DIRTY_FMT | DIRTY_TIME | DIRTY_STAT | DIRTY_HINT;

    printf("midi: %s  format %d, %d track%s, %lu ppqn, ~%lus\n",
        path, sf.format, sf.ntracks, sf.ntracks == 1 ? "" : "s",
        (unsigned long)sf.division,
        (unsigned long)(smf_duration_ms(&sf) / 1000));
    return true;
}

static void next_file(int delta) {
    int i;
    if (!nfiles) return;
    i = cur_file + delta;
    if (i >= nfiles) i = 0;
    if (i < 0) i = nfiles - 1;
    cur_file = i;
    if (!open_file(files[cur_file])) {
        state = ST_STOPPED;
        printf("midi: %s: %s\n", files[cur_file], err_msg);
    }
}

/* ------------------------------------------------------------------
 * the sequencer loop
 * ------------------------------------------------------------------ */

/*
 * Microseconds per kernel tick.
 *
 * The KTIMER runs at ~732Hz, so 1366us. That is the resolution the
 * sequencer gets, and it is ample: a sixteenth note at 120bpm is
 * 125ms, so a tick is about one percent of the shortest thing in an
 * ordinary score. smf.c carries the fractional remainder between
 * calls, so the granularity costs jitter rather than drift.
 */
#define TICK_US 1366u

/* True if any voice was just started and is waiting to be programmed. */
static bool any_retrigger(void) {
    int c;
    for (c = 0; c < SYNTH_VOICES; c++)
        if (sy.voice[c].retrigger) return true;
    return false;
}

static void sequence(void) {

    uint32_t now, dt;
    smf_event_t ev;
    int got, n;

    if (state != ST_PLAYING || !have_file) return;

    now = z_uptime_ticks();
    dt = now - last_tick;
    if (!dt) return;
    last_tick = now;

    /*
     * Bounded, because the elapsed time is wall clock and this process
     * can be away for a long time -- a file dialog, a redraw storm,
     * another app misbehaving. Without the clamp, coming back after a
     * second would advance the score a second in one step and fire
     * every event in it at once, which is a very loud noise.
     *
     * The music simply pauses for the missing interval instead. That
     * is the right trade for a player: a gap is a glitch, a chord of
     * forty simultaneous notes is a fright.
     */
    if (dt > 64) dt = 64;

    got = smf_step(&sf, dt * TICK_US, &ev);
    while (got != SMF_EV_NONE && got != SMF_EV_END) {
        synth_event(&sy, &ev);
        got = smf_step(&sf, 0, &ev);
    }

    /*
     * Envelopes advance on their own slower clock.
     *
     * At the loop rate this ran at ~732Hz, and every step changed
     * eight gains and wrote eight CH_CTRL words. An envelope does not
     * need that: the fastest thing in the GM table is a 1ms attack,
     * and 5ms steps are inaudible on a decay. Accumulating the
     * remainder means the envelope timing is unchanged -- only the
     * write rate falls, by about a factor of four.
     */
    env_accum += dt * TICK_US;
    if (env_accum >= 5000u) {
        synth_tick(&sy, env_accum / 1000u);
        env_accum = 0;
        voices_push();
    } else if (any_retrigger()) {
        /* A note-on must reach the mixer immediately -- delaying it by
         * up to 5ms is an audible late note, and unlike a gain step
         * that is not something the ear forgives. */
        voices_push();
    }

    n = synth_active(&sy);
    if ((uint32_t)n > peak_voices) peak_voices = (uint32_t)n;

    if (got == SMF_EV_END) {
        if (nfiles) next_file(1);
        else { state = ST_STOPPED; stop_all(); }
        dirty |= DIRTY_TIME | DIRTY_STAT;
    }
}

/* ------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------ */

static void draw_row(int row, const char *s) {
    int y = 2 + row * ROW_H;
    z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
    z_win_draw_text(&win, 2, y, s, 1, &z_font_5x8);
}

static void draw_name(void) {
    char b[MIDI_PATH_MAX + 16];
    if (!have_file)
        snprintf(b, sizeof(b), "%s",
            err_msg[0] ? err_msg : "no file (o to open)");
    else if (nfiles)
        snprintf(b, sizeof(b), "%d/%d %s", cur_file + 1, nfiles, cur_path);
    else
        snprintf(b, sizeof(b), "%s", cur_path);
    draw_row(0, b);
}

static void draw_fmt(void) {
    char b[72];
    if (!have_file) { draw_row(1, ""); return; }
    snprintf(b, sizeof(b), "SMF %d  %d trk  %lu ppqn  %lu bpm  %d-bit",
        sf.format, sf.ntracks, (unsigned long)sf.division,
        (unsigned long)(sf.tempo_us ? 60000000u / sf.tempo_us : 0),
        bank_bits);
    draw_row(1, b);
}

static void draw_time(void) {
    char b[72];
    uint32_t el = have_file ? smf_elapsed_ms(&sf) / 1000u : 0;
    uint32_t to = have_file ? smf_duration_ms(&sf) / 1000u : 0;
    snprintf(b, sizeof(b), "%lu:%02lu / %lu:%02lu  vol %d%%  %s",
        (unsigned long)(el / 60), (unsigned long)(el % 60),
        (unsigned long)(to / 60), (unsigned long)(to % 60),
        (volume * 100) / 64,
        state == ST_PLAYING ? "PLAYING"
            : (state == ST_PAUSED ? "PAUSED" : "STOPPED"));
    draw_row(2, b);
}

static void draw_stat(void) {
    char b[72];
    /*
     * `voices` and `stolen` together are the honest measure of whether
     * this file fits in eight channels. A steadily climbing steal
     * count on a busy passage is not a fault -- it is the file asking
     * for more polyphony than the hardware has, and it is the single
     * most useful thing this window can say about why something sounds
     * thin.
     */
    snprintf(b, sizeof(b), "voices %d/%d  peak %lu  stolen %lu",
        synth_active(&sy), SYNTH_VOICES,
        (unsigned long)peak_voices, (unsigned long)sy.stolen);
    draw_row(3, b);
}

static void draw_hint(void) {
    char b[72];
    int i, n = 0;
    for (i = 0; i < SMF_CHANNELS; i++) if (sy.muted[i]) n++;
    if (force_mono)
        snprintf(b, sizeof(b), "MONO (diagnostic)     h = help");
    else if (n)
        snprintf(b, sizeof(b), "%d channel%s muted     h = help",
            n, n == 1 ? "" : "s");
    else
        snprintf(b, sizeof(b), "h = help");
    draw_row(4, b);
}

static void draw_help_row(int i) {
    char b[72];
    int y = 2 + i * ROW_H;
    if (i >= HELP_ROWS) return;
    snprintf(b, sizeof(b), "%-8s %s", help_keys[i].key, help_keys[i].what);
    z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
    z_win_draw_text(&win, 2, y, b, 1, &z_font_5x8);
}

/*
 * Sixteen channel activity bars.
 *
 * Every fill here goes through the hardware blitter -- z_win_fill_rect()
 * does, and the Makefile passes -DZ_GFX_HW_BLIT so the text does too.
 * That is not optional at this size: the software path in
 * z_fb_fill_rect() is a per-pixel read-modify-write of VRAM and its
 * cost is proportional to AREA, which sw/common/zwin.c records as
 * roughly three seconds to clear a 288x216 dialog.
 *
 * -- but the blitter is not free, so only changed bars are drawn --
 *
 * The first version repainted all sixteen unconditionally at 10Hz:
 * thirty-two blitter fills a refresh, each re-deriving the window's
 * clip rectangle, for a display where most channels are silent and a
 * typical file uses two or three. Tracking the last height drawn takes
 * that to a handful.
 *
 * Better still, a changed bar is ONE fill rather than two. Growing
 * paints only the new part; shrinking erases only the part that left.
 * Erasing the full column and redrawing the bar -- the obvious way --
 * is twice the blitter traffic and it FLICKERS, because there is a
 * window between the two fills where the bar is not there at all.
 */
static int8_t meter_last[SMF_CHANNELS];

static void meters_invalidate(void) {
    int i;
    /* -1 means "nothing known on screen", which forces the next pass
     * to paint the whole column. Used after a full redraw, where the
     * window was cleared underneath us. */
    for (i = 0; i < SMF_CHANNELS; i++) meter_last[i] = -1;
}

static void draw_meters(void) {

    int w = z_win_content_w(&win);
    int bw = (w - 4) / SMF_CHANNELS;
    int i, c;
    uint8_t lvl[SMF_CHANNELS];

    if (meter_h < 4 || bw < 3) return;

    memset(lvl, 0, sizeof(lvl));
    for (c = 0; c < SYNTH_VOICES; c++) {
        const synth_voice_t *v = &sy.voice[c];
        uint8_t g;
        if (v->stage == ENV_OFF) continue;
        g = (v->gain_l > v->gain_r) ? v->gain_l : v->gain_r;
        if (g > lvl[v->channel & 15]) lvl[v->channel & 15] = g;
    }

    for (i = 0; i < SMF_CHANNELS; i++) {

        int x = 2 + i * bw;
        int h = (lvl[i] * (meter_h - 2)) / 255;
        int last = meter_last[i];

        if (sy.muted[i]) {
            /* A muted channel shows a floor line rather than nothing,
             * so "silent because muted" and "silent because nothing is
             * playing" are distinguishable at a glance. */
            if (last != 0) {
                z_win_fill_rect(&win, x, meter_y, bw - 1, meter_h, 0);
                z_win_fill_rect(&win, x, meter_y + meter_h - 1, bw - 1, 1, 1);
                meter_last[i] = 0;
            }
            continue;
        }

        if (h == last) continue;

        if (last < 0) {
            /* Nothing known on screen: clear the column, then paint. */
            z_win_fill_rect(&win, x, meter_y, bw - 1, meter_h, 0);
            if (h > 0)
                z_win_fill_rect(&win, x, meter_y + meter_h - h, bw - 1, h, 1);
        } else if (h > last) {
            z_win_fill_rect(&win, x, meter_y + meter_h - h, bw - 1,
                h - last, 1);
        } else {
            z_win_fill_rect(&win, x, meter_y + meter_h - last, bw - 1,
                last - h, 0);
        }

        meter_last[i] = (int8_t)h;
    }
}

static void ui_step(void) {
    if (show_help) {
        if (help_row < HELP_ROWS) draw_help_row(help_row++);
        return;
    }
    if (dirty & DIRTY_NAME) { dirty &= ~DIRTY_NAME; draw_name(); }
    else if (dirty & DIRTY_FMT) { dirty &= ~DIRTY_FMT; draw_fmt(); }
    else if (dirty & DIRTY_TIME) { dirty &= ~DIRTY_TIME; draw_time(); }
    else if (dirty & DIRTY_STAT) { dirty &= ~DIRTY_STAT; draw_stat(); }
    else if (dirty & DIRTY_HINT) { dirty &= ~DIRTY_HINT; draw_hint(); }
}

static void layout(void) {
    int h = z_win_content_h(&win);
    int w = z_win_content_w(&win);
    meter_y = 2 + ROWS * ROW_H + 2;
    meter_h = h - meter_y - 22;
    if (meter_h < 0) meter_h = 0;
    widgets[BTN_PLAY].x = 4;
    widgets[BTN_PLAY].y = (int16_t)(h - 20);
    widgets[BTN_STOP].x = 26;
    widgets[BTN_STOP].y = (int16_t)(h - 20);
    widgets[WID_VOL].x = 54;
    widgets[WID_VOL].y = (int16_t)(h - 16);
    widgets[WID_VOL].w = (int16_t)(w - 60);
}

static void draw_all(void) {
    z_win_clear(&win);
    layout();
    if (show_help) {
        help_row = 0;
        z_widget_invalidate(&wset);
        z_widget_draw_all(&wset, true);
        return;
    }
    dirty = DIRTY_NAME | DIRTY_FMT | DIRTY_TIME | DIRTY_STAT | DIRTY_HINT;
    z_widget_invalidate(&wset);
    z_widget_draw_all(&wset, true);
    meters_invalidate();
    draw_meters();
}

/* ------------------------------------------------------------------
 * transport
 * ------------------------------------------------------------------ */

static void set_play_icon(void) {
    widgets[BTN_PLAY].icon = (state == ST_PLAYING)
        ? midi_icon_pause : midi_icon_play;
    widgets[BTN_PLAY].dirty = true;
    if (windowed && win.id >= 0) z_widget_draw_all(&wset, false);
}

static void do_playpause(void) {
    if (!have_file) {
        if (nfiles) next_file(cur_file < 0 ? 1 : 0);
        set_play_icon();
        return;
    }
    if (state == ST_PLAYING) {
        state = ST_PAUSED;
        stop_all();
        audio_enable(false);
    } else {
        state = ST_PLAYING;
        audio_enable(true);
        /* Reset the clock, or the pause duration counts as elapsed
         * score time and the file jumps forward on resume. */
        last_tick = z_uptime_ticks();
    }
    set_play_icon();
    dirty |= DIRTY_TIME;
}

static void do_stop(void) {
    if (!have_file) return;
    state = ST_STOPPED;
    stop_all();
    smf_rewind(&sf);
    synth_reset(&sy);
    synth_set_master(&sy, (uint8_t)volume);
    peak_voices = 0;
    set_play_icon();
    dirty |= DIRTY_TIME | DIRTY_STAT;
}

static void set_volume(int v) {
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    volume = v;
    synth_set_master(&sy, (uint8_t)volume);
    widgets[WID_VOL].value = (int16_t)volume;
    widgets[WID_VOL].dirty = true;
    if (windowed && win.id >= 0) z_widget_draw_all(&wset, false);
    dirty |= DIRTY_TIME;
}

static void toggle_mute(int ch) {
    if (ch < 0 || ch >= SMF_CHANNELS) return;
    sy.muted[ch] = !sy.muted[ch];
    meter_last[ch] = -1;        /* force the bar to repaint */
    if (sy.muted[ch]) {
        int i;
        for (i = 0; i < SYNTH_VOICES; i++)
            if (sy.voice[i].channel == ch && sy.voice[i].stage != ENV_OFF)
                sy.voice[i].stage = ENV_RELEASE;
    }
    dirty |= DIRTY_HINT;
}

/* ------------------------------------------------------------------
 * input
 * ------------------------------------------------------------------ */

static void on_dialog_msg(z_msg_t *msg, void *user);

/*
 * Open through the standard dialog.
 *
 * Playback stops while it is open. z_dialog_open() runs its own
 * message loop with no per-iteration hook (sw/common/zdialog.c), so
 * the sequencer cannot be advanced -- and a sequencer that is not
 * advanced leaves every sounding note held. Stopping is the honest
 * behaviour; an on_idle hook in zdialog is the real fix and is noted
 * in docs/play_app.md as well, since that app wants it for the same
 * reason.
 */
static void do_open(void) {

    z_dialog_ctx_t ctx;
    char path[MIDI_PATH_MAX];
    int prev = state;

    if (!windowed) return;

    if (state == ST_PLAYING) { state = ST_PAUSED; stop_all(); }

    memset(&ctx, 0, sizeof(ctx));
    ctx.parent = &win;
    ctx.on_msg = on_dialog_msg;

    if (z_dialog_open(&ctx, "/MIDI", path, sizeof(path))) {
        int i;
        cur_file = -1;
        for (i = 0; i < nfiles; i++)
            if (!strcmp(files[i], path)) { cur_file = i; break; }
        if (!open_file(path)) {
            state = ST_STOPPED;
            printf("midi: %s: %s\n", path, err_msg);
        }
    } else if (prev == ST_PLAYING) {
        state = ST_PLAYING;
        last_tick = z_uptime_ticks();
    }

    set_play_icon();
    draw_all();
}

static void handle_key(uint32_t ks) {

    if (show_help) { show_help = false; draw_all(); return; }

    if (ks >= '1' && ks <= '9') { toggle_mute((int)ks - '1'); return; }
    if (ks == '0') { toggle_mute(9); return; }

    switch (ks) {
        case 'h':
        case '?':
            show_help = true;
            help_row = 0;
            z_win_clear(&win);
            z_widget_invalidate(&wset);
            break;
        case ' ': do_playpause(); break;
        case 's': do_stop(); break;
        case 'o': do_open(); break;
        case 'n': next_file(1); set_play_icon(); break;
        case 'p': next_file(-1); set_play_icon(); break;
        case '-': set_volume(volume - 4); break;
        case '=':
        case '+': set_volume(volume + 4); break;
        case 'd': toggle_mute(SMF_DRUM_CHANNEL); break;
        case 'y':
            force_mono = !force_mono;
            dirty |= DIRTY_HINT;
            break;
        case 'q':
        case 27:  want_quit = true; break;
        default: break;
    }
}

static void handle_mouse(uint32_t packed) {
    int cx, cy, act;
    uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
    z_win_mouse_content_xy(&win, packed, &cx, &cy);
    act = z_widget_mouse(&wset, cx, cy, buttons);
    if (act == BTN_PLAY) do_playpause();
    else if (act == BTN_STOP) do_stop();
    else if (act == WID_VOL) set_volume(widgets[WID_VOL].value);
}

static void dispatch(z_msg_t *msg, bool *redraw) {
    switch (msg->subject) {
        case Z_WM_SET_CLIP:
            z_win_apply_clip(&win, &msg->obj);
            break;
        case Z_WM_REDRAW:
            z_win_apply_redraw(&win, msg->obj.val.uint32);
            if (redraw) *redraw = true;
            break;
        case Z_WM_WINDOW_MOVED:
            z_win_parse_rect(&win, &msg->obj);
            break;
        case Z_WM_MOUSE:
            if (msg->obj.type == Z_UINT32)
                handle_mouse(msg->obj.val.uint32);
            break;
        case Z_WM_KEY:
            /* Z_WM_KEY is packed and carries the press/release edge.
             * Acting on both makes every toggle fire twice -- space
             * would pause and instantly unpause. */
            if (Z_WM_UNPACK_KEY_PRESSED(msg->obj.val.uint32))
                handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg->obj.val.uint32));
            break;
        case Z_WM_TITLEBAR_ICON: {
            uint32_t v;
            if (msg->obj.type != Z_UINT32) break;
            v = msg->obj.val.uint32;
            if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;
            if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_OPEN) do_open();
            break;
        }
        default: break;
    }
}

static void on_dialog_msg(z_msg_t *msg, void *user) {
    bool redraw = false;
    (void)user;
    dispatch(msg, &redraw);
    if (redraw) { draw_all(); z_win_redraw_done(&win); }
}

static void drain_messages(void) {
    z_msg_t msg;
    bool redraw = false;
    while (z_msg_read(&msg) == Z_OK) {
        if (msg.subject == Z_WM_CLOSE) { want_quit = true; continue; }
        dispatch(&msg, &redraw);
        if (want_quit) return;
    }
    if (redraw) { draw_all(); z_win_redraw_done(&win); }
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

static void init_widgets(void) {
    memset(widgets, 0, sizeof(widgets));
    widgets[BTN_PLAY].type = Z_WIDGET_BUTTON;
    widgets[BTN_PLAY].w = 20;
    widgets[BTN_PLAY].h = 18;
    widgets[BTN_PLAY].enabled = true;
    widgets[BTN_PLAY].icon = midi_icon_play;
    widgets[BTN_STOP].type = Z_WIDGET_BUTTON;
    widgets[BTN_STOP].w = 20;
    widgets[BTN_STOP].h = 18;
    widgets[BTN_STOP].enabled = true;
    widgets[BTN_STOP].icon = midi_icon_stop;
    widgets[WID_VOL].type = Z_WIDGET_SLIDER;
    widgets[WID_VOL].h = 12;
    widgets[WID_VOL].enabled = true;
    widgets[WID_VOL].vmin = 0;
    widgets[WID_VOL].vmax = 64;
    widgets[WID_VOL].value = (int16_t)volume;
    z_widget_set_init(&wset, widgets, WIDGET_COUNT, &win);
}

int main(void) {

    char arg[MIDI_PATH_MAX];
    uint32_t rate_div, ui_tick;

    printf("\nmidi -- General MIDI file player\n");

    /* Feature bit first, then MAGIC: on a bitstream with no audio
     * block, 0x7000_05xx is decoded by nothing, and an undecoded
     * address on this bus never acks -- probing it hangs the CPU.
     * z_audio_present() does it in the safe order. */
    if (!z_audio_present()) {
        printf("This bitstream has no audio block.\n");
        return 1;
    }

    rate_div = reg_audio_rate & 0xFF;
    if (!z_audio_rate_ok(rate_div)) rate_div = Z_AUDIO_RATE_44K;
    z_audio_start(rate_div);
    audio_claim();
    out_hz = z_audio_rate_hz();

    if (!mix_avail) {
        /* Everything here is the mixer. Without it there is no
         * fallback worth having -- a software synthesiser would have
         * to produce output frames, which is precisely what this
         * machine cannot do (see docs/play_app.md). */
        printf("This bitstream has no hardware mixer; midi needs one.\n");
        printf("Rebuild with `AUDIO_MIXER in rtl/boards.vh.\n");
        return 1;
    }

    printf("midi: output %luHz, 8 voices, %d-bit waveforms%s\n",
        (unsigned long)out_hz, bank_bits,
        bank_bits == 8 ? " (mixer has no FMT16)" : "");

    bank_bits = z_audio_mixer_fmt16() ? 16 : 8;
    synth_build_bank(wavebank, bank_bits);
    synth_init(&sy, out_hz, bank_bits);
    synth_set_master(&sy, (uint8_t)volume);

    arg[0] = 0;
    z_launch_arg_take(arg, sizeof(arg));

    scan_dir("/MIDI");
    if (!nfiles) scan_dir("/midi");
    if (!nfiles) scan_dir("/");
    printf("midi: %d file%s\n", nfiles, nfiles == 1 ? "" : "s");

    init_widgets();

    /* No CLOSE_KILLS_OWNER: this app leaves eight mixer channels
     * running as bus masters, and a process removed from under itself
     * never gets to stop them. */
    windowed = (z_win_create_flags(&win, "midi", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_OPEN_ICON) == Z_OK);
    if (!windowed) printf("midi: no wm; console mode\n");
    printf("midi: h for help\n");

    if (arg[0]) {
        if (!open_file(arg)) printf("midi: %s: %s\n", arg, err_msg);
    } else if (nfiles) {
        cur_file = 0;
        if (!open_file(files[0])) printf("midi: %s\n", err_msg);
    }
    set_play_icon();
    if (windowed) draw_all();

    ui_tick = z_uptime_ticks();

    while (!want_quit) {

        sequence();

        if (windowed) {
            drain_messages();
            ui_step();
        } else {
            int32_t ev = hid_read_key();
            if (ev >= 0)
                handle_key(z_kbd_usage_to_keysym((uint8_t)(ev & 0xFF),
                    (uint8_t)((ev >> 8) & 0xFF)));
        }

        {
            uint32_t now = z_uptime_ticks();
            if (now - ui_tick >= 73) {          /* ~10Hz */
                ui_tick = now;
                dirty |= DIRTY_TIME | DIRTY_STAT;
                if (windowed && !show_help) draw_meters();
            }
        }

        /*
         * Always yield.
         *
         * Unlike sw/apps/play there is nothing to be gained by
         * spinning: the mixer holds the note until told otherwise, so
         * being late by a tick delays an event rather than dropping
         * audio. One tick is 1366us against a shortest-note of about
         * 125ms, and sequence() carries the fractional remainder, so
         * this costs jitter and not drift.
         */
        z_proc_wait(1);
    }

    stop_all();
    z_audio_mixer_enable(false);
    z_audio_stop();
    if (windowed) z_win_destroy(&win);
    printf("\nmidi: done.\n");
    return 0;
}
