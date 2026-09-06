/*
 * chip8 -- sound. See sound.h.
 */

#include <string.h>
#include <stdint.h>

#include "../../common/zeitlos.h"
#include "../../common/zaudio.h"

#include "sound.h"

/* One channel. Nothing else in this app makes noise, and taking one of
 * eight leaves the rest alone for whatever else is running. */
#define C8_SND_CH 0

/* 128 samples: XO-CHIP's pattern buffer is 128 bits, one sample per
 * bit. The default buzzer uses the same 128 samples as eight cycles of
 * a square wave, so both paths share one buffer and one code path.
 *
 * Signed 8-bit, and deliberately not full scale. A hard square at
 * +/-127 on every channel is loud enough to be unpleasant next to
 * anything else the machine is playing, and clips the mixer's sum with
 * one more voice. */
#define C8_SND_SAMPLES 128
#define C8_SND_HI   100
#define C8_SND_LO  -100

/* Volume. Not exposed yet -- phase 6 puts it in the per-ROM config
 * along with the key map. */
#define C8_SND_GAIN 110

static int8_t wave[C8_SND_SAMPLES];

static bool have_audio;
static bool playing;

/* What the buffer was last built from, so an unchanged pattern costs
 * nothing per frame. */
static uint32_t last_gen = 0xFFFFFFFFu;
static bool last_gen_valid;
static uint32_t last_step;

/* 2^(k/48) in Q14, for the XO-CHIP pitch formula below. A table
 * because this core's floating point is libgcc, and 48 constants are
 * smaller and exact where a pow() call is neither. */
static const uint16_t pow2_48[48] = {
	16384, 16622, 16864, 17109, 17358, 17611, 17867, 18127,
	18390, 18658, 18929, 19205, 19484, 19767, 20055, 20347,
	20643, 20943, 21247, 21556, 21870, 22188, 22511, 22838,
	23170, 23507, 23849, 24196, 24548, 24905, 25268, 25635,
	26008, 26386, 26770, 27159, 27554, 27955, 28362, 28774,
	29193, 29618, 30048, 30485, 30929, 31379, 31835, 32298
};

/* App virtual address -> physical.
 *
 * THE MIXER IS A BUS MASTER AND DOES NOT GO THROUGH THE MTU. Handing
 * it an app pointer does not fail quietly: it requests an address
 * nothing decodes, an undecoded address on this bus never acks, and
 * the mixer holds its grant on the main arbiter forever. The CPU is
 * starved of the bus and the machine stops.
 *
 * Same translation sw/apps/gamedemo's music.c and sw/apps/track both
 * do, for the same reason. */
