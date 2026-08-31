/*
 * modplay -- ProTracker MOD playback engine
 *
 * See modplay.h for the interface and for what is and is not
 * supported. This file explains the parts that are not obvious from
 * reading the code.
 *
 * -- FIXED POINT, AND WHY 14 BITS --
 *
 * Sample positions are 32-bit fixed point with 14 fractional bits.
 * That split is forced from both ends:
 *
 *   A ProTracker sample can be 65535 WORDS, i.e. 131070 bytes, so the
 *   integer part needs 18 bits. 131070 * 2^14 = 2147450880, which
 *   fits in a uint32 with room to spare; 2^16 would overflow on the
 *   largest legal sample and 2^15 leaves no headroom for the step
 *   added on top.
 *
 *   1/16384 of a sample is a pitch resolution of about 0.006%, well
 *   under a cent. Not the limit on quality here.
 *
 * -- NO 64-BIT ARITHMETIC ANYWHERE --
 *
 * This runs on a 32-bit core whose 64-bit divide is a libgcc call.
 * The step calculation is the one place that wants to overflow, and
 * it is written to avoid it rather than to rely on the compiler:
 *
 *   step = (7093789 * 2 / period) * 2^14 / rate
 *
 * done naively overflows at the multiply. Computing four times the
 * frequency first and then shifting by 12 keeps every intermediate
 * under 2^30: the largest is 14187579/113 = 125554, and 125554 << 12
 * is 514269184. Losing the sub-Hz part of hz4 costs about 0.1 cents
 * at the lowest note, which is inaudible and buys a routine with no
 * libgcc call in it at all.
 *
 * -- WHY THE FINETUNE TABLE IS A MULTIPLIER, NOT 16 PERIOD TABLES --
 *
 * ProTracker ships 16 tables of 36 periods. That is 1152 bytes of
 * const data for something that is, to within a period unit, a single
 * multiply: finetune f scales the period by 2^(-f/96). One 36-entry
 * table plus 16 multipliers is 104 bytes and sounds the same. On a
 * board with 1MB of main memory that trade is worth making, and the
 * error is far below the 0.006% the fixed point already imposes.
 *
 * -- MIXING HEADROOM --
 *
 * Eight channels at full volume sum to +/-65536, which does not fit in
 * an int16. Rather than clamping -- which is audible distortion
 * exactly when the music is loudest -- the accumulator is scaled by
 * `master`, set so that all channels at full volume lands at about
 * 75% of full scale. That is 768/channels, so a 4-channel module gets
 * 192 and an 8-channel one gets 96, and neither can clip on ordinary
 * material. The clamp is still there, because "cannot clip on
 * ordinary material" is not the same as cannot clip.
 */

#include "modplay.h"

#define MOD_FRAC_BITS 14
#define MOD_FRAC_ONE  (1u << MOD_FRAC_BITS)

/* PAL Amiga: period-to-rate is 7093789.2 / (period * 2). This is four
 * times that numerator, so the division result has two extra bits of
 * precision before the shift -- see this file's header. */
#define MOD_CLOCK4 14187579u

#define MOD_PERIOD_MIN 113
#define MOD_PERIOD_MAX 856

/* Finetune 0, notes C-1 through B-3. Every other finetune is this
 * table times a multiplier -- see the header. */
static const uint16_t mod_periods[36] = {
	856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
	428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
	214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113
};

/* 2^(-f/96) in 16.16, indexed by the RAW finetune nibble: 0..7 are
 * finetunes 0..+7, 8..15 are -8..-1. Sharper finetune means a shorter
 * period, so positive finetunes are below 65536. */
/* NOTE THE TYPE. These are 16.16 fixed point and the first entry is
 * exactly 65536, which does not fit in a uint16_t -- it wraps to 0,
 * and a multiplier of 0 turns every period into 0 for the commonest
 * finetune there is. That was a real bug here, caught by the host
 * pitch test (every note came out at the same clamped 980Hz) and it
 * would have been near-impossible to diagnose by ear. */
