/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * smf -- see smf.h.
 */

#include <string.h>

#include "smf.h"

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t rd16be(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

/*
 * MIDI's variable-length quantity: seven bits per byte, high bit set
 * on every byte but the last.
 *
 * Bounded at five bytes. The format allows four, but a corrupt file
 * with the high bit set in every byte would otherwise walk the cursor
 * off the end of the buffer looking for a terminator that never
 * comes -- and the end of the buffer is somebody else's .bss.
 */
static uint32_t rd_vlq(const uint8_t **p, const uint8_t *end) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < 5 && *p < end; i++) {
        uint8_t b = *(*p)++;
        v = (v << 7) | (uint32_t)(b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

/*
 * Read the delta time of a track's next event and set next_tick.
 *
 * Called once per event, immediately after consuming the previous
 * one, so `next_tick` is always the absolute tick of something not yet
 * played -- which is what lets smf_step() pick the earliest across all
 * tracks with a plain comparison and no lookahead.
 */
static void track_advance(smf_track_t *t, uint32_t from_tick) {
    if (t->p >= t->end) { t->done = true; return; }
    t->next_tick = from_tick + rd_vlq(&t->p, t->end);
}

int smf_open(smf_t *s, const uint8_t *buf, uint32_t len) {

    const uint8_t *p, *end;
    uint32_t hdrlen;
    int i;

    memset(s, 0, sizeof(*s));
    s->buf = buf;
    s->len = len;

    if (len < 14) return SMF_ERR_TRUNC;
    if (memcmp(buf, "MThd", 4) != 0) return SMF_ERR_MAGIC;

    hdrlen = rd32be(buf + 4);
    if (hdrlen < 6 || 8 + hdrlen > len) return SMF_ERR_TRUNC;

    s->format = (int)rd16be(buf + 8);
    s->ntracks = (int)rd16be(buf + 10);
    s->division = rd16be(buf + 12);

    /* Format 2's tracks are independent sequences, not parallel ones,
     * so "play the file" has no single meaning. Refused rather than
     * guessed at. */
    if (s->format != 0 && s->format != 1) return SMF_ERR_FORMAT;

    /* Bit 15 set means SMPTE frames rather than ticks per quarter
     * note -- a different clock entirely. Treating it as PPQN would
     * play at an arbitrary wrong tempo, which reads as a sequencer
     * bug rather than an unsupported mode. */
    if (s->division == 0 || (s->division & 0x8000)) return SMF_ERR_DIVISION;

    if (s->ntracks < 1) return SMF_ERR_TRUNC;
    if (s->ntracks > SMF_MAX_TRACKS) return SMF_ERR_TRACKS;

    /* Walk the chunk list. Anything that is not MTrk is SKIPPED rather
     * than treated as an error: the spec says unknown chunk types must
     * be ignored, and real files do carry them. */
    p = buf + 8 + hdrlen;
    end = buf + len;
    i = 0;

    while (p + 8 <= end && i < s->ntracks) {
        uint32_t clen = rd32be(p + 4);
        const uint8_t *body = p + 8;
        if (body + clen > end) {
            /* A truncated final track is common in files that were cut
             * short. Clamp to what is there and play it rather than
             * refusing the whole file. */
            clen = (uint32_t)(end - body);
        }
        if (memcmp(p, "MTrk", 4) == 0) {
            s->track[i].start = body;
            s->track[i].p = body;
            s->track[i].end = body + clen;
            s->track[i].running = 0;
            s->track[i].done = false;
            i++;
        }
        p = body + clen;
    }

    if (i == 0) return SMF_ERR_TRUNC;
    s->ntracks = i;

    smf_rewind(s);
    return SMF_OK;
}

void smf_rewind(smf_t *s) {

    int i;

    s->tick = 0;
    s->frac = 0;
    s->ended = false;
    /* 120 BPM, the value the spec says to assume when a file never
     * sets one. Plenty do not. */
    s->tempo_us = 500000;

    for (i = 0; i < s->ntracks; i++) {
        smf_track_t *t = &s->track[i];
        /* Each track records where its body began, because a cursor
         * cannot be rewound from itself and the header walk that
         * found it is not worth repeating. */
        t->p = t->start;
        t->running = 0;
        t->done = false;
        t->next_tick = 0;
        track_advance(t, 0);
    }
}

/*
 * Consume one event from track `ti`, translate it, and queue the
 * track's next delta.
 *
 * Returns an SMF_EV_* type; SMF_EV_NONE means "parsed and dropped"
 * (a controller nobody handles, a meta event other than tempo, a
 * sysex), which the caller retries rather than treating as end of
 * data.
 */
static int track_event(smf_t *s, int ti, smf_event_t *ev) {

    smf_track_t *t = &s->track[ti];
    uint8_t status;
    int type = SMF_EV_NONE;

    if (t->done || t->p >= t->end) { t->done = true; return SMF_EV_NONE; }

    status = *t->p;

    if (status & 0x80) {
        t->p++;
        /* Meta and sysex do NOT set running status; only channel
         * messages do. Letting 0xFF become the running status is a
         * classic way to turn the rest of a track into garbage. */
        if (status < 0xF0) t->running = status;
    } else {
        status = t->running;
        if (!status) { t->done = true; return SMF_EV_NONE; }
    }

    if (status == 0xFF) {
        /* meta */
        uint8_t meta = (t->p < t->end) ? *t->p++ : 0x2F;
        uint32_t mlen = rd_vlq(&t->p, t->end);
        if (meta == 0x51 && mlen >= 3 && t->p + 3 <= t->end) {
            s->tempo_us = ((uint32_t)t->p[0] << 16)
                | ((uint32_t)t->p[1] << 8) | (uint32_t)t->p[2];
            if (!s->tempo_us) s->tempo_us = 500000;
        }
        t->p += mlen;
        if (meta == 0x2F) { t->done = true; return SMF_EV_NONE; }

    } else if (status == 0xF0 || status == 0xF7) {
        uint32_t slen = rd_vlq(&t->p, t->end);
        t->p += slen;

    } else {

        uint8_t ch = status & 0x0f;
        uint8_t hi = status & 0xf0;
        uint8_t d1 = (t->p < t->end) ? *t->p++ : 0;
        uint8_t d2 = 0;

        if (hi != 0xC0 && hi != 0xD0)
            d2 = (t->p < t->end) ? *t->p++ : 0;

        ev->channel = ch;
        ev->a = d1 & 0x7f;
        ev->b = d2 & 0x7f;

        switch (hi) {
            case 0x90:
                /* Note-on with velocity 0 IS a note-off. Every file
                 * uses it, because it lets a run of notes share one
                 * running status byte. Missing it leaves every note
                 * sounding forever. */
                type = ev->b ? SMF_EV_NOTE_ON : SMF_EV_NOTE_OFF;
                break;
            case 0x80:
                type = SMF_EV_NOTE_OFF;
                break;
            case 0xC0:
                type = SMF_EV_PROGRAM;
                break;
            case 0xB0:
                switch (ev->a) {
                    case SMF_CC_VOLUME:
                    case SMF_CC_PAN:
                    case SMF_CC_EXPRESSION:
                    case SMF_CC_SUSTAIN:
                    case SMF_CC_ALL_OFF:
                    case SMF_CC_ALL_NOTES_OFF:
                        type = SMF_EV_CONTROL;
                        break;
                    default:
                        type = SMF_EV_NONE;
                        break;
                }
                break;
            case 0xE0:
                ev->bend = (int16_t)(((int)d2 << 7) | (int)d1) - 8192;
                type = SMF_EV_PITCH;
                break;
            default:
                type = SMF_EV_NONE;   /* aftertouch etc: parsed, dropped */
                break;
        }
    }

    track_advance(t, t->next_tick);
    return type;
}

int smf_step(smf_t *s, uint32_t us, smf_event_t *ev) {

    int i, best;

    memset(ev, 0, sizeof(*ev));

    /*
     * Advance the clock, if asked.
     *
     * ticks = us * division / tempo_us. Done in that order and in
     * 16.16 because the alternative -- us/tempo_us first -- is zero
     * for every call shorter than a quarter note, which is all of
     * them. The fraction carries between calls so a 1.37ms kernel
     * tick against a 2.08ms MIDI tick does not quietly round to
     * nothing.
     *
     * No 64-bit arithmetic: us is at most a few thousand per call and
     * division at most 32767, so the product stays well inside 32
     * bits. A caller that hands over a whole second at once would
     * overflow, which is why the app calls this every pass rather
     * than catching up in bulk.
     */
    if (us && !s->ended) {
        uint32_t adv = ((us << 8) / s->tempo_us) * s->division;
        adv += (((us << 8) % s->tempo_us) * s->division) / s->tempo_us;
        s->frac += adv;
        s->tick += s->frac >> 8;
        s->frac &= 0xff;
    }

    if (s->ended) { ev->type = SMF_EV_END; return SMF_EV_END; }

    /* Earliest pending event across all tracks. On a tie the
     * lowest-numbered track wins, which matches the order a format 0
     * file would have had them in. */
    for (;;) {

        best = -1;
        for (i = 0; i < s->ntracks; i++) {
            smf_track_t *t = &s->track[i];
            if (t->done) continue;
            if (t->next_tick > s->tick) continue;
            if (best < 0 || t->next_tick < s->track[best].next_tick)
                best = i;
        }

        if (best < 0) {
            /* Nothing due. If nothing is left anywhere, the file is
             * over -- but only then: a long rest is not an ending. */
            for (i = 0; i < s->ntracks; i++)
                if (!s->track[i].done) { ev->type = SMF_EV_NONE; return SMF_EV_NONE; }
            s->ended = true;
            ev->type = SMF_EV_END;
            return SMF_EV_END;
        }

        {
            int type = track_event(s, best, ev);
            if (type != SMF_EV_NONE) { ev->type = type; return type; }
            /* Parsed and dropped -- go round again rather than
             * returning, or a track full of unhandled controllers
             * would stall the caller's drain loop one event per
             * pass. */
        }
    }
}

void smf_scan_length(smf_t *s) {

    smf_t tmp = *s;
    smf_event_t ev;
    uint32_t guard = 0;

    smf_rewind(&tmp);

    /* Walk with the clock pinned at the furthest pending event rather
     * than stepping in time: a length scan that advanced in
     * microseconds would take as long as the song does. */
    for (;;) {
        int i, best = -1;
        for (i = 0; i < tmp.ntracks; i++) {
            if (tmp.track[i].done) continue;
            if (best < 0 || tmp.track[i].next_tick < tmp.track[best].next_tick)
                best = i;
        }
        if (best < 0) break;
        tmp.tick = tmp.track[best].next_tick;
        track_event(&tmp, best, &ev);
        if (++guard > 2000000u) break;   /* corrupt file insurance */
    }

    s->total_ticks = tmp.tick;
}

uint32_t smf_duration_ms(const smf_t *s) {
    if (!s->division) return 0;
    return (s->total_ticks / s->division) * (s->tempo_us / 1000);
}

uint32_t smf_elapsed_ms(const smf_t *s) {
    if (!s->division) return 0;
    return (s->tick / s->division) * (s->tempo_us / 1000);
}

const char *smf_strerror(int err) {
    switch (err) {
        case SMF_OK:            return "ok";
        case SMF_ERR_MAGIC:     return "not a MIDI file";
        case SMF_ERR_FORMAT:    return "SMF format 2 not supported";
        case SMF_ERR_TRACKS:    return "too many tracks";
        case SMF_ERR_DIVISION:  return "SMPTE timing not supported";
        case SMF_ERR_TRUNC:     return "truncated or malformed";
        default:                return "unknown error";
    }
}
