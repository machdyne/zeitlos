/*
 * play -- streaming audio file player
 *
 *   > run wm
 *   > run play
 *   > run play /AUDIO/SONG.WAV
 *
 * PHASE 1 of the streaming player. See docs/play_app.md.
 *
 * -- what makes this different from sw/apps/track --
 *
 * track loads a whole module into .bss and plays it from memory. That
 * works because a ProTracker module is under 100KB. A minute of
 * 44.1kHz 16-bit stereo audio is 10MB, so this one never holds more
 * than a fraction of the file and instead reads it continuously off
 * the SD card while it plays.
 *
 * Everything awkward about this app follows from that one difference,
 * and from one fact about the hardware: sdmm.c moves one byte per
 * MMIO write / poll / read cycle, about 48 CPU cycles a byte
 * (rtl/spisd.v's own header). SD bandwidth here is CPU, not waiting.
 * 44.1kHz 16-bit stereo is 176KB/s, which is roughly 18% of a whole
 * 48MHz core spent inside spi_xchg() -- and this process gets about a
 * third of the core with wm and a shell resident.
 *
 * So the design is: read in small bounded chunks, keep a large ring so
 * the reads do not have to be punctual, and measure everything.
 *
 * -- the loop shape, which is the whole design --
 *
 *     feed_fifo();          top up the hardware FIFO
 *     pump_ring();          ONE bounded read off the card
 *     feed_fifo();          top up again, immediately
 *     drain_messages();
 *     ui_step();            at most one text row
 *
 * The FIFO holds 1024 frames -- 23ms at 44.1kHz. That is the entire
 * margin for error, so nothing in the loop may block for longer than
 * it. A 2KB read is about 2ms of CPU, several ms of wall time once
 * shared three ways, and comfortably inside 23ms.
 *
 * The second feed_fifo() is not redundant. It is the one that runs
 * immediately after the only blocking call in the loop, which is
 * exactly when the FIFO is at its emptiest.
 *
 * -- the ring is what absorbs everything else --
 *
 * 32KB by default: 186ms at 44.1kHz 16-bit stereo, and four times
 * that for an IMA ADPCM file. It exists to cover the things the loop
 * shape cannot -- a FAT cluster-boundary lookup, another process
 * doing its own filesystem work, wm blocking on a redraw ack.
 *
 * -- what this app does to the audio device --
 *
 * It takes it. There is no arbitration for the audio block in this
 * system, so on startup this clears MIXEN and every hardware mixer
 * channel. If sw/apps/track is running, its music stops.
 *
 * That is deliberate and it is not politeness that is missing. A
 * player that left MIXEN wherever it found it would push frames into
 * a FIFO the DAC is not listening to, produce silence, and give no
 * indication why -- which is a considerably worse experience than
 * "the other player stopped". An audio server is the real answer and
 * is explicitly out of scope for this phase; see docs/play_app.md.
 *
 * -- controls --
 *
 *   space       play / pause
 *   s           stop (rewind to the start)
 *   o           open a file
 *   n / p       next / previous file in the directory
 *   - / =       volume
 *   , / .       seek back / forward 5 seconds
 *   w           waveform display on/off
 *   i           interpolation on/off
 *   q / ESC     quit (or the window's close icon)
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

#include "adec.h"
#include "stream.h"
#include "play_icons.h"
#include "prof.h"

/* ------------------------------------------------------------------
 * build knobs -- see the Makefile and docs/play_app.md
 * ------------------------------------------------------------------ */

/*
 * Ring capacity. MUST be a power of two (stream_init() refuses
 * otherwise) and is the single most important number in this app:
 * it is how long the SD side may be late before playback breaks.
 *
 *   32KB = 186ms at 44.1kHz/16/stereo, 744ms at IMA ADPCM
 *
 * Raise it on a board with memory to spare. Lower it and the first
 * thing that happens is not a click -- it is that `sv` on the status
 * line starts counting, which is the honest early warning.
 */
#ifndef PLAY_RING_SIZE
#define PLAY_RING_SIZE (32 * 1024)
#endif

/*
 * Bytes per filesystem read.
 *
 * A LATENCY budget, not a throughput knob -- the same distinction
 * track.c's FEED_BLOCKS comment makes, and for the same reason. This
 * is the longest the loop can be inside a blocking call, and the FIFO
 * has to survive it. 2KB is ~2ms of CPU; 23ms is the FIFO at 44.1kHz.
 *
 * Bigger is NOT better here. It amortises FatFs overhead slightly and
 * spends the entire safety margin doing it.
 */
#ifndef PLAY_CHUNK
#define PLAY_CHUNK 2048
#endif

/*
 * Frames pushed to the FIFO per feed_fifo() call.
 *
 * Also a latency budget. wm's repair_region() sends Z_WM_REDRAW and
 * BLOCKS on the ack (docs/window_manager.md), so an app that
 * disappears into a push loop for 40ms freezes the whole desktop for
 * 40ms. 256 frames is 5.8ms of audio at 44.1kHz and a few thousand
 * instructions, and the loop calls this twice a pass anyway.
 */
#ifndef PLAY_FEED_MAX
#define PLAY_FEED_MAX 256
#endif

#ifndef PLAY_VOLUME
#define PLAY_VOLUME 40          /* of ADEC_GAIN_MAX (64) */
#endif

/* Seconds between profiler reports. See the report block in main() on
 * why this is not 1. */
/*
 * Mixer-mode ring, in bytes. Power of two.
 *
 * Holds 8-bit samples rather than file bytes, and is read directly by
 * rtl/audio_mixer.v as a bus master -- so unlike PLAY_RING_SIZE this
 * is not a latency buffer for the SD card, it IS the audio. 32KB is
 * 372ms of 22050Hz stereo.
 *
 * Four bytes of slack past the loop length: channel 1's BASE is one
 * byte into the ring (see mixer_start()), so its last fetch before a
 * wrap lands one byte past PLAY_MIXRING. Reading four bytes of our own
 * .bss is harmless; reading four bytes of somebody else's is not, and
 * the mixer issues physical addresses so there is nothing between it
 * and whatever is next in memory.
 */
#ifndef PLAY_MIXRING
#define PLAY_MIXRING (32 * 1024)
#endif
#define PLAY_MIXRING_MASK (PLAY_MIXRING - 1)

#ifndef PLAY_PROF_SECS
#define PLAY_PROF_SECS 4
#endif

#ifndef PLAY_MAX_FILES
#define PLAY_MAX_FILES 64
#endif

#define PLAY_PATH_MAX 64

/* ------------------------------------------------------------------
 * state
 * ------------------------------------------------------------------ */

enum { ST_STOPPED = 0, ST_PLAYING, ST_PAUSED };

static uint8_t   ring[PLAY_RING_SIZE];

/*
 * THE MIXER IS A BUS MASTER AND ISSUES PHYSICAL ADDRESSES.
 *
 * This process sees its own memory through the MTU, which remaps
 * 0x8000_0000 to wherever the kernel actually put it. So `mixring` is
 * a VIRTUAL address and handing it to the mixer unchanged points the
 * sample fetches at whatever physically lives at that offset -- which
 * on this SOC is the BIOS and the kernel. It would play, and it would
 * play the wrong memory.
 *
 * reg_mtu_base reads as 0 where no translation is active, so this is
 * correct in both contexts with no special case. Same trap, same fix,
 * as sw/apps/track/track.c's phys_of().
 */
#define Z_APP_VIRT_BASE 0x80000000u

static uint32_t phys_of(const void *p) {
    return reg_mtu_base + ((uint32_t)p - Z_APP_VIRT_BASE);
}

/* 4-byte aligned: the mixer fetches whole words and selects a byte, so
 * an unaligned base costs nothing in correctness but the loop
 * arithmetic below assumes the ring starts on a word. */
static uint8_t   mixring[PLAY_MIXRING + 4] __attribute__((aligned(4)));

static bool      mix_mode;        /* mixer is driving playback */
static bool      mix_avail;       /* this bitstream has one at all */
static uint32_t  mix_head;        /* absolute bytes written to mixring */
static uint32_t  mix_lastpos;     /* previous MIXPOS, for lap detection */
static uint32_t  mix_laps;
static uint32_t  mix_starved;

/* Source frames already behind us when the ring was last (re)started.
 * mix_laps/mix_lastpos are reset by every mixer_start(), so without
 * this the elapsed time and the seek target both reset to zero at
 * every seek -- which made ',' and '.' appear to do nothing at all. */
static uint32_t  mix_base_src;
static bool      mix_padded;    /* silence already laid down at EOF */

/*
 * How source bytes become the signed 8-bit samples the mixer fetches.
 *
 * MIXCONV_U8     WAV 8-bit: unsigned, flip the top bit.
 * MIXCONV_HIBYTE 16-bit LE: take the HIGH byte. The top byte of a
 *                little-endian signed 16-bit sample IS the signed
 *                8-bit sample -- so this is a strided copy with no
 *                arithmetic in it at all, not a conversion.
 * MIXCONV_HIBE   16-bit BE (.au): the same byte, at the other offset.
 * MIXCONV_TAB    u-law / A-law: one lookup, through a byte table
 *                derived from adec's own expansion table so the two
 *                cannot disagree about the companding law.
 */
enum {
    MIXCONV_U8 = 0,     /* 8-bit unsigned  -> 8-bit ring, XOR */
    MIXCONV_HIBYTE,     /* 16-bit LE       -> 8-bit ring, high byte */
    MIXCONV_HIBE,       /* 16-bit BE       -> 8-bit ring, high byte */
    MIXCONV_TAB,        /* companded       -> 8-bit ring, lookup */
    MIXCONV_COPY16,     /* 16-bit LE       -> 16-bit ring, straight copy */
    MIXCONV_SWAP16,     /* 16-bit BE       -> 16-bit ring, byte swap */
    MIXCONV_TAB16       /* companded       -> 16-bit ring, lookup */
};
static int       mix_conv;
static int       mix_src_per;      /* source bytes consumed per unit */
static int       mix_ring_per;     /* ring bytes produced per unit */
static int       mix_bps;          /* ring bytes per sample: 1 or 2 */
static bool      mix_fmt16;        /* channels configured for 16-bit */
static int8_t    mix_tab[256];