static const uint32_t mod_finetune[16] = {
	65536, 65065, 64596, 64132, 63670, 63212, 62757, 62306,  /* 0..+7 */
	69433, 68933, 68438, 67945, 67456, 66971, 66489, 66011   /* -8..-1 */
};

/* ProTracker's vibrato sine, one half cycle. Position runs 0..63 and
 * bit 5 supplies the sign. */
static const uint8_t mod_sine[32] = {
	  0,  24,  49,  74,  97, 120, 141, 161,
	180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197,
	180, 161, 141, 120,  97,  74,  49,  24
};

static void update_gains(mod_player_t *m);

static uint16_t rd16(const uint8_t *p) {
	return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t clamp_period(int32_t p) {
	if (p < MOD_PERIOD_MIN) return MOD_PERIOD_MIN;
	if (p > MOD_PERIOD_MAX) return MOD_PERIOD_MAX;
	return (int16_t)p;
}

static uint8_t clamp_vol(int32_t v) {
	if (v < 0) return 0;
	if (v > 64) return 64;
	return (uint8_t)v;
}

/*
 * Nearest note index for a period, used by arpeggio and by tone
 * portamento. Linear scan of 36 entries rather than a binary search:
 * it runs a handful of times per row, not per sample, and the table is
 * descending, which is the direction that makes a hand-written binary
 * search easy to get subtly wrong.
 */
static int period_to_note(int16_t period) {
	int i;
	for (i = 0; i < 36; i++)
		if (period >= (int16_t)mod_periods[i]) return i;
	return 35;
}

static uint16_t note_to_period(int note, uint8_t finetune) {
	uint32_t p;
	if (note < 0) note = 0;
	if (note > 35) note = 35;
	p = ((uint32_t)mod_periods[note] * mod_finetune[finetune & 15]) >> 16;
	return (uint16_t)p;
}

/* Set a channel's playback step from an EFFECTIVE period -- the base
 * period plus whatever arpeggio or vibrato is doing this tick. The
 * channel's own ch->period is left alone, which is what makes those
 * two effects non-destructive. */
static void set_step(mod_player_t *m, mod_channel_t *ch, int32_t period) {
	uint32_t hz4;

	if (period < MOD_PERIOD_MIN) period = MOD_PERIOD_MIN;
	if (period > MOD_PERIOD_MAX) period = MOD_PERIOD_MAX;

	hz4 = MOD_CLOCK4 / (uint32_t)period;
	ch->step = (hz4 << (MOD_FRAC_BITS - 2)) / m->rate;
}

static void set_tempo(mod_player_t *m) {
	/* A tick is 2.5/BPM seconds, so frames per tick is rate*2.5/BPM.
	 * Written as (rate*5)/(bpm*2) to stay in integers; rate*5 is at
	 * most 240000 for any rate this SOC can produce. */
	if (m->bpm < 32) m->bpm = 32;
	m->samples_per_tick = (m->rate * 5u) / ((uint32_t)m->bpm * 2u);
	if (m->samples_per_tick == 0) m->samples_per_tick = 1;
}

void modplay_retempo(mod_player_t *m) {
	int c;
	set_tempo(m);
	for (c = 0; c < m->channels; c++)
		if (m->ch[c].active) set_step(m, &m->ch[c], m->ch[c].period);
}

const char *modplay_strerror(int err) {
	switch (err) {
	case MOD_OK:             return "ok";
	case MOD_ERR_TOO_SMALL:  return "file is too small to be a module";
	case MOD_ERR_NOT_MOD:    return "no ProTracker signature (not a .mod?)";
	case MOD_ERR_CHANNELS:   return "too many channels for this player";
	case MOD_ERR_TRUNCATED:  return "file is truncated (header wants more)";
	default:                 return "unknown error";
	}
}

void modplay_set_separation(mod_player_t *m, int percent) {
	int c;

	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	m->separation = (percent * 256) / 100;

	for (c = 0; c < m->channels; c++) {
		/* Amiga order is L R R L, repeating.
		 *
		 * The near side stays at FULL gain and only the far side is
		 * attenuated. The obvious alternative -- 128 +/- half, summing
		 * to 256 -- costs 6dB in the centre, so a module at
		 * separation 0 came out half as loud as the same module hard
		 * panned, and no single master scale could be right for both.
		 * This way the worst case is the same either way: every
		 * channel at full volume on one side. */
		int left = ((c & 3) == 0 || (c & 3) == 3);
		m->ch[c].pan_l = (uint16_t)(left ? 256 : 256 - m->separation);
		m->ch[c].pan_r = (uint16_t)(left ? 256 - m->separation : 256);
	}

	/* Immediately, not at the next tick: a separation change the user
	 * can hear only after the next row would feel broken. */
	update_gains(m);

}

int modplay_init(mod_player_t *m, const void *file, uint32_t len,
	uint32_t rate)
{
	const uint8_t *f = (const uint8_t *)file;
	uint32_t off;
	int i, c, maxpat;

	for (i = 0; i < (int)sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;

	if (len < 1084) return MOD_ERR_TOO_SMALL;

	m->file = f;
	m->file_len = len;
	m->rate = rate ? rate : 22050;

	/* Channel count from the magic at 1080. Checked BEFORE anything
	 * else is trusted: every offset below depends on it, so a wrong
	 * guess here reads sample headers out of pattern data and produces
	 * noise rather than an error. */
	if (f[1080] == 'M' && f[1081] == '.' && f[1082] == 'K' && f[1083] == '.')
		m->channels = 4;
	else if (f[1080] == 'M' && f[1081] == '!' && f[1082] == 'K' && f[1083] == '!')
		m->channels = 4;
	else if (f[1080] == 'F' && f[1081] == 'L' && f[1082] == 'T' && f[1083] == '4')
		m->channels = 4;
	else if (f[1081] == 'C' && f[1082] == 'H' && f[1083] == 'N' &&
		f[1080] >= '1' && f[1080] <= '9')
		m->channels = f[1080] - '0';
	else
		return MOD_ERR_NOT_MOD;

	if (m->channels < 1 || m->channels > MOD_MAX_CHANNELS)
		return MOD_ERR_CHANNELS;

	for (i = 0; i < 20; i++) m->name[i] = (char)f[i];
	m->name[20] = 0;

	/* Sample headers: 30 bytes each from offset 20, 1-based. */
	for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
		const uint8_t *h = f + 20 + (i - 1) * 30;
		m->samples[i].length     = (uint32_t)rd16(h + 22) * 2u;
		m->samples[i].finetune   = h[24] & 0x0F;
		m->samples[i].volume     = h[25] > 64 ? 64 : h[25];
		m->samples[i].loop_start = (uint32_t)rd16(h + 26) * 2u;
		m->samples[i].loop_len   = (uint32_t)rd16(h + 28) * 2u;
	}

	m->song_len = f[950];
	if (m->song_len < 1 || m->song_len > 128) m->song_len = 128;

	maxpat = 0;
	for (i = 0; i < 128; i++) {
		m->order[i] = f[952 + i];
		if (m->order[i] > maxpat) maxpat = m->order[i];
	}
	m->num_patterns = maxpat + 1;

	m->patterns = f + 1084;
	m->pattern_bytes = 64u * (uint32_t)m->channels * 4u;

	off = 1084 + (uint32_t)m->num_patterns * m->pattern_bytes;
	if (off > len) return MOD_ERR_TRUNCATED;

	/* Sample data follows the patterns, in sample order.
	 *
	 * Clamped against the real file length rather than trusted. A
	 * truncated or padded module is common enough in the wild that
	 * refusing to play one would be worse than playing what is there,
	 * but reading past the buffer is not an option -- so a sample that
	 * runs off the end is shortened and one that starts off the end is
	 * dropped. */
	for (i = 1; i <= MOD_MAX_SAMPLES; i++) {
		mod_sample_t *s = &m->samples[i];
		if (off >= len) {
			s->length = 0;
			s->data = 0;
			continue;
		}
		if (off + s->length > len) s->length = len - off;
		s->data = (const int8_t *)(f + off);
		/* Byte offset within the file as well as a pointer: the
		 * hardware mixer needs an address, and only the caller knows
		 * where the file actually lives. */
		s->offset = off;
		off += s->length;

		if (s->loop_len <= 2 || s->loop_start >= s->length) {
			s->loop_len = 0;
		} else if (s->loop_start + s->loop_len > s->length) {
			s->loop_len = s->length - s->loop_start;
			if (s->loop_len <= 2) s->loop_len = 0;
		}
	}

	m->speed = 6;
	m->bpm = 125;
	set_tempo(m);

	m->tick = 0;
	m->row_ticks = m->speed;
	m->row = 0;
	m->order_pos = 0;
	m->tick_remaining = 0;
	m->break_row = -1;
	m->jump_order = -1;
	m->loops = 0;

	m->master = 768 / m->channels;

	for (c = 0; c < m->channels; c++) {
		m->ch[c].volume = 0;
		m->ch[c].period = MOD_PERIOD_MAX;
		m->ch[c].active = 0;
	}

	modplay_set_separation(m, 65);

	return MOD_OK;
}

static void trigger(mod_player_t *m, mod_channel_t *ch, uint32_t offset) {
	/* Recorded for the hardware path, which needs to know a note
	 * STARTED rather than inferring it from state that also changes
	 * for other reasons. Harmless and unused in software mode. */
	ch->hw_trigger = 1;
	ch->hw_offset = (uint8_t)(offset >> 8);
	mod_sample_t *s;

	if (ch->sample < 1 || ch->sample > MOD_MAX_SAMPLES) return;
	s = &m->samples[ch->sample];
	if (!s->data || s->length == 0) { ch->active = 0; return; }

	ch->data = s->data;
	ch->length = s->length;
	ch->loop_start = s->loop_start;
	ch->loop_len = s->loop_len;

	/* 9xx past the end of the sample: ProTracker plays silence rather
	 * than wrapping, and a lot of modules use a too-large offset
	 * deliberately to cut a sample short. */
	if (offset >= s->length) {
		ch->active = 0;
		return;
	}

	ch->pos = offset << MOD_FRAC_BITS;
	ch->active = 1;
	ch->vib_pos = 0;
}

static void row_tick0(mod_player_t *m) {
	const uint8_t *rowp;
	int c;

	rowp = m->patterns
		+ (uint32_t)m->order[m->order_pos] * m->pattern_bytes
		+ (uint32_t)m->row * (uint32_t)m->channels * 4u;

	for (c = 0; c < m->channels; c++) {
		mod_channel_t *ch = &m->ch[c];
		const uint8_t *n = rowp + c * 4;
		int sample = (n[0] & 0xF0) | (n[2] >> 4);
		int period = ((n[0] & 0x0F) << 8) | n[1];
		int effect = n[2] & 0x0F;
		int param = n[3];
		int porta = (effect == 3 || effect == 5);

		ch->effect = (uint8_t)effect;
		ch->param = (uint8_t)param;

		/* A sample number with no note reloads volume and finetune but
		 * does NOT restart the sample. Modules use this constantly to
		 * change volume mid-note; treating it as a retrigger is one of
		 * the classic ways a player ends up sounding "clicky". */
		if (sample >= 1 && sample <= MOD_MAX_SAMPLES) {
			ch->sample = (uint8_t)sample;
			ch->volume = m->samples[sample].volume;
			ch->finetune = m->samples[sample].finetune;
		}

		if (period) {
			int note = period_to_note((int16_t)period);
			uint16_t p = note_to_period(note, ch->finetune);

			if (porta) {
				/* Tone portamento: the note sets the TARGET and
				 * nothing restarts. */
				ch->porta_target = (int16_t)p;
			} else {
				ch->period = (int16_t)p;
				ch->porta_target = (int16_t)p;
				trigger(m, ch, (effect == 9)
					? ((uint32_t)param << 8) : 0);
			}
		}

		switch (effect) {
		case 0x3:
			if (param) ch->porta_speed = (uint8_t)param;
			break;
		case 0x4:
			if (param >> 4) ch->vib_speed = (uint8_t)(param >> 4);
			if (param & 0x0F) ch->vib_depth = (uint8_t)(param & 0x0F);
			break;
		case 0x9:
			if (param) ch->offset_mem = (uint8_t)param;
			break;
		case 0xB:
			m->jump_order = param;
			break;
		case 0xC:
			ch->volume = clamp_vol(param);
			break;
		case 0xD:
			m->break_row = (param >> 4) * 10 + (param & 0x0F);
			if (m->break_row > 63) m->break_row = 0;
			break;
		case 0xE:
			switch (param >> 4) {
			case 0x1:
				ch->period = clamp_period(ch->period - (param & 0x0F));
				break;
			case 0x2:
				ch->period = clamp_period(ch->period + (param & 0x0F));
				break;
			case 0x9:
				ch->retrig_mem = (uint8_t)(param & 0x0F);
				break;
			case 0xA:
				ch->volume = clamp_vol(ch->volume + (param & 0x0F));
				break;
			case 0xB:
				ch->volume = clamp_vol(ch->volume - (param & 0x0F));
				break;
			case 0xE:
				/* Pattern delay repeats the row for (x+1) times the
				 * normal tick count WITHOUT re-reading it -- which is
				 * why row_ticks is separate from speed. Retriggering
				 * the notes here would turn a delay into a stutter. */
				m->row_ticks = m->speed * (1 + (param & 0x0F));
				break;
			default:
				break;
			}
			break;
		case 0xF:
			if (param == 0) break;
			if (param < 32) {
				m->speed = param;
				m->row_ticks = m->speed;
			} else {
				m->bpm = param;
				set_tempo(m);
			}
			break;
		default:
			break;
		}

		if (ch->active) set_step(m, ch, ch->period);
	}
}

static void vol_slide(mod_channel_t *ch, int param) {
	int up = param >> 4;
	int down = param & 0x0F;
	/* Both nibbles set is ambiguous; ProTracker lets up win. */
	if (up) ch->volume = clamp_vol(ch->volume + up);
	else if (down) ch->volume = clamp_vol(ch->volume - down);
}

static void tone_porta(mod_player_t *m, mod_channel_t *ch) {
	if (ch->porta_target == 0) return;
	if (ch->period < ch->porta_target) {
		ch->period += ch->porta_speed;
		if (ch->period > ch->porta_target) ch->period = ch->porta_target;
	} else if (ch->period > ch->porta_target) {
		ch->period -= ch->porta_speed;
		if (ch->period < ch->porta_target) ch->period = ch->porta_target;
	}
	set_step(m, ch, ch->period);
}

static void do_vibrato(mod_player_t *m, mod_channel_t *ch) {
	int delta = (mod_sine[ch->vib_pos & 31] * ch->vib_depth) / 128;
	if (ch->vib_pos & 32) delta = -delta;
	set_step(m, ch, ch->period + delta);
	ch->vib_pos = (uint8_t)((ch->vib_pos + ch->vib_speed) & 63);
}

static void row_tickn(mod_player_t *m) {
	int c;

	for (c = 0; c < m->channels; c++) {
		mod_channel_t *ch = &m->ch[c];
		int param = ch->param;

		switch (ch->effect) {
		case 0x0:
			if (param) {
				/* Arpeggio cycles base / +x / +y every tick. Applied
				 * to the step only -- ch->period must survive, or the
				 * note ends up transposed when the effect stops. */
				int n = m->tick % 3;
				int note = period_to_note(ch->period);
				if (n == 1) note += param >> 4;
				else if (n == 2) note += param & 0x0F;
				set_step(m, ch, note_to_period(note, ch->finetune));
			}
			break;
		case 0x1:
			ch->period = clamp_period(ch->period - param);
			set_step(m, ch, ch->period);
			break;
		case 0x2:
			ch->period = clamp_period(ch->period + param);
			set_step(m, ch, ch->period);
			break;
		case 0x3:
			tone_porta(m, ch);
			break;
		case 0x4:
			do_vibrato(m, ch);
			break;
		case 0x5:
			tone_porta(m, ch);
			vol_slide(ch, param);
			break;
		case 0x6:
			do_vibrato(m, ch);
			vol_slide(ch, param);
			break;
		case 0xA:
			vol_slide(ch, param);
			break;
		case 0xE:
			if ((param >> 4) == 0x9) {
				int iv = ch->retrig_mem;
				if (iv && (m->tick % iv) == 0) trigger(m, ch, 0);
			} else if ((param >> 4) == 0xC) {
				if (m->tick == (param & 0x0F)) ch->volume = 0;
			}
			break;
		default:
			break;
		}
	}
}

static void advance_row(mod_player_t *m) {
	int c;

	/* Bxx and Dxx on the same row: ProTracker takes the jump for the
	 * order and the break for the row, which is how "jump to position
	 * N, row R" is written. Handling them independently is what makes
	 * that fall out. */
	if (m->jump_order >= 0) {
		m->order_pos = m->jump_order;
		m->row = (m->break_row >= 0) ? m->break_row : 0;
		m->jump_order = -1;
		m->break_row = -1;
		if (m->order_pos >= m->song_len) { m->order_pos = 0; m->loops++; }
		return;
	}

	if (m->break_row >= 0) {
		m->row = m->break_row;
		m->break_row = -1;
		m->order_pos++;
		if (m->order_pos >= m->song_len) { m->order_pos = 0; m->loops++; }
		return;
	}

	m->row++;
	if (m->row >= 64) {
		m->row = 0;
		m->order_pos++;
		if (m->order_pos >= m->song_len) { m->order_pos = 0; m->loops++; }
	}

	(void)c;
}

static void do_tick(mod_player_t *m) {
	if (m->tick == 0) {
		/* row_ticks is reset here, before the row is read, so that a
		 * pattern delay set on the PREVIOUS row does not leak into
		 * this one. EEx sets it again from inside row_tick0(). */
		m->row_ticks = m->speed;
		row_tick0(m);
	} else {
		row_tickn(m);
	}

	update_gains(m);

	m->tick++;
	if (m->tick >= m->row_ticks) {
		m->tick = 0;
		advance_row(m);
	}
}

/*
 * Per-channel gains, recomputed once per tick.
 *
 * The inner loop below runs channels x samples times a second and is
 * the single hottest thing this app does on a 48MHz core with no
 * instruction cache. Folding volume and pan together here turns
 *
 *     v = sample * volume;  l += (v * pan_l) >> 8;  r += (v * pan_r) >> 8;
 *
 * -- three multiplies and two shifts per channel per SAMPLE -- into
 * two multiplies, at a cost of sixteen multiplies per tick, i.e. about
 * 800 a second. Done here, at the end of every tick, rather than at
 * each of the half-dozen places volume can change (note trigger, Cxx,
 * Axy, EAx/EBx, ECx, tone-porta-plus-slide): one place that cannot
 * fall out of step, for a cost that does not show up in a profile.
 */
static void update_gains(mod_player_t *m) {
	int c;
	for (c = 0; c < m->channels; c++) {
		mod_channel_t *ch = &m->ch[c];
		ch->gain_l = ((int32_t)ch->volume * (int32_t)ch->pan_l) >> 8;
		ch->gain_r = ((int32_t)ch->volume * (int32_t)ch->pan_r) >> 8;
	}
}

/*
 * Mix accumulators.
 *
 * The loop below is CHANNEL-OUTER, SAMPLE-INNER, which is the whole
 * point of these existing. The obvious arrangement -- sample outer,
 * channel inner -- reloads every field of mod_channel_t (data, pos,
 * step, length, loop_start, loop_len, both gains) on every single
 * sample, because the compiler cannot prove the accumulators do not
 * alias the player struct. Inverting the loops hoists all of that into
 * registers for a whole block at a time and pays only two accumulator
 * accesses per channel per sample instead.
 *
 * That matters more here than it would elsewhere: picorv32 on Obst has
 * no instruction cache and its main memory is SRAM, so every avoided
 * load is a real bus cycle, not a cache hit.
 *
 * Statics rather than stack: MOD_MIX_BLOCK * 2 * 4 bytes is 512, and
 * an app's stack tier is 16KB shared with its heap (see mod.c).
 */
#define MOD_MIX_BLOCK 64

static int32_t acc_l[MOD_MIX_BLOCK];
static int32_t acc_r[MOD_MIX_BLOCK];

static void mix(mod_player_t *m, int16_t *out, uint32_t n) {
	uint32_t i;
	int c;
	int32_t master = m->master;

	for (i = 0; i < n; i++) {
		acc_l[i] = 0;
		acc_r[i] = 0;
	}

	for (c = 0; c < m->channels; c++) {
		mod_channel_t *ch = &m->ch[c];
		const int8_t *data;
		uint32_t pos, step, len, loop_end, loop_len;
		int32_t gl, gr;

		if (!ch->active) continue;
		if (ch->gain_l == 0 && ch->gain_r == 0) {
			/* Silent channel: advance the position without touching
			 * the accumulators. A muted channel still has to keep its
			 * place -- ProTracker modules fade a channel out and back
			 * in mid-note constantly -- but it costs nothing to skip
			 * the arithmetic while it is inaudible. */
			pos = ch->pos;
			step = ch->step;
			len = ch->length;
			loop_len = ch->loop_len << MOD_FRAC_BITS;
			loop_end = (ch->loop_start + ch->loop_len) << MOD_FRAC_BITS;
			for (i = 0; i < n; i++) {
				if ((pos >> MOD_FRAC_BITS) >= len) { ch->active = 0; break; }
				pos += step;
				if (loop_len) while (pos >= loop_end) pos -= loop_len;
			}
			ch->pos = pos;
			continue;
		}

		data = ch->data;
		pos = ch->pos;
		step = ch->step;
		len = ch->length;
		gl = ch->gain_l;
		gr = ch->gain_r;
		loop_len = ch->loop_len << MOD_FRAC_BITS;
		loop_end = (ch->loop_start + ch->loop_len) << MOD_FRAC_BITS;

		for (i = 0; i < n; i++) {
			int32_t s;

			if ((pos >> MOD_FRAC_BITS) >= len) { ch->active = 0; break; }

			s = data[pos >> MOD_FRAC_BITS];
			acc_l[i] += s * gl;
			acc_r[i] += s * gr;

			pos += step;

			/* A while rather than an if: nothing forbids a loop
			 * shorter than one step, and a module that does it would
			 * otherwise walk off the end of its own sample. It costs
			 * one untaken branch in every normal case. */
			if (loop_len) while (pos >= loop_end) pos -= loop_len;
		}

		ch->pos = pos;
	}

	for (i = 0; i < n; i++) {
		int32_t l = (acc_l[i] * master) >> 8;
		int32_t r = (acc_r[i] * master) >> 8;
		if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
		if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
		*out++ = (int16_t)l;
		*out++ = (int16_t)r;
	}
}

void modplay_render(mod_player_t *m, int16_t *out, int frames) {
	while (frames > 0) {
		uint32_t n;

		if (m->tick_remaining == 0) {
			do_tick(m);
			m->tick_remaining = m->samples_per_tick;
		}

		n = m->tick_remaining;
		if (n > (uint32_t)frames) n = (uint32_t)frames;
		if (n > MOD_MIX_BLOCK) n = MOD_MIX_BLOCK;

		mix(m, out, n);

		out += n * 2;
		frames -= (int)n;
		m->tick_remaining -= n;
	}
}

/* ------------------------------------------------------------------
 * display helpers
 * ------------------------------------------------------------------ */

static const char *const note_names[12] = {
	"C-", "C#", "D-", "D#", "E-", "F-",
	"F#", "G-", "G#", "A-", "A#", "B-"
};

void modplay_note_name(int note, char *out4) {
	if (note < 0 || note > 35) {
		out4[0] = '-'; out4[1] = '-'; out4[2] = '-'; out4[3] = 0;
		return;
	}
	out4[0] = note_names[note % 12][0];
	out4[1] = note_names[note % 12][1];
	out4[2] = (char)('1' + (note / 12));
	out4[3] = 0;
}

void modplay_get_cell(const mod_player_t *m, int order_pos, int row,
	int chan, mod_cell_t *out)
{
	const uint8_t *n;
	int period;

	out->note = -1;
	out->sample = 0;
	out->effect = 0;
	out->param = 0;

	if (!m->patterns) return;
	if (order_pos < 0 || order_pos >= m->song_len) return;
	if (row < 0 || row > 63) return;
	if (chan < 0 || chan >= m->channels) return;

	n = m->patterns
		+ (uint32_t)m->order[order_pos] * m->pattern_bytes
		+ (uint32_t)row * (uint32_t)m->channels * 4u
		+ (uint32_t)chan * 4u;

	period = ((n[0] & 0x0F) << 8) | n[1];
	out->sample = (n[0] & 0xF0) | (n[2] >> 4);
	out->effect = n[2] & 0x0F;
	out->param = n[3];

	/* The file stores a period, not a note index. period_to_note()
	 * is the same rounding the player itself uses, so what the
	 * display shows is what the player will actually play -- a
	 * separate nearest-note search here could disagree with it on a
	 * module whose periods are slightly off the standard table, and
	 * a display that disagrees with the audio is worse than no
	 * display. */
	if (period) out->note = period_to_note((int16_t)period);
}

/* ------------------------------------------------------------------
 * hardware mixer path (phase 3)
 * ------------------------------------------------------------------ */

void modplay_advance(mod_player_t *m) {
	do_tick(m);
}

void modplay_hw_channel(mod_player_t *m, int c, mod_hw_channel_t *out) {
	mod_channel_t *ch;
	mod_sample_t *s;

	out->base = 0;
	out->length = 0;
	out->loop_start = 0;
	out->loop_len = 0;
	out->sample_hz = 0;
	out->gain_l = 0;
	out->gain_r = 0;
	out->enable = 0;
	out->trigger = 0;
	out->offset = 0;

	if (c < 0 || c >= m->channels) return;
	ch = &m->ch[c];

	out->trigger = ch->hw_trigger;
	out->offset = ch->hw_offset;
	ch->hw_trigger = 0;

	if (ch->sample < 1 || ch->sample > MOD_MAX_SAMPLES) return;
	s = &m->samples[ch->sample];
	if (!s->data || s->length == 0) return;

	out->base = s->offset;
	out->length = s->length;
	out->loop_start = s->loop_start;
	out->loop_len = s->loop_len;

	/* Playback rate from the CURRENT period, which already includes
	 * finetune, vibrato and arpeggio -- set_step() computed the same
	 * thing for the software path, from the same number. One source
	 * for pitch, whichever mixer is running. */
	out->sample_hz = ch->period ? (MOD_CLOCK4 / 4u) / (uint32_t)ch->period : 0;

	/* gain_l/gain_r here are 0..64 volume against 0..256 pan, giving
	 * 0..64. The hardware takes 0..255, so scale up by 4 -- which is
	 * also what makes a single channel at full volume reach full
	 * hardware gain rather than a quarter of it. */
	out->gain_l = (uint8_t)(ch->gain_l > 63 ? 255 : ch->gain_l * 4);
	out->gain_r = (uint8_t)(ch->gain_r > 63 ? 255 : ch->gain_r * 4);
	out->enable = ch->active ? 1 : 0;
}