static uint32_t phys_of(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;
	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

/* XO-CHIP playback rate: 4000 * 2^((pitch - 64) / 48) Hz.
 *
 * Split into a whole number of octaves and a remainder so the table
 * above only needs one octave. C's / and % truncate toward zero, which
 * for a negative exponent -- every pitch below 64, i.e. most of the
 * useful range -- gives the wrong octave and a remainder outside the
 * table. Hence the explicit floor. */
static uint32_t pitch_to_hz(uint8_t pitch) {

	int e = (int)pitch - 64;
	int oct = e / 48;
	int rem = e % 48;
	uint32_t hz;

	if (rem < 0) { rem += 48; oct -= 1; }

	hz = (4000u * pow2_48[rem]) >> 14;

	if (oct > 0) hz <<= oct;
	else if (oct < 0) hz >>= -oct;

	if (hz < 1) hz = 1;
	return hz;

}

/* Fill the buffer with eight cycles of a square wave.
 *
 * Eight rather than one so the loop is long enough for the mixer's
 * step to land near an exact number of samples per cycle at ordinary
 * rates -- one cycle of 128 samples would need a 56kHz playback rate
 * for a 440Hz tone, which is above what the DAC runs at. */
static void build_square(void) {
	int i;
	for (i = 0; i < C8_SND_SAMPLES; i++)
		wave[i] = ((i / 8) & 1) ? C8_SND_LO : C8_SND_HI;
}

/* Expand XO-CHIP's 128-bit pattern, MSB of byte 0 first. */
static void build_pattern(const c8_t *c) {
	int i;
	for (i = 0; i < C8_SND_SAMPLES; i++) {
		int bit = (c->pattern[i >> 3] >> (7 - (i & 7))) & 1;
		wave[i] = bit ? C8_SND_HI : C8_SND_LO;
	}
}

void c8_sound_init(void) {

	have_audio = z_audio_present() && z_audio_mixer_present();
	playing = false;
	last_gen_valid = false;
	last_step = 0;

	if (!have_audio) return;

	build_square();

	/* Start the DAC if nothing else already has. z_audio_start()
	 * flushes and re-enables unconditionally, which is harmless here
	 * -- this app is the only thing making noise while it runs -- and
	 * is the only way to be sure the rate divider is something we
	 * know. */
	z_audio_start(Z_AUDIO_RATE_44K);
	z_audio_mixer_enable(true);

	Z_AUDIO_CH_BASE(C8_SND_CH)    = phys_of(wave);
	Z_AUDIO_CH_LEN(C8_SND_CH)     = C8_SND_SAMPLES;
	Z_AUDIO_CH_LOOPST(C8_SND_CH)  = 0;
	Z_AUDIO_CH_LOOPLEN(C8_SND_CH) = C8_SND_SAMPLES;
	Z_AUDIO_CH_CTRL(C8_SND_CH)    = z_audio_ch_ctrl(0, 0, false, false, 0);

}

bool c8_sound_available(void) {
	return have_audio;
}

void c8_sound_update(const c8_t *c) {

	bool want = (c->st != 0);
	uint32_t hz, step;

	if (!have_audio) return;

	/* Rebuild only when the guest has actually touched its audio
	 * state. audio_gen exists precisely so this is a word compare
	 * rather than a 16-byte memcmp every frame. */
	if (!last_gen_valid || c->audio_gen != last_gen) {

		if (c->audio_gen == 0) {
			/* No XO-CHIP audio instruction has ever run, so this is a
			 * CHIP-8 or SUPER-CHIP program and the sound is the
			 * buzzer. Eight cycles in the buffer, so 440Hz wants 16
			 * samples per cycle. */
			build_square();
			hz = 440u * 16u;
		} else {
			build_pattern(c);
			hz = pitch_to_hz(c->pitch);
		}

		step = z_audio_step(hz, z_audio_rate_hz());
		if (step == 0) step = 1;
		last_step = step;

		Z_AUDIO_CH_STEP(C8_SND_CH) = step;

		last_gen = c->audio_gen;
		last_gen_valid = true;

		/* A pattern rewritten mid-note must NOT restart the channel:
		 * XO-CHIP programs modulate the buffer while it plays, and
		 * retriggering on every write turns a sustained tone into a
		 * clicking one. Only the gate below ever triggers. */
		if (playing)
			Z_AUDIO_CH_CTRL(C8_SND_CH) =
				z_audio_ch_ctrl(C8_SND_GAIN, C8_SND_GAIN, true, false, 0);

	}

	if (want == playing) return;

	if (want) {
		Z_AUDIO_CH_STEP(C8_SND_CH) = last_step;
		Z_AUDIO_CH_CTRL(C8_SND_CH) =
			z_audio_ch_ctrl(C8_SND_GAIN, C8_SND_GAIN, true, true, 0);
	} else {
		Z_AUDIO_CH_CTRL(C8_SND_CH) = z_audio_ch_ctrl(0, 0, false, false, 0);
	}

	playing = want;

}

void c8_sound_shutdown(void) {
	if (!have_audio) return;
	Z_AUDIO_CH_CTRL(C8_SND_CH) = z_audio_ch_ctrl(0, 0, false, false, 0);
	playing = false;
}