/*
 * Why the mixer was NOT used for this file, or "" if it was.
 *
 * This exists because its absence cost three rounds of hardware
 * profiling. The app had two playback paths, chose between them
 * silently per file, and reported the choice only as one word buried
 * in a status row -- so a card still holding IMA files from an earlier
 * encoding run produced profiles that looked exactly like the mixer
 * doing nothing, when in fact the mixer was never engaged at all.
 *
 * A program that picks between two strategies must say which one it
 * picked and why, somewhere nobody can miss.
 */
static const char *mix_reject = "";

/* A one-line answer to "I pressed a key and nothing happened". Shown
 * on the diagnostics row until the next second tick clears it. */
static const char *mix_note = "";
static int16_t   outbuf[PLAY_FEED_MAX * 2];
static uint8_t   probe[ADEC_PROBE_BYTES];

static adec_t    dec;
static stream_t  st;

static int       state = ST_STOPPED;
static int       fh = -1;
static char      cur_path[PLAY_PATH_MAX];
static uint32_t  read_pos, data_end;
static uint32_t  out_hz;
static uint32_t  rate_div;
static int       volume = PLAY_VOLUME;
static bool      interp = true;
static bool      show_scope = true;
static bool      show_help;
static int       help_row;      /* next help line to paint, one per pass */
static bool      want_quit = false;
static bool      have_file = false;
static char      err_msg[48];

/* directory listing -- static, no malloc anywhere in this app. An
 * app's stack and heap share one 16KB tier (Z_PROC_STACK_SIZE_DEFAULT,
 * sw/os/kernel.h) and this is the same argument zflist.c makes for
 * doing its own listing without allocating. */
static char      files[PLAY_MAX_FILES][PLAY_PATH_MAX];
static int       nfiles;
static int       cur_file = -1;
static char      list_buf[4096];

/* -- measurement. This is why the app exists in this phase. -- */
static uint32_t  sd_bytes, sd_rate;         /* bytes/s off the card */
static uint32_t  last_starved;
static uint32_t  played_frames;             /* output frames this track */
static uint32_t  rr_start_tick;
static bool      saw_underrun;
static uint32_t  prof_secs, prof_el;

static z_win_t   win;
static bool      windowed;

/* ------------------------------------------------------------------
 * layout
 * ------------------------------------------------------------------ */

#define WIN_W 320
#define WIN_H 178

#define ROW_H (z_font_5x8.h + 1)
#define ROWS  5

#define BTN_PLAY  0
#define BTN_STOP  1
#define WID_VOL   2
#define WIDGET_COUNT 3

static z_widget_t widgets[WIDGET_COUNT];
static z_widget_set_t wset;

static int scope_y, scope_h, scope_x;
static int scope_min, scope_max;
static bool scope_have;

/*
 * The help overlay.
 *
 * A list of keys with no description is a reminder, not help -- it is
 * only readable by someone who already knows what the keys do. This is
 * the list with the meanings, which is what makes it worth the screen.
 *
 * Painted ONE LINE PER MAIN-LOOP PASS, like everything else here. A
 * text row costs ~16ms of wall time once the CPU is shared three ways,
 * so twelve of them in one call is 190ms -- survivable on the mixer
 * path, which has 372ms of ring in front of it, and an audible gap on
 * the software path, which has 23ms of FIFO. Doing it incrementally
 * costs a counter and works on both.
 */
static const struct { const char *key; const char *what; } help_keys[] = {
    { "space",   "play / pause" },
    { "s",       "stop, rewind to the start" },
    { "o",       "open a file (or the titlebar icon)" },
    { "n / p",   "next / previous file" },
    { "- / =",   "volume down / up" },
    { ", / .",   "seek back / forward 5 seconds" },
    { "w",       "waveform on / off" },
    { "i",       "interpolation (software path only)" },
    { "m",       "force the software path, for A/B" },
    { "h",       "this help" },
    { "q / esc", "quit" },
};
#define HELP_ROWS ((int)(sizeof(help_keys) / sizeof(help_keys[0])))

#define DIRTY_NAME  (1u << 0)
#define DIRTY_FMT   (1u << 1)
#define DIRTY_TIME  (1u << 2)
#define DIRTY_BUF   (1u << 3)
#define DIRTY_DIAG  (1u << 4)
static uint32_t dirty;

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
 * Take exclusive ownership of the audio block.
 *
 * The mixer is a bus master reading sample data from physical
 * addresses (rtl/audio_mixer.v). If track was killed rather than
 * exiting cleanly, its channels are still enabled and still fetching
 * from memory the kernel has since handed to somebody else -- so this
 * is not only about being heard, it is about stopping a DMA engine
 * that is reading a dead process's buffer.
 *
 * MIXEN in particular has to be cleared or every frame pushed below
 * goes into a FIFO the DAC is not reading, and the symptom is perfect
 * silence with a status line that says everything is fine.
 */
static void audio_claim(void) {
    int c;
    mix_avail = z_audio_mixer_present();
    if (mix_avail)
        for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
    z_audio_mixer_enable(false);
}

/* ------------------------------------------------------------------
 * file handling
 * ------------------------------------------------------------------ */

