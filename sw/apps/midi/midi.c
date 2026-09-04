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

/*
 * The keyboard plays on MIDI channel 16 (index 15).
 *
 * Not channel 1, which is what most files use for the melody -- the
 * point is to play ALONG with a file, and sharing a channel would mean
 * sharing its program, its volume and its pan, so the file would
 * reprogram the instrument under the player's fingers.
 */
#define KBD_CHANNEL 15
#define KBD_SEMIS 29        /* C .. E two octaves up: 17 white, 12 black */

static uint8_t   kbd_base = 48;     /* semitone 0 = C3 */
static uint8_t   kbd_program = 80;  /* synth lead: cuts through a mix */
static bool      kbd_down[KBD_SEMIS];
static int8_t    kbd_drawn[KBD_SEMIS];  /* what is on screen: -1 unknown */
static int       kbd_x, kbd_y, kbd_h, kbd_kw;

/* Which key the mouse is currently holding down, or -1. Separate from
 * kbd_down[] because a key can be held by the computer keyboard and
 * the mouse at once, and releasing one must not cut the other. */
static int       kbd_mouse_semi = -1;

static void kbd_layout(void);
static void kbd_draw_all(void);
static void kbd_refresh(void);
static void kbd_all_off(void);
static void kbd_note(int semi, bool on);
static void kbd_set_program(int p);
static int  kbd_semi_of(uint32_t ks);
static int  kbd_hit(int cx, int cy);
static void kbd_note(int semi, bool on);

static z_win_t   win;
static bool      windowed;

/* ------------------------------------------------------------------
 * layout
 * ------------------------------------------------------------------ */

/*
 * Sized to the layout, not the other way round.
 *
 * This was 344x236 and nearly half of it was empty: the keyboard was
 * pinned to the bottom of the content area while the meters were
 * capped near the top, so every pixel of window height beyond what the
 * two needed became a gap between them. On a 512x384 screen that is a
 * quarter of the display spent on nothing.
 *
 * The layout now packs from the top and these numbers are what it
 * actually needs -- see layout(), which computes the same total and
 * degrades if wm gives it less.
 */
#define WIN_W 304
#define WIN_H 134
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
    { "ZXCV..",  "play notes -- lower octave, SDGHJ are its sharps" },
    { "QWER..",  "play notes -- octave above, 23567 are its sharps" },
    { "up/down", "shift the keyboard an octave" },
    { "[ / ]",   "keyboard instrument" },
    { "space",   "start / pause the file playing" },
    { "enter",   "stop, rewind to the start" },
    { "tab",     "open a file (or the titlebar icon)" },
    { "left/rt", "previous / next file" },
    { "- / =",   "volume down / up" },
    { "F1",      "this help" },
    { "F2",      "mute / unmute drums" },
    { "F3",      "force mono (diagnostic: L and R identical)" },
    { "F4",      "panic -- all notes off" },
    { "esc",     "quit" }
};
#define HELP_ROWS ((int)(sizeof(help_keys) / sizeof(help_keys[0])))

/* ------------------------------------------------------------------
 * audio device
 * ------------------------------------------------------------------ */

/*
 * There is deliberately no audio_enable() helper here.
 *
 * There was one, and it was called from pause -- which cleared the
 * audio block's EN bit, the DAC output enable for the whole device.
 * The keyboard went silent whenever the file was paused. Nothing in
 * this app should touch that bit: z_audio_start() sets it at startup
 * and z_audio_stop() clears it on the way out, and between those two
 * points the output stays on whether or not anything is playing.
 * Silence is a frame of zeros.
 */

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

/*
 * Release everything the FILE is playing, leaving the keyboard alone.
 *
 * Stopping or pausing a backing track should not cut the note under
 * the player's fingers -- playing over a paused file is a normal thing
 * to do. Channel 16 is the keyboard's alone (see KBD_CHANNEL), which
 * is what makes the two separable at all.
 */
