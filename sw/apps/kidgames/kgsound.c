/*
 * kidgames -- sound. See kgsound.h.
 */

#include "kgsound.h"
#ifndef KG_SOUND_HOSTED
#include "../../common/zeitlos.h"
#include "../../common/zaudio.h"
#endif

/*
 * Channel 7, not 0.
 *
 * Every other app that makes a noise takes channel 0 -- chip8's
 * buzzer, gamedemo's music, audiotest. The mixer is global state and
 * outlives any one app, so a cue fired here on channel 0 would cut off
 * whatever a tracker in another window was playing through it. Taking
 * the far end leaves the low channels, which everything else reaches
 * for first, alone.
 */
#define SND_CH 7

/* Rendering rate. Not the DAC's rate -- the mixer resamples, and
 * CH_STEP is how it is told what to resample from. 11kHz is plenty for
 * three short tones and keeps the buffer a quarter the size 44kHz
 * would need, which is the whole reason to pick it. */
#define SND_HZ 11025

/* 4096 samples is 372ms at SND_HZ, which is longer than the longest
 * cue. Static: an app's stack and heap come out of one 16KB allocation
 * (Z_PROC_STACK_SIZE_DEFAULT) and this is a quarter of it. */
#define SND_MAX 4096

/* Not full scale. A hard tone at +/-127 is unpleasant next to anything
 * else the machine is playing and clips the mixer's sum with one more
 * voice -- chip8's sound.c makes the same point and picks the same
 * kind of number. */
#define SND_AMP 90
#define SND_GAIN 100

static int8_t wave[SND_MAX];
static int wave_len;
static bool have_audio;

/*
 * App virtual address -> physical.
 *
 * THE MIXER IS A BUS MASTER AND DOES NOT GO THROUGH THE MTU. Handing
 * it an app pointer does not fail quietly: it requests an address
 * nothing decodes, an undecoded address on this bus never acks, and
 * the mixer holds its grant on the main arbiter forever. The CPU is
 * starved of the bus and the machine stops -- not this app, the
 * machine.
 *
 * The same translation sw/apps/chip8's sound.c, gamedemo's music.c and
 * sw/apps/track all do, for the same reason. It is copied rather than
 * shared because none of them has put it in sw/common yet, and a
 * fourth copy is a better argument for doing so than a third was.
 */
#ifndef KG_SOUND_HOSTED
static uint32_t phys_of(const void *p)
{
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;

	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;

	return base + (v & 0x0FFFFFFFu);
}
#endif

/*
 * Append `ms` of a triangle wave at `hz`, fading from amp0 to amp1.
 *
 * Triangle rather than square. A square is one comparison per sample
 * and is what every other beep in this tree uses, and it is the wrong
 * choice here: its odd harmonics make it read as an ALARM, which is
 * exactly the wrong thing to say to a six-year-old who has just got an
 * answer wrong. A triangle is two multiplies and sounds like a note.
 *
 * The fade is what keeps it from clicking. A tone that stops at full
 * amplitude is a step in the waveform, and a step is a click -- on a
 * small speaker, a louder one than the note.
 */
static void note(int hz, int ms, int amp0, int amp1)
{
	int n = (ms * SND_HZ) / 1000;
	int period = hz > 0 ? SND_HZ / hz : SND_HZ;
	int half = period / 2;
	int i;

	if (half < 1) half = 1;

	for (i = 0; i < n && wave_len < SND_MAX; i++) {

		int p = i % period;
		int amp = amp0 + ((amp1 - amp0) * i) / (n > 1 ? n - 1 : 1);
		int v;

		if (p < half) v = -amp + (2 * amp * p) / half;
		else v = amp - (2 * amp * (p - half)) / half;

		wave[wave_len++] = (int8_t)v;

	}
}

/* A gap. Silence between notes, so two notes read as two rather than
 * as one note that changes pitch. */
static void rest(int ms)
{
	int n = (ms * SND_HZ) / 1000;
	int i;

	for (i = 0; i < n && wave_len < SND_MAX; i++) wave[wave_len++] = 0;
}

#ifndef KG_SOUND_HOSTED

void kg_sound_init(void)
{
	have_audio = z_audio_present() && z_audio_mixer_present();
	wave_len = 0;

	if (!have_audio) return;

	/* Start the DAC if nothing else has. z_audio_start() flushes and
	 * re-enables unconditionally -- harmless, and the only way to be
	 * sure the rate divider is one we know, which CH_STEP is computed
	 * against. */
	z_audio_start(Z_AUDIO_RATE_44K);
	z_audio_mixer_enable(true);

	Z_AUDIO_CH_BASE(SND_CH)    = phys_of(wave);
	Z_AUDIO_CH_LOOPST(SND_CH)  = 0;
	/* 0 is ONE-SHOT (docs/audio.md's channel register table): the
	 * channel plays to CH_LEN and stops by itself, which is the whole
	 * reason a cue needs no timer and no further CPU. */
	Z_AUDIO_CH_LOOPLEN(SND_CH) = 0;
	Z_AUDIO_CH_CTRL(SND_CH)    = z_audio_ch_ctrl(0, 0, false, false, 0);
}