static bool ext_playable(const char *name) {

    static const char *exts[] = { "wav", "au", "snd", "raw", "pcm", 0 };
    const char *dot = 0, *p;
    int e;

    for (p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return false;
    dot++;

    for (e = 0; exts[e]; e++) {
        int i;
        for (i = 0; exts[e][i]; i++) {
            char c = dot[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != exts[e][i]) break;
        }
        if (!exts[e][i] && dot[i] == 0) return true;
    }
    return false;
}

/*
 * Scan a directory for playable files.
 *
 * Extension-based, as agreed. Note that FatFs is built here with
 * FF_USE_LFN 0 (sw/os/fs/fatfs/ffconf.h), so every name is an 8.3
 * short name -- "MYSONG~1.WAV" is what a long name looks like from
 * this side. tools/mkaudio.py names its output accordingly.
 *
 * A .MOD in the directory is skipped rather than played: modules are
 * loaded whole and driven through the hardware mixer, which is a
 * different engine entirely (sw/apps/track). Mixing the two into one
 * playlist is phase 3.
 */
static void scan_dir(const char *dir) {

    uint32_t count = 0, trunc = 0;
    const char *p;
    uint32_t i;

    if (!fs_list_into(dir, list_buf, sizeof(list_buf), 0,
        PLAY_MAX_FILES * 2, &count, &trunc))
        return;

    p = list_buf;
    for (i = 0; i < count && nfiles < PLAY_MAX_FILES; i++) {
        size_t l = strlen(p);
        if (ext_playable(p) && l < PLAY_PATH_MAX) {
            strcpy(files[nfiles], p);
            nfiles++;
        }
        p += l + 1;
    }
}

/* -- mixer-driven playback, defined below the drawing section --
 * Forward-declared because open_track()/close_track()/seek_to_frame()
 * all reach into it and they come first; moving the definitions up
 * instead would put 200 lines of playback engine in the middle of the
 * file-handling code. */
static bool mixer_supported(void);
static void mixer_start(void);
static void mixer_stop(void);
static void mixer_set_gain(void);
static void mixer_config_conv(void);
static void mixer_conv(uint8_t *dst, const uint8_t *src, uint32_t nout);
static uint32_t mixer_played_src(void);

static void close_track(void) {
    if (mix_mode) { mixer_stop(); mix_mode = false; }
    if (fh >= 0) {
        fs_close_handle(fh);
        fh = -1;
    }
    have_file = false;
}

static void seek_to_frame(uint32_t frame);

/*
 * Open `path` and get ready to play it.
 *
 * Every failure path closes the handle. There are only Z_FS_MAX_OPEN
 * (8) handles in the whole system and zfs.h documents that a handle
 * belonging to a process that dies without closing it is never
 * released -- so a player that leaks one on a bad file runs out after
 * four bad files and then cannot open a good one either.
 */
static bool open_track(const char *path) {

    int n, rc;
    uint32_t size;

    close_track();
    err_msg[0] = 0;
    state = ST_STOPPED;

    size = (uint32_t)fs_size((char *)path);
    if (!size) {
        snprintf(err_msg, sizeof(err_msg), "not found or empty");
        return false;
    }

    fh = fs_open_read(path);
    if (fh < 0) {
        snprintf(err_msg, sizeof(err_msg), "cannot open");
        return false;
    }

    n = fs_read_chunk(fh, probe, (int)sizeof(probe));
    if (n <= 0) {
        snprintf(err_msg, sizeof(err_msg), "read failed");
        close_track();
        return false;
    }

    rc = adec_parse(probe, (uint32_t)n, size, path, &dec);
    if (rc != ADEC_OK) {
        snprintf(err_msg, sizeof(err_msg), "%s", adec_error_name(rc));
        close_track();
        return false;
    }

    adec_set_gain(&dec, volume);

    if (!stream_init(&st, ring, PLAY_RING_SIZE, &dec, out_hz)) {
        /* Only reachable for a block size beyond STREAM_MAX_BLOCK or a
         * rate the parser let through -- both worth naming rather than
         * reporting as a generic failure, because the fix is a rebuild
         * or a re-encode and those are different actions. */
        snprintf(err_msg, sizeof(err_msg), "block %lu too large",
            (unsigned long)dec.block);
        close_track();
        return false;
    }
    stream_set_interp(&st, interp);

    strncpy(cur_path, path, sizeof(cur_path) - 1);
    cur_path[sizeof(cur_path) - 1] = 0;
    data_end = dec.data_off + dec.data_len;
    have_file = true;

    seek_to_frame(0);

    /* Mixer if the file suits it, FIFO otherwise. Decided per FILE
     * rather than once at startup: a playlist can mix 8-bit and 16-bit
     * material and each should get the path that can actually carry
     * it. */
    mix_mode = mixer_supported();
    if (mix_mode) mixer_start();
    else mixer_stop();

    /* Say which path this file got, and if it is the slow one, say
     * why and what to do about it. On the console, every time, not
     * only in the window -- see mix_reject's own comment. */
    printf("play: %s  %s %dch %luHz -> %s\n", path,
        adec_codec_name(dec.codec), dec.channels,
        (unsigned long)dec.rate,
        mix_mode ? (mix_fmt16 ? "MIXER 16-bit"
                              : (mix_src_per == 2 ? "MIXER 8-bit (truncated)"
                                                  : "MIXER 8-bit"))
                 : "FIFO -- SOFTWARE PATH");
    if (!mix_mode)
        printf("play: not using the mixer: %s\n", mix_reject);
    PROF_PATH(mix_mode ? "MIXER" : "FIFO");

    state = ST_PLAYING;
    dirty = DIRTY_NAME | DIRTY_FMT | DIRTY_TIME | DIRTY_BUF | DIRTY_DIAG;
    return true;
}

/*
 * Reposition. The seek offset is rounded to a decodable boundary by
 * adec_seek_offset() -- for IMA that is a whole ADPCM block, because
 * the predictor state lives in the block header and starting mid-block
 * decodes the rest of it as a burst of noise.
 */
static void seek_to_frame(uint32_t frame) {

    uint32_t off;

    if (fh < 0) return;

    off = adec_seek_offset(&dec, frame);
    if (!fs_seek(fh, off)) return;

    read_pos = off;
    played_frames = adec_frame_at_offset(&dec, off);

    stream_reset(&st);
    stream_set_interp(&st, interp);

    /* In mixer mode the ring IS the audio, so a seek has to refill it
     * and retrigger rather than flush a FIFO. mixer_start() does both;
     * calling it here rather than duplicating the prefill is what
     * keeps "where does the ring get filled" a question with one
     * answer. */
    if (mix_mode) { mixer_start(); return; }

    /* Whatever is in the FIFO is from before the seek. Playing it out
     * first is a fraction of a second of the old position after the
     * user asked for a new one -- cheaper to discard than to explain.
     * FLUSH is a command bit and does not disturb EN. */
    reg_audio_ctrl = (reg_audio_ctrl & 0xFFu) | Z_AUDIO_CTRL_FLUSH;

    dirty |= DIRTY_TIME;
}

static void next_track(int delta) {

    int i;

    if (!nfiles) return;

    i = cur_file + delta;
    if (i >= nfiles) i = 0;
    if (i < 0) i = nfiles - 1;
    cur_file = i;

    /* Wrap rather than stop at the end: this is a player meant to be
     * left running while other things happen, same as track. */
    if (!open_track(files[cur_file]))
        state = ST_STOPPED;
}

/* ------------------------------------------------------------------
 * the streaming loop
 * ------------------------------------------------------------------ */

/* ONE bounded read. See the file header on why this is 2KB and why
 * bigger is not better. */
static void pump_ring(void) {

    uint8_t *p;
    uint32_t room, want;
    int got;

    if (fh < 0 || !have_file) return;

    if (read_pos >= data_end) {
        stream_set_eof(&st, true);
        return;
    }

    room = stream_write_ptr(&st, &p);
    if (!room) return;

    want = (room < PLAY_CHUNK) ? room : PLAY_CHUNK;
    if (want > data_end - read_pos) want = data_end - read_pos;
    if (!want) return;

    PROF_BEGIN(P_PUMP);
    got = fs_read_chunk(fh, p, (int)want);
    PROF_END(P_PUMP);
    if (got <= 0) {
        /* 0 is a clean EOF and negative is a real failure, but both
         * mean the same thing here: there is no more audio coming and
         * what is buffered should still play out. */
        stream_set_eof(&st, true);
        return;
    }

    stream_commit(&st, (uint32_t)got);
    read_pos += (uint32_t)got;
    sd_bytes += (uint32_t)got;

    if (read_pos >= data_end) stream_set_eof(&st, true);
}

/*
 * Peak envelope for the scope, sampled rather than measured.
 *
 * Every eighth frame, because two compares per frame at 44.1kHz is
 * 88,000 comparisons a second spent on decoration. One in eight is
 * 11,000 and still catches the envelope -- the scope is showing the
 * shape of the audio, not resolving individual cycles, and at one
 * column per loop pass it is already averaging thousands of frames
 * into each column.
 */
/*
 * Same, for the mixer path's 8-bit ring.
 *
 * The scope stopped working the moment the mixer path landed, and the
 * reason is worth stating: scope_accum() was called from feed_fifo(),
 * which in mixer mode never runs. Anything hung off the software
 * path's inner loop silently stopped -- the waveform, the seek
 * position, and the elapsed time all had the same cause.
 *
 * `stride` skips the other channel: the ring is interleaved, so the
 * left channel is every second byte for a stereo stream and every byte
 * for a mono one.
 */
static void scope_accum_ring(const uint8_t *f, uint32_t n) {

    /* Left channel only, every eighth frame -- so the byte stride is
     * sample width times channel count times eight. Reading the ring
     * rather than the decoder's output is what keeps this working on
     * a path where the CPU never sees an output frame. */
    uint32_t stride = (uint32_t)mix_bps * (uint32_t)dec.channels * 8u;
    uint32_t i;

    if (!show_scope || !stride) return;

    for (i = 0; i + (uint32_t)mix_bps <= n; i += stride) {
        int v = (mix_bps == 2)
            ? (int)(int16_t)(uint16_t)(f[i] | ((uint16_t)f[i + 1] << 8))
            : (((int)(int8_t)f[i]) << 8);
        if (!scope_have) { scope_min = scope_max = v; scope_have = true; }
        else {
            if (v < scope_min) scope_min = v;
            if (v > scope_max) scope_max = v;
        }
    }
}

static void scope_accum(const int16_t *f, uint32_t n) {

    uint32_t i;

    if (!show_scope) return;

    for (i = 0; i < n; i += 8) {
        int v = f[i * 2];
        if (!scope_have) { scope_min = scope_max = v; scope_have = true; }
        else {
            if (v < scope_min) scope_min = v;
            if (v > scope_max) scope_max = v;
        }
    }
}

/*
 * Top up the FIFO, bounded.
 *
 * Reads the free space ONCE and pushes into it with the unchecked
 * push, rather than calling z_audio_push() per frame -- that costs a
 * STATUS read per frame, which at 44.1kHz is 44,000 extra MMIO reads
 * a second asking a question the loop bound already answers. zaudio.h
 * says so explicitly; this is the loop it is describing.
 */
static uint32_t feed_fifo(void) {

    uint32_t space, n, i;

    if (state != ST_PLAYING || !have_file) return 0;

    space = z_audio_space();
    if (!space) return 0;

    /*
     * Don't render a handful of frames.
     *
     * Every pass of the main loop costs a fixed amount -- a message
     * drain, a scope column, a uptime syscall -- whether it produced
     * 256 frames or 8. Measured on hardware: a run where the FIFO
     * stayed near full did 1515 passes for 32512 frames, 21 frames a
     * pass, and the fixed overhead alone came to 180 cycles per
     * output frame against a 350-cycle budget. The same workload with
     * 85 large passes ran a third faster.
     *
     * So if the FIFO is comfortable and there is only a sliver of
     * room, leave it: the space will still be there next pass, larger,
     * and the pass that fills it will amortise its overhead properly.
     * The floor is deliberately conditional on the FIFO being above a
     * quarter -- when it is genuinely draining, every frame counts and
     * this must not stand in the way.
     */
    if (space < 64 && z_audio_level() > 256) return 0;

    if (space > PLAY_FEED_MAX) space = PLAY_FEED_MAX;

    {
        PROF_BEGIN(P_RENDER);
        n = stream_render(&st, outbuf, space);
        PROF_END(P_RENDER);
    }
    if (!n) return 0;

    {
        PROF_BEGIN(P_PUSH);
        for (i = 0; i < n; i++)
            z_audio_push_unchecked(outbuf[i * 2], outbuf[i * 2 + 1]);
        PROF_END(P_PUSH);
    }
    PROF_ADD_OUT(n);

    scope_accum(outbuf, n);
    played_frames += n;
    return n;
}


/* ------------------------------------------------------------------
 * mixer-driven playback
 *
 * The CPU stops touching output frames entirely. rtl/audio_mixer.v
 * walks a ring buffer in main memory as a bus master, doing the phase
 * accumulation, the rate conversion and the gain in gateware on a
 * clock that cannot be preempted. This code's whole remaining job is
 * to keep bytes in front of it.
 *
 * -- why this exists --
 *
 * Measured on hardware with PLAY_PROF=1, the FIFO path spends about
 * 1546 cycles per output frame against a budget of 358. Of that,
 * ~1128 is stream_render() and ~122 is the push loop -- both purely a
 * consequence of the CPU visiting all 46875 output frames a second.
 * At the measured IPC of 0.08 the budget is roughly 29 instructions
 * per output frame, which is less than it takes to interpolate two
 * channels and store them. There is no software arrangement that
 * fits; see docs/play_app.md.
 *
 * This path visits input BYTES instead, once each, with no
 * arithmetic. At 22050Hz 8-bit stereo that is 44100 bytes a second at
 * the measured 49 cycles/byte -- about 13% of this process's share.
 *
 * -- why 8-bit only --
 *
 * The mixer fetches one signed byte per channel per frame. That is
 * what it was built for (MOD samples) and widening it to 16 bits is
 * not free: it needs a halfword select, a wider accumulator (24 bits
 * today, and eight channels of 16x8 products overflow it) and a
 * different output shift. So mixer mode accepts 8-bit WAV and nothing
 * else; every other format falls back to the FIFO path, which still
 * works and is still the reference.
 *
 * -- level --
 *
 * The mixer's output stage is sum(sample * gain) * mixvol >> 10,
 * sized so that EIGHT channels at full gain reach full scale. Two
 * channels therefore top out around a quarter of full scale, about
 * 12dB below what the FIFO path produces from the same material.
 * That is a property of the hardware, not of this code, and on a
 * 1-bit sigma-delta output that drives headphones directly with no
 * attenuator it is not obviously the wrong end of the range to be at.
 * ------------------------------------------------------------------ */

/* Bytes kept between the write head and the mixer's read position.
 * Not a safety margin against the mixer -- it never stalls -- but
 * against writing a byte the mixer is about to fetch, which would be
 * heard as a click rather than as a dropout. */
#define MIX_GUARD 64

/*
 * Can this file go through the mixer?
 *
 * The mixer fetches one signed byte per channel per frame, so
 * everything here ends up 8-bit whatever it started as. What decides
 * the answer is not the file's depth, it is whether the CONVERSION is
 * cheap enough to do at the source rate.
 *
 *   8-bit  (PCM_U8)     one XOR per byte
 *   16-bit (PCM_S16LE)  a strided copy -- take the high byte
 *   u-law / A-law       one table lookup per byte
 *
 *   IMA ADPCM           ~900 cycles per source frame, measured. At
 *                       22050Hz that is 118% of this process's whole
 *                       CPU share before a single byte is read off
 *                       the card. Refused -- it goes to the FIFO
 *                       path, where it is equally hopeless but at
 *                       least the numbers are comparable with the
 *                       rest of that path's.
 *
 * So a 16-bit WAV is supported, and is truncated to 8 bits on the way
 * into the ring. That is a real loss of resolution and it is stated on
 * the format line as "MIXER 8b". Encoding at 8 bits with mkaudio.py
 * instead loses exactly the same resolution and halves the SD traffic
 * doing it -- which is why that is the default. Playing a 16-bit file
 * here is the convenience path, not the good one.
 */
static bool mixer_supported(void) {

    mix_reject = "";

    if (!have_file) { mix_reject = "no file"; return false; }
    if (!mix_avail) {
        mix_reject = "no mixer in this bitstream";
        return false;
    }
    if (dec.channels != 1 && dec.channels != 2) {
        mix_reject = "channel count";
        return false;
    }

    switch (dec.codec) {
        case ADEC_PCM_U8:
        case ADEC_PCM_S16LE:
        case ADEC_PCM_S16BE:
        case ADEC_ULAW:
        case ADEC_ALAW:
            return true;
        case ADEC_IMA_WAV:
            /* Not a limitation of the mixer -- a limitation of the
             * CPU. Decoding IMA costs ~900 cycles per source frame,
             * measured, which at 22050Hz is more than this process's
             * entire share before a byte is read off the card. The
             * mixer would have nothing to be starved of. */
            mix_reject = "IMA: 5.5x the CPU of wav8 for identical output";
            return false;
        default:
            mix_reject = "codec not mixer-capable";
            return false;
    }
}

/*
 * Pick the conversion, and build its table if it needs one.
 *
 * The table comes from adec's OWN tab8 rather than from a second copy
 * of the G.711 arithmetic -- with the gain forced to unity first,
 * because in mixer mode volume is the mixer's job (CH_CTRL gains) and
 * folding it in here as well would apply it twice.
 */
/*
 * Pick the conversion, the sample width of the ring, and therefore the
 * channel format.
 *
 * The rule is: use 16-bit whenever the SOURCE has more than 8 bits to
 * give and the hardware can fetch them. An 8-bit file stays 8-bit --
 * widening it would double the ring traffic to carry a zero byte, and
 * halve how much audio the ring holds, for no gain whatsoever.
 *
 * So:
 *   8-bit WAV        -> 8-bit ring   (nothing to gain)
 *   16-bit WAV/.au   -> 16-bit ring  (straight copy, and CHEAPER than
 *                                     the strided high-byte copy it
 *                                     replaces)
 *   u-law / A-law    -> 16-bit ring  (the expansion table is already
 *                                     16-bit; truncating it was
 *                                     throwing away ~5 bits)
 *
 * ...and every one of those falls back to the 8-bit form on a
 * bitstream whose mixer predates CH_CTRL[18]. Checked, not assumed:
 * an older mixer ACCEPTS the bit and ignores it, so a 16-bit ring
 * would play as 8-bit garbage at half pitch, which is a much worse
 * failure than not offering the feature.
 */
static void mixer_config_conv(void) {

    int i;
    bool w16 = mix_avail && z_audio_mixer_fmt16();

    switch (dec.codec) {

        case ADEC_PCM_S16LE:
            mix_conv = w16 ? MIXCONV_COPY16 : MIXCONV_HIBYTE;
            mix_src_per = 2;
            mix_ring_per = w16 ? 2 : 1;
            break;

        case ADEC_PCM_S16BE:
            mix_conv = w16 ? MIXCONV_SWAP16 : MIXCONV_HIBE;
            mix_src_per = 2;
            mix_ring_per = w16 ? 2 : 1;
            break;

        case ADEC_ULAW:
        case ADEC_ALAW:
            /* Gain forced to unity: in mixer mode volume is the
             * mixer's job (CH_CTRL gains) and folding it in here as
             * well would apply it twice. */
            adec_set_gain(&dec, ADEC_GAIN_UNITY);
            if (!w16)
                for (i = 0; i < 256; i++)
                    mix_tab[i] = (int8_t)(dec.tab8[i] >> 8);
            mix_conv = w16 ? MIXCONV_TAB16 : MIXCONV_TAB;
            mix_src_per = 1;
            mix_ring_per = w16 ? 2 : 1;
            break;

        case ADEC_PCM_U8:
        default:
            mix_conv = MIXCONV_U8;
            mix_src_per = 1;
            mix_ring_per = 1;
            break;
    }

    /* A "unit" above is one SAMPLE, so ring bytes per unit is exactly
     * the ring's sample width. */
    mix_bps = mix_ring_per;
    mix_fmt16 = (mix_bps == 2);
}

/* Source bytes -> `nout` RING BYTES (not samples: a 16-bit ring
 * produces two per sample). */
static void mixer_conv(uint8_t *dst, const uint8_t *src, uint32_t nout) {
    uint32_t i;
    switch (mix_conv) {
        case MIXCONV_HIBYTE:
            for (i = 0; i < nout; i++) dst[i] = src[i * 2 + 1];
            break;
        case MIXCONV_HIBE:
            for (i = 0; i < nout; i++) dst[i] = src[i * 2];
            break;

        /* 16-bit LE straight through. The mixer reads a halfword out
         * of a 32-bit word little-endian, which is exactly how a
         * 16-bit WAV already sits on the card -- so this is a copy,
         * and CHEAPER than the strided high-byte version it replaces:
         * same SD traffic, no per-sample indexing. */
        case MIXCONV_COPY16:
            memcpy(dst, src, nout);
            break;

        case MIXCONV_SWAP16:
            for (i = 0; i + 1 < nout; i += 2) {
                dst[i] = src[i + 1];
                dst[i + 1] = src[i];
            }
            break;

        /* Companded to full 16 bits. dec.tab8 is already int16 at
         * unity gain, so this is the same lookup the 8-bit path does
         * without the >> 8 that was discarding five bits of it. */
        case MIXCONV_TAB16:
            for (i = 0; i + 1 < nout; i += 2) {
                int16_t v = dec.tab8[src[i >> 1]];
                dst[i] = (uint8_t)(v & 0xff);
                dst[i + 1] = (uint8_t)((uint16_t)v >> 8);
            }
            break;
        case MIXCONV_TAB:
            for (i = 0; i < nout; i++) dst[i] = (uint8_t)mix_tab[src[i]];
            break;
        default:
            for (i = 0; i < nout; i++) dst[i] = src[i] ^ 0x80;
            break;
    }
}

/*
 * Copy from the card into the ring, converting to signed as it goes.
 *
 * The conversion is one XOR per byte and it is the only arithmetic in
 * this entire path. It exists because WAV 8-bit PCM is the one depth
 * WAV stores UNSIGNED -- 0x80 is silence -- while the mixer treats
 * every sample as signed. Getting it wrong does not produce noise, it
 * produces the correct audio with a large DC offset: inaudible on a
 * small speaker and a warm resistor on headphones.
 */
/*
 * Bytes written but not yet played, as a SIGNED quantity.
 *
 * This exists because it was unsigned, and that bug had the player
 * running 26 seconds past the end of a 3:22 track.
 *
 * The mixer never stops. Once the file runs out, `mix_head` stops
 * advancing while the mixer keeps consuming and wrapping, so
 * `consumed` overtakes it and `mix_head - consumed` underflows to
 * something near 2^32. Every test then read backwards: mixer_fill()
 * saw "more used than the ring holds" and returned, and the
 * end-of-track check saw "not nearly drained yet" and never fired --
 * so the ring looped its stale contents indefinitely while the clock
 * counted on past the track length.
 *
 * Both readings were wrong in the same direction and for the same
 * reason, which is why the symptom was silence-free: it kept playing
 * something, and the something was the last 743ms of the song.
 */
static int32_t mixer_used(void) {
    uint32_t consumed = mix_laps * PLAY_MIXRING + mix_lastpos;
    return (int32_t)(mix_head - consumed);
}

/*
 * At end of file, lay a band of silence in front of the write head.
 *
 * What is ahead of the head is the OLDEST data in the ring, and the
 * mixer will play it -- there is no "stop at the end" for a looping
 * channel. Without this the last fraction of a second of every track
 * is a snatch of audio from a second earlier.
 */
static void mixer_pad_silence(void) {
    uint32_t idx = mix_head & PLAY_MIXRING_MASK;
    uint32_t n = MIX_GUARD * 4;
    uint32_t to_end = PLAY_MIXRING - idx;
    if (n > to_end) n = to_end;
    memset(mixring + idx, 0, n);
}

static void mixer_fill(void) {

    uint32_t pos, room, want, srcwant;
    int32_t used;
    uint8_t *p;
    int got;

    if (fh < 0 || !have_file) return;

    /* Where the mixer has got to. MIXPOS is a byte offset inside the
     * ring, so it wraps; laps are counted by watching it go backwards.
     * One read per pass, of a value that only advances once per audio
     * frame -- there is nothing to be gained by reading it more. */
    pos = z_audio_ch_pos_bytes(0) & PLAY_MIXRING_MASK;
    if (pos < mix_lastpos) mix_laps++;
    mix_lastpos = pos;

    used = mixer_used();

    /* The mixer never underruns -- it just replays whatever is still
     * in the ring. That is a quieter failure than the FIFO's sticky
     * UNDERRUN bit and needs its own detection, or a starving player
     * sounds like a stuttering one with no indication why.
     *
     * Negative means the mixer has overtaken the write head entirely,
     * which is the same fault seen later. */
    if (used < (int32_t)MIX_GUARD) mix_starved++;
    if (used < 0) used = 0;
    if (used > (int32_t)PLAY_MIXRING) used = (int32_t)PLAY_MIXRING;

    room = PLAY_MIXRING - (uint32_t)used;
    if (room <= MIX_GUARD) return;
    room -= MIX_GUARD;

    if (read_pos >= data_end) {
        if (!mix_padded) { mixer_pad_silence(); mix_padded = true; }
        return;
    }

    want = (room < PLAY_CHUNK) ? room : PLAY_CHUNK;

    /* Stop at the ring's end rather than stitching a read across the
     * wrap: two short reads a lap is cheaper than the bookkeeping. */
    {
        uint32_t idx = mix_head & PLAY_MIXRING_MASK;
        uint32_t to_end = PLAY_MIXRING - idx;
        if (want > to_end) want = to_end;
        p = mixring + idx;
    }

    /* Ring bytes wanted -> source bytes needed, then back again, so
     * both ends land on whole samples. Two ratios rather than one
     * divisor because the conversion can now EXPAND (u-law is one
     * source byte to two ring bytes) as well as shrink. */
    want -= (want % (uint32_t)mix_ring_per);
    srcwant = (want / (uint32_t)mix_ring_per) * (uint32_t)mix_src_per;
    if (srcwant > data_end - read_pos) {
        srcwant = data_end - read_pos;
        srcwant -= (srcwant % (uint32_t)mix_src_per);
        want = (srcwant / (uint32_t)mix_src_per) * (uint32_t)mix_ring_per;
    }
    if (!want || !srcwant) return;

    /*
     * Staged through `ring`, the FIFO path's buffer.
     *
     * The two paths are mutually exclusive, so 32KB of .bss is
     * already sitting there unused whenever this one is running.
     * Reading straight into `mixring` and converting in place would
     * save the copy for the 1:1 conversions and be flatly wrong for
     * the 2:1 one, which needs twice as many source bytes as it
     * produces -- and a buffer that is sometimes in-place and
     * sometimes not is how a subtle overlap bug gets written.
     */
    PROF_BEGIN(P_PUMP);
    got = fs_read_chunk(fh, ring, (int)srcwant);
    PROF_END(P_PUMP);

    if (got <= 0) return;

    got -= (got % mix_src_per);        /* whole samples only */
    if (got <= 0) return;

    {
        uint32_t nring = ((uint32_t)got / (uint32_t)mix_src_per)
            * (uint32_t)mix_ring_per;

        PROF_BEGIN(P_PUSH);
        mixer_conv(p, ring, nring);
        PROF_END(P_PUSH);

        scope_accum_ring(p, nring);

        mix_head += nring;
        read_pos += (uint32_t)got;
        sd_bytes += (uint32_t)got;
        PROF_ADD_SRC(nring
            / ((uint32_t)mix_bps * (uint32_t)dec.channels));
    }
}

static void mixer_stop(void) {
    int c;
    for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
    z_audio_mixer_enable(false);
}

/*
 * Point the mixer at the ring and start it.
 *
 * Both channels share one interleaved ring; channel 1's BASE is one
 * byte further in, which is how it reads the right-hand sample of each
 * frame. That is a workaround for a real constraint rather than a
 * preference: CH_CTRL's TRIG offset field is in units of 256 BYTES,
 * so there is no way to start a channel at byte 1. Offsetting BASE
 * costs nothing and keeps the ring a single contiguous span that the
 * filesystem can read straight into.
 *
 * STEP is in BYTES per output frame, so a stereo stream steps two
 * bytes per source frame and the ratio is doubled. No 64-bit
 * arithmetic: 2 * 200000 << 14 is comfortably inside 32 bits, and
 * adec_parse() already refuses a rate above 200000.
 */
static void mixer_start(void) {

    uint32_t base = phys_of(mixring);
    uint32_t step;
    uint8_t g = (volume >= 64) ? 255 : (uint8_t)(volume * 4);
    int c;

    mix_head = 0;
    mix_lastpos = 0;
    mix_laps = 0;
    mix_starved = 0;
    mix_padded = false;
    /* Captured BEFORE the prefill: bytes about to be read into the
     * ring have not been played yet. */
    mix_base_src = adec_frame_at_offset(&dec, read_pos);

    for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
    z_audio_mixer_enable(false);

    mixer_config_conv();

    /* Prefill before starting. The mixer begins consuming the instant
     * MIXEN goes up and has no notion of "not ready yet", so starting
     * on an empty ring plays a bufferful of whatever .bss held.
     *
     * Zeroed rather than left alone for the same reason: a partial
     * prefill at the end of a short file would otherwise loop
     * uninitialised memory round for as long as the track ran. */
    memset(mixring, 0, sizeof(mixring));
    while (mix_head < PLAY_MIXRING - PLAY_CHUNK && read_pos < data_end) {
        uint32_t idx = mix_head & PLAY_MIXRING_MASK;
        uint32_t want = PLAY_MIXRING - idx;
        uint32_t srcwant;
        int got;
        if (want > PLAY_CHUNK) want = PLAY_CHUNK;
        want -= (want % (uint32_t)mix_ring_per);
        srcwant = (want / (uint32_t)mix_ring_per) * (uint32_t)mix_src_per;
        if (srcwant > data_end - read_pos) {
            srcwant = data_end - read_pos;
            srcwant -= (srcwant % (uint32_t)mix_src_per);
            want = (srcwant / (uint32_t)mix_src_per)
                * (uint32_t)mix_ring_per;
        }
        if (!want || !srcwant) break;
        got = fs_read_chunk(fh, ring, (int)srcwant);
        if (got <= 0) break;
        got -= (got % mix_src_per);
        if (got <= 0) break;
        mixer_conv(mixring + idx, ring,
            ((uint32_t)got / (uint32_t)mix_src_per)
                * (uint32_t)mix_ring_per);
        mix_head += ((uint32_t)got / (uint32_t)mix_src_per)
            * (uint32_t)mix_ring_per;
        read_pos += (uint32_t)got;
    }

    if (dec.channels == 2) {
        /* CH_STEP is in BYTES per output frame, so an interleaved
         * stereo stream advances channels * bytes-per-sample of them
         * per source frame: 2 for 8-bit, 4 for 16-bit. */
        step = ((2u * (uint32_t)mix_bps * dec.rate) << Z_AUDIO_STEP_FRAC)
            / out_hz;
        for (c = 0; c < 2; c++) {
            /* Channel 1 starts one SAMPLE in, which is one byte at
             * 8-bit and two at 16-bit -- and offsetting BASE is the
             * only way to do it, because CH_CTRL's TRIG offset field
             * is in units of 256 bytes. */
            Z_AUDIO_CH_BASE(c) = base + (uint32_t)c * (uint32_t)mix_bps;
            Z_AUDIO_CH_LEN(c) = PLAY_MIXRING;
            Z_AUDIO_CH_LOOPST(c) = 0;
            Z_AUDIO_CH_LOOPLEN(c) = PLAY_MIXRING;
            Z_AUDIO_CH_STEP(c) = step;
        }
        /* Hard-panned: channel 0 is the left sample, channel 1 the
         * right. Summing both into both sides would be a mono
         * downmix at double level, which is exactly what the first
         * version of this did. */
        Z_AUDIO_CH_CTRL(0) =
            z_audio_ch_ctrl_fmt(g, 0, true, true, 0, mix_fmt16);
        Z_AUDIO_CH_CTRL(1) =
            z_audio_ch_ctrl_fmt(0, g, true, true, 0, mix_fmt16);
    } else {
        /* Mono: ONE channel feeding both sides. Two channels reading
         * the same byte would sum to double amplitude, and halving the
         * gain to compensate throws away a bit of resolution for
         * nothing. */
        step = (((uint32_t)mix_bps * dec.rate) << Z_AUDIO_STEP_FRAC)
            / out_hz;
        Z_AUDIO_CH_BASE(0) = base;
        Z_AUDIO_CH_LEN(0) = PLAY_MIXRING;
        Z_AUDIO_CH_LOOPST(0) = 0;
        Z_AUDIO_CH_LOOPLEN(0) = PLAY_MIXRING;
        Z_AUDIO_CH_STEP(0) = step;
        Z_AUDIO_CH_CTRL(0) =
            z_audio_ch_ctrl_fmt(g, g, true, true, 0, mix_fmt16);
    }

    z_audio_mixer_enable(true);
}

static void mixer_set_gain(void) {
    uint8_t g = (volume >= 64) ? 255 : (uint8_t)(volume * 4);
    /* EN without TRIG changes gain WITHOUT restarting the sample --
     * which for a ring buffer would mean jumping the read position
     * back to 0 mid-stream. zaudio.h calls this distinction out and
     * this is the call site that most needs it. */
    if (dec.channels == 2) {
        Z_AUDIO_CH_CTRL(0) =
            z_audio_ch_ctrl_fmt(g, 0, true, false, 0, mix_fmt16);
        Z_AUDIO_CH_CTRL(1) =
            z_audio_ch_ctrl_fmt(0, g, true, false, 0, mix_fmt16);
    } else {
        Z_AUDIO_CH_CTRL(0) =
            z_audio_ch_ctrl_fmt(g, g, true, false, 0, mix_fmt16);
    }
}

/* Source frames the mixer has actually played, for the time display. */
/* Source frames the mixer has played. The ring holds one BYTE per
 * sample whatever the file's depth was, so this counts ring bytes and
 * divides by the ring's sample width and the channel count. What the
 * SOURCE format was does not enter into it, which is exactly the point
 * of converting on the way in. */
static uint32_t mixer_played_src(void) {
    uint32_t consumed = mix_laps * PLAY_MIXRING + mix_lastpos;
    uint32_t f = mix_base_src
        + consumed / ((uint32_t)mix_bps * (uint32_t)dec.channels);
    /* Clamped, because the ring keeps circulating for a moment after
     * the last real sample and an elapsed time that walks past the
     * total is how this bug announced itself: "3:48 of 3:22". */
    uint32_t total = adec_total_frames(&dec);
    return (total && f > total) ? total : f;
}

/* ------------------------------------------------------------------
 * drawing
 *
 * Every lesson here is inherited from sw/apps/track's display, which
 * learned them the expensive way: text costs about 16ms of wall time
 * per row once the CPU is shared three ways, a full repaint of five
 * rows is most of the FIFO's entire margin, and doing it in one call
 * stalls the audio for exactly that long.
 *
 * So: at most ONE row is repainted per pass of the main loop, rows are
 * marked dirty rather than redrawn on a timer, and nothing that
 * changes continuously (the time, the buffer meter) is allowed to
 * change more than once a second.
 * ------------------------------------------------------------------ */

static void draw_row(int row, const char *s) {
    int y = 2 + row * ROW_H;
    z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
    z_win_draw_text(&win, 2, y, s, 1, &z_font_5x8);
}

static void fmt_time(char *out, int cap, uint32_t s) {
    snprintf(out, cap, "%lu:%02lu",
        (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void draw_name(void) {
    /* PLAY_PATH_MAX for the path, plus room for the "nn/nn " prefix.
     * Sized rather than trusted to snprintf's truncation: the row is
     * the only place the filename appears, and silently losing the
     * end of it is exactly the wrong thing to do to the one field
     * that says which file is playing. -Wformat-truncation flagged
     * the old b[64]. */
    char b[PLAY_PATH_MAX + 16];
    if (!have_file) {
        snprintf(b, sizeof(b), "%s", err_msg[0] ? err_msg : "no file (o to open)");
    } else if (nfiles) {
        snprintf(b, sizeof(b), "%d/%d %s", cur_file + 1, nfiles, cur_path);
    } else {
        snprintf(b, sizeof(b), "%s", cur_path);
    }
    draw_row(0, b);
}

static void draw_fmt(void) {
    char b[64];
    if (!have_file) { draw_row(1, ""); return; }
    snprintf(b, sizeof(b), "%s %dch %luHz -> %luHz %s",
        adec_codec_name(dec.codec), dec.channels,
        (unsigned long)dec.rate, (unsigned long)out_hz,
        /* 16b / 8b / 8b! -- the last meaning "this source had more
         * than eight bits and they were thrown away", which is worth
         * distinguishing from a source that only ever had eight. */
        mix_mode ? (mix_fmt16 ? "MIXER 16b"
                              : (mix_src_per == 2 ? "MIXER 8b!" : "MIXER 8b"))
                 : (mix_reject[0] ? "SOFT" : (interp ? "lerp" : "near")));
    draw_row(1, b);
}

static void draw_time(void) {
    char b[64], a[16], t[16];
    uint32_t total = adec_total_frames(&dec);
    /*
     * Both figures are converted to SECONDS before anything else
     * happens to them, and the reason is not tidiness.
     *
     * The total is in SOURCE frames and the elapsed count is in OUTPUT
     * frames, so they cannot be compared or divided against each other
     * directly -- a 22kHz file on a 44.1kHz DAC would report itself as
     * twice its real length, which reads as a decoder bug rather than
     * as a label bug. Rescaling frames to frames is the obvious fix
     * and it OVERFLOWS: a five-minute file is 13 million frames, and
     * 13e6 * 48000 does not fit in 32 bits. Seconds fit in anything.
     *
     * The alternative is 64-bit arithmetic, which on this core means a
     * libgcc call -- see track.c's engine notes on why this tree
     * avoids those.
     */
    /* In mixer mode the CPU never sees an output frame, so elapsed
     * time comes from where the MIXER has got to. Two sources for one
     * number, but they measure different things and the alternative --
     * a counter the CPU keeps -- would be an estimate of what the
     * hardware is doing rather than a reading of it. */
    uint32_t el_s = mix_mode
        ? (dec.rate ? mixer_played_src() / dec.rate : 0)
        : (out_hz ? played_frames / out_hz : 0);
    uint32_t tot_s = dec.rate ? total / dec.rate : 0;

    fmt_time(a, sizeof(a), el_s);
    if (tot_s) fmt_time(t, sizeof(t), tot_s);
    else strcpy(t, "--:--");

    snprintf(b, sizeof(b), "%s / %s  vol %d%%  %s", a, t,
        (volume * 100) / ADEC_GAIN_MAX,
        state == ST_PLAYING ? "PLAYING"
            : (state == ST_PAUSED ? "PAUSED" : "STOPPED"));
    draw_row(2, b);
}

/*
 * The line this phase exists for.
 *
 *   buf   ring occupancy. Should sit high and steady. Sagging means
 *         the card cannot keep ahead of the DAC.
 *   sv    stream starvations: the renderer had room in the FIFO and
 *         nothing to put there. This is the SD side failing.
 *   ur    the hardware's sticky UNDERRUN bit: the FIFO itself ran dry.
 *         Can happen with a healthy ring if something blocked the loop
 *         for longer than 23ms -- a redraw, or another process.
 *
 * The two failures are separate on purpose. They sound identical and
 * want opposite fixes: sv wants a smaller stream or a bigger ring, ur
 * wants less work in the loop. docs/audio.md makes the same
 * distinction for the mixer, for the same reason.
 */
static void draw_buf(void) {
    char b[64];
    uint32_t fill = !have_file ? 0
        : (mix_mode
            ? ((mix_head - (mix_laps * PLAY_MIXRING + mix_lastpos))
                * 100u) / PLAY_MIXRING
            : (stream_avail(&st) * 100u) / PLAY_RING_SIZE);
    /*
     * `ur` is the FIFO's sticky UNDERRUN bit and means nothing on the
     * mixer path -- the mixer cannot underrun, it replays the ring.
     * Showing a permanently blank field there would imply the check
     * was being made and passing, so the field is simply absent and
     * `sv`, which mixer_fill() computes itself, carries the whole
     * story.
     */
    if (mix_mode)
        snprintf(b, sizeof(b), "buf %2lu%%  sv%lu  sd %luK/s",
            (unsigned long)fill, (unsigned long)mix_starved,
            (unsigned long)(sd_rate / 1024));
    else
        snprintf(b, sizeof(b), "buf %2lu%%  sv%lu  %s  sd %luK/s",
            (unsigned long)fill, (unsigned long)last_starved,
            saw_underrun ? "ur" : "  ",
            (unsigned long)(sd_rate / 1024));
    draw_row(3, b);
}

/*
 * The bottom row: help pointer, transient note, or why the slow path
 * is in use.
 *
 * This row used to carry fd/pm/ms/dr -- per-phase worst-case wall
 * times in kernel ticks. They were left behind when the rdcycle
 * profiler replaced the z_uptime_ticks() instrumentation, and had been
 * reading a constant 0000 ever since: the variables were still
 * declared and still cleared once a second, and nothing wrote them.
 * A status line showing four zeros forever is worse than one showing
 * nothing, because it looks like a measurement.
 *
 * Timings now live in PLAY_PROF=1, where they are cycle-accurate and
 * go to the console. This row is for the user.
 */
static void draw_diag(void) {

    char b[80];

    if (mix_note[0]) {
        snprintf(b, sizeof(b), "%s", mix_note);
    } else if (!mix_mode && mix_reject[0]) {
        /* The single most useful thing this window can say when
         * playback is slow, and for three rounds of hardware
         * profiling it did not say it. */
        snprintf(b, sizeof(b), "SOFT: %s", mix_reject);
    } else {
        snprintf(b, sizeof(b), "h = help");
    }

    draw_row(4, b);
}

static void draw_help_row(int i) {
    char b[80];
    int y = 2 + i * ROW_H;
    if (i >= HELP_ROWS) return;
    snprintf(b, sizeof(b), "%-8s %s", help_keys[i].key, help_keys[i].what);
    z_win_fill_rect(&win, 0, y, z_win_content_w(&win), ROW_H, 0);
    z_win_draw_text(&win, 2, y, b, 1, &z_font_5x8);
}

/* One item per pass. A no-op once everything is clean. */
static void ui_step(void) {

    /* While help is up it owns the content area, so nothing else is
     * painted -- including the scope, which would otherwise sweep a
     * line through the middle of the text. */
    if (show_help) {
        if (help_row < HELP_ROWS) draw_help_row(help_row++);
        return;
    }

    if (dirty & DIRTY_NAME) { dirty &= ~DIRTY_NAME; draw_name(); }
    else if (dirty & DIRTY_FMT) { dirty &= ~DIRTY_FMT; draw_fmt(); }
    else if (dirty & DIRTY_TIME) { dirty &= ~DIRTY_TIME; draw_time(); }
    else if (dirty & DIRTY_BUF) { dirty &= ~DIRTY_BUF; draw_buf(); }
    else if (dirty & DIRTY_DIAG) { dirty &= ~DIRTY_DIAG; draw_diag(); }
}

/*
 * The scope, one column per pass.
 *
 * A sweep rather than a scroll: the column at scope_x is erased and
 * redrawn, then scope_x advances and the column ahead of it is left
 * blank as a cursor. Two blitter fills per pass and no reading back of
 * pixels.
 *
 * Scrolling the strip instead would mean moving the whole rectangle
 * every column. z_fb_hw_scroll() only moves vertically, so that would
 * be a per-pixel software loop over the full width -- hundreds of
 * times more work than this, for an aesthetic difference.
 *
 * The peak-to-peak envelope is drawn, not the waveform: at one column
 * per pass each column already covers thousands of frames, so there is
 * no waveform left to draw. An envelope is honest about that.
 */
static void scope_step(void) {

    int w = z_win_content_w(&win);
    int mid, hi, lo, top, hgt;

    if (show_help) return;
    if (!show_scope || scope_h < 4) return;
    if (!scope_have) return;

    if (scope_x >= w) scope_x = 0;

    mid = scope_y + scope_h / 2;
    hi = mid - (scope_max * (scope_h / 2)) / 32768;
    lo = mid - (scope_min * (scope_h / 2)) / 32768;
    if (hi > lo) { int t = hi; hi = lo; lo = t; }
    top = hi;
    hgt = lo - hi + 1;

    z_win_fill_rect(&win, scope_x, scope_y, 1, scope_h, 0);
    z_win_fill_rect(&win, scope_x, top, 1, hgt, 1);

    scope_x++;
    if (scope_x >= w) scope_x = 0;
    /* blank cursor column, so the sweep position is visible */
    z_win_fill_rect(&win, scope_x, scope_y, 1, scope_h, 0);

    scope_have = false;
}

static void layout(void) {
    int h = z_win_content_h(&win);
    int w = z_win_content_w(&win);

    scope_y = 2 + ROWS * ROW_H + 2;
    scope_h = h - scope_y - 22;
    if (scope_h < 0) scope_h = 0;

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
    dirty = DIRTY_NAME | DIRTY_FMT | DIRTY_TIME | DIRTY_BUF | DIRTY_DIAG;
    scope_x = 0;
    scope_have = false;
    z_widget_invalidate(&wset);
    z_widget_draw_all(&wset, true);
}

/* ------------------------------------------------------------------
 * transport
 * ------------------------------------------------------------------ */

/*
 * The transport button shows what pressing it will DO, not what the
 * player is currently doing: a triangle while stopped or paused
 * (press to start), two bars while playing (press to pause). That is
 * the universal convention and it is what this always computed.
 *
 * It nonetheless appeared inverted on hardware, and the reason was
 * not the logic -- it was that NOTHING REPAINTED THE BUTTON. Setting
 * z_widget_t.dirty only marks it; z_widget_draw_all() is what
 * actually draws, and the only call to it was inside draw_all(), i.e.
 * on a full Z_WM_REDRAW. So the button kept whatever face it had at
 * the last full repaint and lagged the state by one transition --
 * which looks exactly like inverted logic and is not.
 *
 * Drawing here rather than adding another arm to ui_step()'s
 * one-item-per-pass chain: a transport change is a user action that
 * happens a few times a minute, and one 20x18 button is far cheaper
 * than the text row ui_step() already draws in a single pass. Making
 * it wait for a turn would put a visible lag on the one control the
 * user just pressed.
 */
static void set_play_icon(void) {
    widgets[BTN_PLAY].icon = (state == ST_PLAYING)
        ? play_icon_pause : play_icon_play;
    widgets[BTN_PLAY].dirty = true;
    if (windowed && win.id >= 0) z_widget_draw_all(&wset, false);
}

static void do_playpause(void) {
    if (!have_file) {
        if (nfiles) next_track(cur_file < 0 ? 1 : 0);
        else return;
    }
    if (state == ST_PLAYING) {
        state = ST_PAUSED;
        /* EN=0 mutes without draining, so the FIFO's contents survive
         * a pause and resume is instant. rtl/audio.v was changed
         * specifically so this works; see docs/audio.md.
         *
         * In mixer mode it also stops the DAC reading anything, but
         * the mixer's own channels keep advancing -- so a long pause
         * silently walks the ring forward. Disabling MIXEN parks it
         * instead, and mixer_start() on resume is what makes that
         * exact rather than approximately right. */
        audio_enable(false);
        if (mix_mode) z_audio_mixer_enable(false);
    } else {
        state = ST_PLAYING;
        if (mix_mode) mixer_start();
        audio_enable(true);
    }
    set_play_icon();
    dirty |= DIRTY_TIME;
}

static void do_stop(void) {
    if (!have_file) return;
    state = ST_STOPPED;
    audio_enable(false);
    seek_to_frame(0);
    audio_enable(true);
    set_play_icon();
    dirty |= DIRTY_TIME | DIRTY_BUF;
}

static void do_seek_rel(int seconds) {
    int32_t s;
    if (!have_file || !out_hz || !dec.rate) return;
    /* Via seconds, for the same two reasons draw_time() gives: the two
     * frame counts are in different units, and rescaling one to the
     * other overflows 32 bits on any file of real length. */
    /* Which clock says "now" depends on the path: the mixer's own
     * position, or the frames this process pushed. Both via SECONDS,
     * because the two frame counts are in different units and
     * rescaling one to the other overflows 32 bits on any file of real
     * length. */
    s = (int32_t)(mix_mode ? (mixer_played_src() / dec.rate)
                           : (played_frames / out_hz)) + seconds;
    if (s < 0) s = 0;
    seek_to_frame((uint32_t)s * dec.rate);
}

static void set_volume(int v) {
    if (v < 0) v = 0;
    if (v > ADEC_GAIN_MAX) v = ADEC_GAIN_MAX;
    volume = v;
    if (have_file) adec_set_gain(&dec, volume);
    if (mix_mode) mixer_set_gain();
    widgets[WID_VOL].value = (int16_t)volume;
    widgets[WID_VOL].dirty = true;
    /* Same reasoning as set_play_icon(): a slider that repaints only
     * on the next full redraw reads as a broken control. */
    if (windowed && win.id >= 0) z_widget_draw_all(&wset, false);
    dirty |= DIRTY_TIME;
}

/* ------------------------------------------------------------------
 * input
 * ------------------------------------------------------------------ */

static void on_dialog_msg(z_msg_t *msg, void *user);

/*
 * Open a file through the standard dialog.
 *
 * PLAYBACK STOPS FOR THE DURATION, and that is a real limitation
 * rather than an oversight worth hiding. z_dialog_open() runs its own
 * message loop until the user chooses (sw/common/zdialog.c), and that
 * loop offers no hook that runs on every pass -- only a callback for
 * messages it does not handle itself. With the FIFO holding 23ms,
 * anything longer than a moment underruns continuously, which is a
 * far worse noise than a clean pause.
 *
 * The fix is small and belongs in zdialog rather than here: an
 * on_idle callback in z_dialog_ctx_t, called once per dlg_run()
 * iteration, would let this app keep feeding. Deliberately not done in
 * this phase, which is scoped to sw/apps/play. See docs/play_app.md.
 */
static void do_open(void) {

    z_dialog_ctx_t ctx;
    char path[PLAY_PATH_MAX];
    int prev = state;

    if (!windowed) return;

    if (state == ST_PLAYING) {
        state = ST_PAUSED;
        audio_enable(false);
        set_play_icon();
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.parent = &win;
    ctx.on_msg = on_dialog_msg;

    if (z_dialog_open(&ctx, "/AUDIO", path, sizeof(path))) {
        int i;
        /* If the chosen file is in the scanned list, adopt its index
         * so n/p continue to work from there. */
        cur_file = -1;
        for (i = 0; i < nfiles; i++)
            if (!strcmp(files[i], path)) { cur_file = i; break; }
        audio_enable(true);
        if (!open_track(path)) state = ST_STOPPED;
    } else {
        if (prev == ST_PLAYING) {
            state = ST_PLAYING;
            audio_enable(true);
        }
    }

    set_play_icon();
    draw_all();
}

static void handle_key(uint32_t ks) {

    /* Any key dismisses help, and the key that dismissed it is NOT
     * then acted on. Pressing something to make a panel go away and
     * having it also do the thing is the wrong kind of surprise. */
    if (show_help) {
        show_help = false;
        draw_all();
        return;
    }

    switch (ks) {

        case 'h':
        case '?':
            show_help = true;
            help_row = 0;
            z_win_clear(&win);
            z_widget_invalidate(&wset);
            break;
        case ' ':  do_playpause(); break;
        case 's':  do_stop(); break;
        case 'o':  do_open(); break;
        case 'n':  next_track(1); set_play_icon(); break;
        case 'p':  next_track(-1); set_play_icon(); break;
        case '-':  set_volume(volume - 4); break;
        case '=':
        case '+':  set_volume(volume + 4); break;
        case ',':  do_seek_rel(-5); break;
        case '.':  do_seek_rel(5); break;
        case 'w':
            show_scope = !show_scope;
            if (windowed && scope_h > 0)
                z_win_fill_rect(&win, 0, scope_y,
                    z_win_content_w(&win), scope_h, 0);
            scope_x = 0;
            scope_have = false;
            break;
        /* Force the FIFO path on a file the mixer could have taken, so
         * the two can be compared on the same material in the same
         * session. Without it every comparison is between two runs
         * and half the variables move. */
        case 'm':
            if (have_file) {
                if (mix_mode) { mixer_stop(); mix_mode = false;
                    seek_to_frame(0); }
                else if (mixer_supported()) { mix_mode = true;
                    seek_to_frame(0); }
                dirty |= DIRTY_NAME | DIRTY_FMT | DIRTY_BUF | DIRTY_DIAG;
            }
            break;
        case 'i':
            interp = !interp;
            stream_set_interp(&st, interp);
            dirty |= DIRTY_FMT;
            break;
        case 'q':
        case 27:   want_quit = true; break;
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
            /* Z_WM_KEY is a PACKED word, not a bare keysym, and
             * carries the press/release edge. Acting on both edges
             * makes every toggle fire twice -- space would pause and
             * instantly unpause. track.c documents the same trap. */
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

        default:
            break;
    }
}

/* Messages arriving while the file dialog owns the loop. Servicing
 * Z_WM_REDRAW here is not optional -- wm blocks on the ack, and a
 * dropped redraw stalls the whole desktop until its timeout. */
static void on_dialog_msg(z_msg_t *msg, void *user) {
    bool redraw = false;
    (void)user;
    dispatch(msg, &redraw);
    if (redraw) {
        draw_all();
        z_win_redraw_done(&win);
    }
}

static void drain_messages(void) {

    z_msg_t msg;
    bool redraw = false;

    while (z_msg_read(&msg) == Z_OK) {
        if (msg.subject == Z_WM_CLOSE) { want_quit = true; continue; }
        dispatch(&msg, &redraw);
        if (want_quit) return;
    }

    if (redraw) {
        draw_all();
        z_win_redraw_done(&win);
    }
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
    widgets[BTN_PLAY].icon = play_icon_play;

    widgets[BTN_STOP].type = Z_WIDGET_BUTTON;
    widgets[BTN_STOP].w = 20;
    widgets[BTN_STOP].h = 18;
    widgets[BTN_STOP].enabled = true;
    widgets[BTN_STOP].icon = play_icon_stop;

    widgets[WID_VOL].type = Z_WIDGET_SLIDER;
    widgets[WID_VOL].h = 12;
    widgets[WID_VOL].enabled = true;
    widgets[WID_VOL].vmin = 0;
    widgets[WID_VOL].vmax = ADEC_GAIN_MAX;
    widgets[WID_VOL].value = (int16_t)volume;

    z_widget_set_init(&wset, widgets, WIDGET_COUNT, &win);
}

int main(void) {

    char arg[PLAY_PATH_MAX];
    uint32_t last_tick;

    printf("\nplay -- streaming audio player\n");

    /* Feature bit FIRST, then MAGIC. On a bitstream built before
     * rtl/audio.v existed, 0x7000_05xx is decoded by nothing and an
     * undecoded address on this bus never acks -- probing it hangs the
     * CPU outright. sw/common/zaudio.h explains at length; this is the
     * safe order and z_audio_present() is what does it. */
    if (!z_audio_present()) {
        printf("This bitstream has no audio block.\n");
        printf("Rebuild with `AUDIO in rtl/boards.vh for this board.\n");
        return 1;
    }

    /* Start from the rate the BOARD came up with rather than a
     * constant. `AUDIO_RATE_RESET exists so a board can pick a rate
     * its outputs can actually carry -- 46875Hz on an S/PDIF board,
     * where anything below 32kHz will not lock at the receiver -- and
     * an app that overwrites it throws that away. Everything is
     * resampled to whatever this turns out to be, so no file cares. */
    rate_div = reg_audio_rate & 0xFF;
    if (!z_audio_rate_ok(rate_div)) rate_div = Z_AUDIO_RATE_44K;

    z_audio_start(rate_div);
    audio_claim();
    out_hz = z_audio_rate_hz();

    printf("play: output %luHz, mixer %s\n", (unsigned long)out_hz,
        mix_avail ? "present" : "ABSENT (FIFO path only)");

    /* Launch argument, if wm passed one. Taken EARLY, before the
     * window exists: it blocks on wm's reply through z_msg_wait(),
     * which discards anything else that arrives meanwhile, and at
     * startup nothing else is in flight. Later it would not be safe.
     * See zwin.h. */
    arg[0] = 0;
    z_launch_arg_take(arg, sizeof(arg));

    scan_dir("/AUDIO");
    if (!nfiles) scan_dir("/audio");
    if (!nfiles) scan_dir("/");

    printf("play: %d playable file%s\n", nfiles, nfiles == 1 ? "" : "s");

    init_widgets();

    /* Windowed if there is a wm to talk to, console if not.
     * z_win_create_flags() fails rather than hanging when nothing is
     * listening, so this doubles as the probe. The console path is not
     * nostalgia -- it is the mode to use when something is wrong,
     * because it depends on nothing but the audio block, the
     * filesystem and stdout. */
    /*
     * Note what is NOT set here: Z_WIN_FLAG_CLOSE_KILLS_OWNER.
     *
     * track sets it, and track is right to -- it holds nothing that
     * needs releasing. This app holds an open filesystem handle for
     * the whole length of a track, and zfs.h is explicit that a handle
     * belonging to a process that dies without closing it is NEVER
     * released: there is no process-exit hook that sweeps them. There
     * are four in the entire system.
     *
     * So the close icon must send Z_WM_CLOSE and let this app unwind
     * through close_track(), rather than removing the process from
     * under it. Killing it from the shell still leaks the handle and
     * there is nothing this app can do about that; see
     * docs/play_app.md.
     */
    windowed = (z_win_create_flags(&win, "play", WIN_W, WIN_H, -1, -1,
        Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_OPEN_ICON) == Z_OK);

    if (!windowed)
        printf("play: no wm; console mode\n");
    printf("play: h for help\n");

    if (arg[0]) {
        if (!open_track(arg)) printf("play: %s: %s\n", arg, err_msg);
    } else if (nfiles) {
        cur_file = 0;
        if (!open_track(files[0])) printf("play: %s\n", err_msg);
    }
    set_play_icon();

    if (windowed) draw_all();

    rr_start_tick = z_uptime_ticks();
    last_tick = rr_start_tick;

    while (!want_quit) {

        uint32_t now;

        PROF_PASS();

        /* -- audio first, always --
         *
         * The z_uptime_ticks() brackets that used to be here are gone.
         * They were a syscall through the reg_kernel trampoline with
         * 1.37ms resolution, called nine times a pass, measuring
         * phases far shorter than one tick -- see prof.h. Everything
         * is now bracketed by rdcycle instead, which is one
         * instruction and costs no gateware. */
        if (mix_mode) {
            /* One call. No render, no push, no interleaved top-up --
             * the mixer is reading the ring on its own clock and does
             * not care when this process next gets scheduled. That is
             * the whole point of the path. */
            if (state == ST_PLAYING) mixer_fill();
        } else {
            feed_fifo();
            pump_ring();
            /* Again, immediately after the only blocking call in the
             * loop -- which is exactly when the FIFO is emptiest. */
            feed_fifo();
        }

        if (z_audio_underrun()) {
            saw_underrun = true;
            z_audio_clear_underrun();
        }

        /* -- end of track -- */
        /* Done when the file has been read AND the mixer has caught up
         * with everything written. mixer_used() is signed precisely so
         * "caught up and overshot" reads as <= 0 rather than as a
         * number near 2^32. */
        if (have_file && state == ST_PLAYING && mix_mode
            && read_pos >= data_end && mixer_used() <= 0) {
            if (nfiles) { next_track(1); set_play_icon(); }
            else { state = ST_STOPPED; set_play_icon(); dirty |= DIRTY_TIME; }
        } else if (have_file && state == ST_PLAYING && !mix_mode
            && stream_drained(&st)
            && z_audio_level() == 0) {
            if (nfiles) { next_track(1); set_play_icon(); }
            else { state = ST_STOPPED; set_play_icon(); dirty |= DIRTY_TIME; }
        }

        /* -- ui -- */
        if (windowed) {
            { PROF_BEGIN(P_MSG);   drain_messages(); PROF_END(P_MSG); }
            { PROF_BEGIN(P_TEXT);  ui_step();        PROF_END(P_TEXT); }
            { PROF_BEGIN(P_SCOPE); scope_step();     PROF_END(P_SCOPE); }
        } else {
            int32_t ev = hid_read_key();
            if (ev >= 0)
                handle_key(z_kbd_usage_to_keysym((uint8_t)(ev & 0xFF),
                    (uint8_t)((ev >> 8) & 0xFF)));
        }

        /* -- once a second: recompute the measurements -- */
        now = z_uptime_ticks();
        if (now - rr_start_tick >= 732) {
            uint32_t el = now - rr_start_tick;
            /* bytes/s, staying inside 32 bits: sd_bytes over one
             * second is at most a few hundred KB and 732 times that is
             * comfortably under 2^32. */
            sd_rate = el ? ((sd_bytes / el) * 732) : 0;
            sd_bytes = 0;
            last_starved = have_file ? st.starved : 0;
            rr_start_tick = now;
            dirty |= DIRTY_TIME | DIRTY_BUF | DIRTY_DIAG;
            /* Notes are transient by design: a message that stays up
             * stops being about the key that was just pressed. */
            if (mix_note[0]) mix_note = "";
            if (!windowed)
                printf("\r  %s buf %2lu%% sv%lu %s sd %luK/s   ",
                    state == ST_PLAYING ? "play" : "stop",
                    (unsigned long)(have_file
                        ? (stream_avail(&st) * 100u) / PLAY_RING_SIZE : 0),
                    (unsigned long)last_starved,
                    saw_underrun ? "UNDER" : "     ",
                    (unsigned long)(sd_rate / 1024));
            saw_underrun = false;

            /* Measured honestly: the report is itself a phase, because
             * printf on this libc is not cheap and pretending
             * otherwise would hide the instrument's own cost inside
             * the loop overhead it is trying to account for. */
            /*
             * The report is expensive -- measured at ~14.5M cycles on
             * hardware, a third of a second of wall time, because this
             * libc's formatted output is not cheap and there are nine
             * calls of it. At one second it was consuming most of a
             * timeslice's worth of the very budget it reports on.
             *
             * So it runs every PLAY_PROF_SECS seconds, not every one,
             * and it is still measured as its own phase rather than
             * hidden. An instrument that distorts the measurement is
             * tolerable only if it says by how much.
             */
            prof_el += el;
            prof_secs++;
            if (prof_secs >= PLAY_PROF_SECS) {
                PROF_BEGIN(P_DUMP);
                prof_report(prof_el, out_hz);
                PROF_END(P_DUMP);
                prof_secs = 0;
                prof_el = 0;
            }
        }

        /*
         * Yield -- but only when there is slack.
         *
         * track.c sleeps a tick every pass because a tracker's real
         * work is 50 register writes a second and everything else is
         * waiting. This one has continuous work, and sleeping while
         * the ring is draining is how a player ends up starved on a
         * machine that had the cycles all along.
         *
         * So: give the tick back when both buffers are comfortable,
         * and take the whole slice when they are not. wm and sh both
         * busy-poll anyway (docs/audio.md), so this is no ruder than
         * its neighbours -- and it is quiet the moment the stream is
         * keeping up, which is most of the time.
         */
        if (state != ST_PLAYING
            || (mix_mode
                && (mix_head - (mix_laps * PLAY_MIXRING + mix_lastpos))
                   > PLAY_MIXRING / 2)
            || (!mix_mode && have_file
                && stream_avail(&st) > PLAY_RING_SIZE / 2
                && z_audio_level() > 512)) {
            PROF_BEGIN(P_WAIT);
            z_proc_wait(1);
            PROF_END(P_WAIT);
        }

        (void)last_tick;
    }

    close_track();
    z_audio_stop();

    if (windowed) z_win_destroy(&win);
    else printf("\n");

    printf("play: done.\n");
    return 0;
}