static void synth_file_notes_off(void) {
    smf_event_t ev;
    int ch;
    memset(&ev, 0, sizeof(ev));
    ev.type = SMF_EV_CONTROL;
    ev.a = SMF_CC_ALL_NOTES_OFF;
    for (ch = 0; ch < SMF_CHANNELS; ch++) {
        if (ch == KBD_CHANNEL) continue;
        ev.channel = (uint8_t)ch;
        synth_event(&sy, &ev);
    }
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
    /*
     * Loaded but NOT started.
     *
     * The app opens as an instrument: the keyboard is live, nothing is
     * playing, and space starts the file. Auto-playing on launch is
     * the wrong default for something you might have opened to play
     * rather than to listen to -- and it is a worse surprise, because
     * the first thing it would do is take the audio device and start
     * making noise.
     *
     * next_file() overrides this when a track ends, so playing through
     * a directory still works once it has been started once.
     */
    state = ST_STOPPED;
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

/*
 * Advance the envelopes and push the voices, whatever the transport is
 * doing.
 *
 * Split out of sequence() because the KEYBOARD has to work while a
 * file is stopped or paused -- which is the app's default state. Tying
 * the envelope clock to file playback meant a note played from the
 * computer keyboard attacked and then froze at whatever level it had
 * reached, sustaining until its voice was stolen.
 */
static void synth_service(uint32_t dt) {

    env_accum += dt * TICK_US;
    if (env_accum >= 5000u) {
        synth_tick(&sy, env_accum / 1000u);
        env_accum = 0;
        voices_push();
    } else if (any_retrigger()) {
        /* A note-on must reach the mixer immediately -- delaying it by
         * up to 5ms is an audible late note, and on a keyboard that is
         * the difference between playable and not. */
        voices_push();
    }
}

static void sequence(void) {

    uint32_t now, dt;
    smf_event_t ev;
    int got, n;

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

    /* Envelopes and voices first, and unconditionally: the keyboard
     * plays whether or not a file is running. */
    synth_service(dt);

    if (state != ST_PLAYING || !have_file) return;

    got = smf_step(&sf, dt * TICK_US, &ev);
    while (got != SMF_EV_NONE && got != SMF_EV_END) {
        synth_event(&sy, &ev);
        got = smf_step(&sf, 0, &ev);
    }


    n = synth_active(&sy);
    if ((uint32_t)n > peak_voices) peak_voices = (uint32_t)n;

    if (got == SMF_EV_END) {
        if (nfiles) {
            next_file(1);
            /* Keep going: open_file() leaves a file stopped, which is
             * right on launch and wrong at the end of a track. */
            state = ST_PLAYING;
            last_tick = z_uptime_ticks();
        } else {
            state = ST_STOPPED;
            stop_all();
        }
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
        snprintf(b, sizeof(b), "MONO (diagnostic)   F1 = help");
    else
        snprintf(b, sizeof(b), "kbd C%d %s%s   space=play  F1=help",
            (kbd_base / 12) - 1, synth_family_name(kbd_program),
            n ? "  (muted)" : "");
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


/* ------------------------------------------------------------------
 * the on-screen keyboard
 *
 * -- the layout is the tracker layout, and that is not arbitrary --
 *
 * Two rows of the computer keyboard laid out as two octaves of a
 * piano: the ZXCV row is the lower octave with SDGHJ as its black
 * keys, and the QWERTY row is the octave above with 23567 as its
 * black keys. Every tracker and most DAWs use exactly this, so anyone
 * who has typed music before already knows it, and anyone who has not
 * can read it off the screen because the letters are drawn on the
 * keys.
 *
 * The cost is that it consumes almost the whole alphanumeric area, so
 * the transport moved to keys the layout does not touch -- space,
 * Enter, Tab, the arrows and F1..F4. Per-channel muting on 1..9 and 0
 * had to go entirely: those are black keys now. It was a diagnostic
 * aid and the keyboard is worth more.
 *
 * -- the two rows deliberately OVERLAP --
 *
 * The QWERTY row starts an octave above the ZXCV row, so semitones
 * 12..16 are reachable from both (`,` and `q` are the same note). That
 * is how the original layout works and it is not a bug to be tidied
 * up: it means a two-handed player can cross rows without a gap.
 * ------------------------------------------------------------------ */

static const struct { uint16_t ks; uint8_t semi; } kbd_map[] = {
    /* lower row: ZXCVBNM,./ with SDGHJL; as its black keys */
    { 'z',  0 }, { 's',  1 }, { 'x',  2 }, { 'd',  3 }, { 'c',  4 },
    { 'v',  5 }, { 'g',  6 }, { 'b',  7 }, { 'h',  8 }, { 'n',  9 },
    { 'j', 10 }, { 'm', 11 }, { ',', 12 }, { 'l', 13 }, { '.', 14 },
    { ';', 15 }, { '/', 16 },
    /* upper row: QWERTYUIOP with 2357 90 as its black keys */
    { 'q', 12 }, { '2', 13 }, { 'w', 14 }, { '3', 15 }, { 'e', 16 },
    { 'r', 17 }, { '5', 18 }, { 't', 19 }, { '6', 20 }, { 'y', 21 },
    { '7', 22 }, { 'u', 23 }, { 'i', 24 }, { '9', 25 }, { 'o', 26 },
    { '0', 27 }, { 'p', 28 }
};
#define KBD_MAP_N ((int)(sizeof(kbd_map) / sizeof(kbd_map[0])))

/* true for the black keys of a chromatic octave */
static const uint8_t semi_black[12] = { 0,1,0,1,0,0,1,0,1,0,1,0 };
static bool kbd_is_black(int semi) { return semi_black[semi % 12] != 0; }

/*
 * The keyboard plays on MIDI channel 16 (index 15).
 *
 * Not channel 1, which is what most files use for the melody -- the
 * point is to play ALONG with a file, and sharing a channel would mean
 * sharing its program, its volume and its pan, so the file would
 * reprogram the instrument under the player's fingers.
 */

/* Display letter per semitone. The lower row wins where both rows
 * reach a note, because that is the hand position most players start
 * from. */
static char kbd_letter(int semi) {
    int i;
    for (i = 0; i < KBD_MAP_N; i++)
        if (kbd_map[i].semi == (uint8_t)semi) return (char)kbd_map[i].ks;
    return ' ';
}

/* White-key index of a semitone, and the reverse. */
static int kbd_white_index(int semi) {
    static const int8_t w[12] = { 0,-1,1,-1,2,3,-1,4,-1,5,-1,6 };
    int oct = semi / 12;
    int k = w[semi % 12];
    return (k < 0) ? -1 : (oct * 7 + k);
}

static void kbd_layout(void) {
    int w = z_win_content_w(&win);
    /* 17 white keys across the content area. */
    kbd_kw = (w - 4) / 17;
    kbd_x = 2;
}

/*
 * Draw one key.
 *
 * One key, not the whole keyboard: a note on or off changes exactly
 * one, and repainting twenty-nine keys plus their letters for each
 * would be ~60 blitter fills and 29 glyph blits at every keypress.
 * Every fill here goes through the hardware blitter and every letter
 * through the glyph blitter (-DZ_GFX_HW_BLIT), but neither is free
 * enough to waste at that rate.
 */
/*
 * Geometry of one key. Shared by the drawing and the hit test, because
 * two copies of "where is this key" is how a keyboard ends up playing
 * the note next to the one you clicked.
 */
static void kbd_key_rect(int semi, int *px, int *py, int *pw, int *ph) {
    if (!kbd_is_black(semi)) {
        int wi = kbd_white_index(semi);
        *px = kbd_x + wi * kbd_kw;
        *pw = kbd_kw - 1;
        *ph = kbd_h;
    } else {
        /* A black key sits over the boundary between the two white
         * keys either side. */
        int wi = kbd_white_index(semi - 1);
        *px = kbd_x + (wi + 1) * kbd_kw - (kbd_kw / 3);
        *pw = (kbd_kw * 2) / 3;
        *ph = (kbd_h * 3) / 5;
    }
    *py = kbd_y;
}

/*
 * Draw one key.
 *
 * One key, not the whole keyboard: a note on or off changes exactly
 * one, and repainting twenty-nine keys plus their letters for each
 * would be ~60 blitter fills and 29 glyph blits at every keypress.
 * Every fill goes through the hardware blitter and every letter
 * through the glyph blitter (-DZ_GFX_HW_BLIT), but neither is free
 * enough to waste at that rate.
 *
 * The label goes through z_win_draw_text2() with an EXPLICIT
 * background. z_win_draw_text() takes one colour and the glyph
 * blitter paints a solid cell with the background hard-wired to 0
 * (zgfx.c), so a colour-0 letter on a light key is ink of 0 on a cell
 * of 0 -- not merely invisible, it erases the key underneath. That is
 * exactly how the black keys came out with no letters on them.
 */
static void kbd_draw_key(int semi) {

    int black = kbd_is_black(semi);
    int on = kbd_down[semi];
    int fg, bg, x, y, kw, kh;
    char lab[2];

    if (kbd_h < 8 || kbd_kw < 5) return;

    kbd_key_rect(semi, &x, &y, &kw, &kh);

    if (!black) {
        /* Outline while up, solid while down. */
        bg = on ? 1 : 0;
        fg = on ? 0 : 1;
        z_win_fill_rect(&win, x, y, kw, kh, bg);
        z_win_fill_rect(&win, x, y, kw, 1, fg);
        z_win_fill_rect(&win, x, y + kh - 1, kw, 1, fg);
        z_win_fill_rect(&win, x, y, 1, kh, fg);
        z_win_fill_rect(&win, x + kw - 1, y, 1, kh, fg);
    } else {
        /* Solid while up, outline while down -- the inverse of a white
         * key, which is what makes the two readable as different kinds
         * of key on a one-bit display. */
        bg = on ? 0 : 1;
        fg = on ? 1 : 0;
        z_win_fill_rect(&win, x, y, kw, kh, bg);
        z_win_fill_rect(&win, x, y, kw, 1, fg);
        z_win_fill_rect(&win, x, y + kh - 1, kw, 1, fg);
        z_win_fill_rect(&win, x, y, 1, kh, fg);
        z_win_fill_rect(&win, x + kw - 1, y, 1, kh, fg);
    }

    lab[0] = kbd_letter(semi);
    lab[1] = 0;
    if (lab[0] != ' ' && kw >= 7)
        z_win_draw_text2(&win, x + (kw - 5) / 2,
            y + kh - (black ? 10 : 11), lab, fg, bg, &z_font_5x8);

    kbd_drawn[semi] = (int8_t)on;
}

/*
 * Which key is under a content-relative point, or -1.
 *
 * Black keys are tested FIRST because they are drawn over the whites
 * and overlap them: a click in the overlap belongs to the black key,
 * which is what a real keyboard does and what the picture on screen
 * says.
 */
static int kbd_hit(int cx, int cy) {

    int i, x, y, kw, kh;

    if (kbd_h < 8 || kbd_kw < 5) return -1;
    if (cy < kbd_y || cy >= kbd_y + kbd_h) return -1;

    for (i = 0; i < KBD_SEMIS; i++) {
        if (!kbd_is_black(i)) continue;
        kbd_key_rect(i, &x, &y, &kw, &kh);
        if (cx >= x && cx < x + kw && cy < y + kh) return i;
    }
    for (i = 0; i < KBD_SEMIS; i++) {
        if (kbd_is_black(i)) continue;
        kbd_key_rect(i, &x, &y, &kw, &kh);
        if (cx >= x && cx < x + kw) return i;
    }
    return -1;
}

/* Whites first, then blacks over them -- a black key overlaps its
 * neighbours, so painting it second is what makes the overlap look
 * right rather than leaving a notch. */
static void kbd_draw_all(void) {
    int i;
    if (kbd_h < 8) return;
    z_win_fill_rect(&win, 0, kbd_y, z_win_content_w(&win), kbd_h, 0);
    for (i = 0; i < KBD_SEMIS; i++) if (!kbd_is_black(i)) kbd_draw_key(i);
    for (i = 0; i < KBD_SEMIS; i++) if (kbd_is_black(i)) kbd_draw_key(i);
}

/* Only the keys whose state changed, and a black key's white
 * neighbours after it, so the overlap is not left half-erased. */
static void kbd_refresh(void) {
    int i;
    if (kbd_h < 8) return;
    for (i = 0; i < KBD_SEMIS; i++) {
        if (kbd_drawn[i] == (int8_t)kbd_down[i]) continue;
        kbd_draw_key(i);
        if (!kbd_is_black(i)) {
            if (i > 0 && kbd_is_black(i - 1)) kbd_draw_key(i - 1);
            if (i + 1 < KBD_SEMIS && kbd_is_black(i + 1)) kbd_draw_key(i + 1);
        }
    }
}

/* keysym -> semitone, or -1 */
static int kbd_semi_of(uint32_t ks) {
    int i;
    for (i = 0; i < KBD_MAP_N; i++)
        if (kbd_map[i].ks == ks) return (int)kbd_map[i].semi;
    return -1;
}

static void kbd_note(int semi, bool on) {

    smf_event_t ev;
    int note = (int)kbd_base + semi;

    if (note < 0 || note > 127) return;

    memset(&ev, 0, sizeof(ev));
    ev.type = on ? SMF_EV_NOTE_ON : SMF_EV_NOTE_OFF;
    ev.channel = KBD_CHANNEL;
    ev.a = (uint8_t)note;
    ev.b = on ? 100 : 0;
    synth_event(&sy, &ev);
}

static void kbd_all_off(void) {
    int i;
    for (i = 0; i < KBD_SEMIS; i++)
        if (kbd_down[i]) { kbd_down[i] = false; kbd_note(i, false); }
    kbd_mouse_semi = -1;
}

static void kbd_set_program(int p) {
    smf_event_t ev;
    if (p < 0) p = 127;
    if (p > 127) p = 0;
    kbd_program = (uint8_t)p;
    memset(&ev, 0, sizeof(ev));
    ev.type = SMF_EV_PROGRAM;
    ev.channel = KBD_CHANNEL;
    ev.a = kbd_program;
    synth_event(&sy, &ev);
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

/*
 * Heights of the fixed rows. Named because layout() adds them up twice
 * -- once to place things and once to work out whether they fit.
 */
#define LAY_METER_H  12
#define LAY_KBD_H    30
#define LAY_BTN_H    18
#define LAY_GAP       3

static void layout(void) {

    int h = z_win_content_h(&win);
    int w = z_win_content_w(&win);
    int y = 2 + ROWS * ROW_H;
    int want_kbd = LAY_KBD_H;
    int want_meter = LAY_METER_H;
    int need;

    /*
     * PACKED FROM THE TOP, not spread between the top and the bottom.
     *
     * The previous version placed the text rows from the top and the
     * keyboard and buttons from the bottom, with the meters taking
     * whatever was left in between -- capped, so anything past the cap
     * became dead space. Packing downwards means a window bigger than
     * the layout has its slack in one place at the bottom, where it is
     * obvious, rather than as a hole in the middle.
     *
     * Degradation is in reverse order of importance: if the window is
     * too short, the keyboard goes first and then the meters, because
     * a cramped keyboard is worse than none and the buttons and status
     * rows have to stay.
     */
    need = y + 2 + want_meter + LAY_GAP + want_kbd + LAY_GAP
        + LAY_BTN_H + 2;
    if (h < need) {
        want_kbd = 0;
        need = y + 2 + want_meter + LAY_GAP + LAY_BTN_H + 2;
        if (h < need) want_meter = 0;
    }

    y += 2;
    meter_y = y;
    meter_h = want_meter;
    y += want_meter;

    if (want_kbd) {
        y += LAY_GAP;
        kbd_y = y;
        kbd_h = want_kbd;
        y += want_kbd;
    } else {
        kbd_h = 0;
        kbd_y = h;
    }
    kbd_layout();

    y += LAY_GAP;
    widgets[BTN_PLAY].x = 4;
    widgets[BTN_PLAY].y = (int16_t)y;
    widgets[BTN_STOP].x = 26;
    widgets[BTN_STOP].y = (int16_t)y;
    widgets[WID_VOL].x = 54;
    widgets[WID_VOL].y = (int16_t)(y + 3);
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
    {
        int i;
        for (i = 0; i < KBD_SEMIS; i++) kbd_drawn[i] = -1;
    }
    kbd_draw_all();
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
        /*
         * Only the FILE stops.
         *
         * This used to also call audio_enable(false), which clears the
         * audio block's EN bit -- and that is the DAC's output enable
         * for the WHOLE device, not a property of the file. The
         * sequencer stopped, the keyboard kept triggering voices, the
         * mixer kept mixing them, and none of it reached the output:
         * pressing pause silenced the instrument.
         *
         * It was correct before there was a keyboard, when "paused"
         * and "silent" meant the same thing. Now they do not. Nothing
         * here touches the output enable; z_audio_start() sets it once
         * and z_audio_stop() clears it on the way out.
         */
        synth_file_notes_off();
    } else {
        state = ST_PLAYING;
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
    synth_file_notes_off();
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

    /* The dialog runs its own message loop, so no key release can be
     * delivered while it is up -- anything held would be stuck on. Let
     * go of it, and clear kbd_down[] so the drawn keys match. */
    kbd_all_off();
    if (state == ST_PLAYING) { state = ST_PAUSED; stop_all(); }

    memset(&ctx, 0, sizeof(ctx));
    ctx.parent = &win;
    ctx.on_msg = on_dialog_msg;

    if (z_dialog_open(&ctx, "/AUDIO", path, sizeof(path))) {
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

/*
 * Every key, with its press/release edge.
 *
 * Notes need both: a key going down starts a note and a key coming up
 * releases it, and without the release every note sustains until its
 * voice is stolen. wm delivers both (Z_WM_UNPACK_KEY_PRESSED), which
 * is what makes a playable keyboard possible at all rather than a
 * fixed-length blip per keypress.
 *
 * Key repeat is suppressed by kbd_down[]: holding a key produces a
 * stream of presses and retriggering the note on each would be a
 * machine-gun, not a held note.
 */
static void handle_key(uint32_t ks, bool pressed) {

    int semi;

    if (show_help) {
        if (pressed) { show_help = false; draw_all(); }
        return;
    }

    /* -- notes first: they own most of the alphanumeric area -- */
    semi = kbd_semi_of(ks);
    if (semi >= 0) {
        if (pressed) {
            if (!kbd_down[semi]) {
                kbd_down[semi] = true;
                kbd_note(semi, true);
            }
        } else if (kbd_down[semi]) {
            kbd_down[semi] = false;
            kbd_note(semi, false);
        }
        kbd_refresh();
        return;
    }

    if (!pressed) return;

    switch (ks) {

        case ' ':          do_playpause(); break;
        case '\r':
        case '\n':         do_stop(); break;
        case '\t':         do_open(); break;

        case Z_KEY_RIGHT:  next_file(1); set_play_icon(); break;
        case Z_KEY_LEFT:   next_file(-1); set_play_icon(); break;

        case Z_KEY_UP:
        case Z_KEY_DOWN: {
            /* Shift the whole keyboard, releasing anything held first
             * -- the note a key is holding would otherwise never get
             * its note-off, because the key now means a different
             * note. */
            int b = (int)kbd_base + (ks == Z_KEY_UP ? 12 : -12);
            kbd_all_off();
            if (b < 0) b = 0;
            if (b > 96) b = 96;
            kbd_base = (uint8_t)b;
            kbd_refresh();
            dirty |= DIRTY_HINT;
            break;
        }

        case '[':          kbd_set_program(kbd_program - 8);
                           dirty |= DIRTY_HINT; break;
        case ']':          kbd_set_program(kbd_program + 8);
                           dirty |= DIRTY_HINT; break;

        case '-':          set_volume(volume - 4); break;
        case '=':
        case '+':          set_volume(volume + 4); break;

        case Z_KEY_F1:     show_help = true;
                           help_row = 0;
                           z_win_clear(&win);
                           z_widget_invalidate(&wset);
                           break;
        case Z_KEY_F2:     toggle_mute(SMF_DRUM_CHANNEL); break;
        case Z_KEY_F3:     force_mono = !force_mono;
                           dirty |= DIRTY_HINT; break;
        case Z_KEY_F4:     kbd_all_off();
                           synth_all_off(&sy);
                           voices_silence();
                           kbd_refresh();
                           break;

        case 27:           want_quit = true; break;
        default: break;
    }
}

static void handle_mouse(uint32_t packed) {

    int cx, cy, act, semi;
    uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);

    z_win_mouse_content_xy(&win, packed, &cx, &cy);

    act = z_widget_mouse(&wset, cx, cy, buttons);
    if (act == BTN_PLAY) { do_playpause(); return; }
    if (act == BTN_STOP) { do_stop(); return; }
    if (act == WID_VOL) { set_volume(widgets[WID_VOL].value); return; }

    if (show_help) return;

    /*
     * Clicking the keys.
     *
     * Held rather than triggered: the note lasts as long as the button
     * is down, the same as a computer key, so the envelope's release
     * means something. A click that fired a fixed-length blip would
     * make every instrument sound the same regardless of its ADSR.
     *
     * Dragging across the keyboard slides from note to note --
     * glissando -- because the current key is released and the new one
     * pressed whenever the one under the pointer changes. That falls
     * out of tracking which key the mouse holds rather than being a
     * feature written on purpose, which is the nice kind.
     */
    semi = (buttons & Z_MOUSE_BTN_LEFT) ? kbd_hit(cx, cy) : -1;

    if (semi != kbd_mouse_semi) {
        if (kbd_mouse_semi >= 0) {
            kbd_down[kbd_mouse_semi] = false;
            kbd_note(kbd_mouse_semi, false);
        }
        if (semi >= 0) {
            kbd_down[semi] = true;
            kbd_note(semi, true);
        }
        kbd_mouse_semi = semi;
        kbd_refresh();
    }
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
            handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg->obj.val.uint32),
                Z_WM_UNPACK_KEY_PRESSED(msg->obj.val.uint32) != 0);
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

    /*
     * /AUDIO, the same directory sw/apps/track and sw/apps/play use.
     *
     * One place for everything that makes a sound, rather than a
     * directory per app -- a card with AUDIO, MIDI and MOD folders
     * makes you remember which app wanted which, and the only thing
     * that actually distinguishes them is the extension, which
     * ext_is_mid() already checks. .MOD and .WAV files sitting
     * alongside are simply not listed here, and this app's .MID files
     * are not listed by the other two.
     *
     * Both cases are tried because FatFs is built with FF_USE_LFN 0
     * and matches 8.3 names case-insensitively, but the path given to
     * f_opendir() is taken as written.
     */
    scan_dir("/AUDIO");
    if (!nfiles) scan_dir("/audio");
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
            /* Console mode has no release edge from hid_read_key(),
             * so notes would sustain forever. Transport only. */
            int32_t ev = hid_read_key();
            if (ev >= 0)
                handle_key(z_kbd_usage_to_keysym((uint8_t)(ev & 0xFF),
                    (uint8_t)((ev >> 8) & 0xFF)), true);
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