void kg_sound_shutdown(void)
{
	if (!have_audio) return;

	Z_AUDIO_CH_CTRL(SND_CH) = z_audio_ch_ctrl(0, 0, false, false, 0);
}

bool kg_sound_available(void) { return have_audio; }

#endif	/* !KG_SOUND_HOSTED */

int kg_sound_buffer_max(void) { return SND_MAX; }

int kg_sound_render(kg_sound_t which)
{
	/*
	 * Rendered into the one buffer, per cue, rather than three buffers
	 * built once. Three would be 12KB of .bss on an app that already
	 * carries a 1.5KB doubling buffer and 3KB of art, to save a few
	 * thousand integer operations that happen at most twice a minute.
	 *
	 * Overwriting a buffer the mixer is still reading would glitch,
	 * and cannot happen here: the closest two cues can fall is a right
	 * answer followed by a level-up, and kg_round_show() blocks for
	 * 1600ms between them -- five times the longest cue. Worth knowing
	 * if a fourth cue is ever added somewhere tighter.
	 */
	wave_len = 0;

	switch (which) {

	case KG_SND_RIGHT:
		/* Two notes going UP. Up is the entire message, and it is one
		 * a kid reads without being taught. */
		note(660, 80, SND_AMP, SND_AMP);
		note(880, 120, SND_AMP, 0);
		break;

	case KG_SND_WRONG:
		/*
		 * One low note, fading.
		 *
		 * Deliberately not a descending pair, which is the obvious
		 * mirror of the right answer and sounds like a rebuke. This
		 * app never punishes a wrong answer -- it shows the correct
		 * one and moves on -- and the sound should agree with that.
		 * Low and brief reads as "not that one", where a falling
		 * fourth reads as "no".
		 */
		note(220, 200, SND_AMP, 0);
		break;

	case KG_SND_LEVELUP:
		/*
		 * Three notes up, a little arpeggio. Longer than the others
		 * because it happens rarely and is the one moment in the app
		 * worth making a fuss of.
		 *
		 * 330ms TOTAL, and the number matters: SND_MAX is 4096
		 * samples, which at SND_HZ is 371ms. The first version of
		 * this ran 400ms and was therefore cut off partway through
		 * its last note -- inaudibly wrong in the sense that nothing
		 * failed, and audibly wrong in that a note stopping at full
		 * amplitude is a click.
		 *
		 * tests/test_data.c caught it on the run it was written,
		 * which is the argument for that test existing: nobody was
		 * ever going to report "the level-up sound ends slightly
		 * abruptly" as a bug. Anything added here has to keep the
		 * total under SND_MAX, and the test is what enforces it.
		 */
		note(523, 70, SND_AMP, SND_AMP);
		rest(20);
		note(659, 70, SND_AMP, SND_AMP);
		rest(20);
		note(784, 150, SND_AMP, 0);
		break;

	}

	return wave_len;
}

#ifndef KG_SOUND_HOSTED

void kg_sound_play(kg_sound_t which)
{
	uint32_t step;

	if (!have_audio) return;

	if (kg_sound_render(which) <= 0) return;

	step = z_audio_step(SND_HZ, z_audio_rate_hz());
	if (step == 0) step = 1;

	Z_AUDIO_CH_BASE(SND_CH) = phys_of(wave);
	Z_AUDIO_CH_LEN(SND_CH)  = (uint32_t)wave_len;
	Z_AUDIO_CH_STEP(SND_CH) = step;

	/* enable AND trig: trig is what restarts a channel from the
	 * offset field. Enable without trig would leave a cue that has
	 * already finished sitting silent at its end. */
	Z_AUDIO_CH_CTRL(SND_CH) =
		z_audio_ch_ctrl(SND_GAIN, SND_GAIN, true, true, 0);
}

#else	/* KG_SOUND_HOSTED */

/*
 * The host build keeps the RENDERER -- note(), rest() and
 * kg_sound_render() above are the shipped ones -- and drops only the
 * four entry points that touch the mixer's registers, which are
 * unmapped MMIO on a build machine.
 *
 * That split is the point. A stub for the whole file would make
 * tests/test_data.c's buffer-overrun check test nothing at all.
 */
void kg_sound_init(void) { }
void kg_sound_shutdown(void) { }
bool kg_sound_available(void) { return false; }
void kg_sound_play(kg_sound_t which) { (void)which; }

#endif
