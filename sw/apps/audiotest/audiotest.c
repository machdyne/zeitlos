/*
 * audiotest -- prove the audio path on real hardware
 *
 *   > run audiotest
 *
 * PHASE 1's test harness. Deliberately the dullest possible consumer
 * of sw/common/zaudio.h: no window, no mixer, no file loading, nothing
 * that could be the thing that is broken. If this makes a clean tone
 * then the DAC wiring, the clock dividers, the FIFO, the register
 * decode and the serialiser are all correct, and anything that goes
 * wrong afterwards is a software problem.
 *
 * That ordering is the whole point of doing it before the MOD player.
 * A tracker exercises pitch, volume, looping and multi-channel mixing
 * simultaneously, so when it sounds wrong there is nothing to bisect
 * against. This gives it something.
 *
 * -- what to check, in this order --
 *
 *   info      the register dump. If MAGIC is wrong, stop here; nothing
 *             else in this program means anything.
 *   tone      steady square wave. Should be a clean pitch with no
 *             clicking. Clicking means underruns, which the underrun
 *             count will confirm.
 *   sweep     rising pitch. Catches a stuck phase accumulator that a
 *             single tone would not.
 *   stereo    left channel only, then right only. This is the ONLY
 *             test that catches a swapped PT8211 WS polarity, which is
 *             the one thing in the RTL not verified against hardware
 *             -- see z_audio_swap_lr().
 *   silence   EN set, nothing pushed. Should be SILENT, not a buzz. A
 *             buzz here means the sigma-delta is not settling at 50%
 *             duty, or the amplifier is picking up the bit clock.
 *
 * Polled, not interrupt-driven, and that is on purpose too: this
 * program should depend on as little of the kernel as possible. The
 * interrupt path is exercised by the MOD player in phase 2.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"

#define SECONDS_PER_TEST 2

// A square wave, not a sine, and not because a sine is hard.
//
// A square is the harshest thing this path can be asked to carry: it
// is full-scale on every sample and it changes sign abruptly, so a
// sigma-delta that is marginally stable will misbehave on it long
// before it misbehaves on a sine. It also has a hard edge that shows
// up clearly on a scope at the DAC pin, which a sine does not.
//
// Amplitude is deliberately below full scale. The modulator has
// headroom by design (rtl/audio_out.v's SD_FS), but driving the very
// last bit of range proves nothing musical and makes clipping in the
// board's output stage look like a bug in here.
#define AMPLITUDE 20000

static uint32_t rate_hz;

/*
 * THE MIXER IS A BUS MASTER AND ISSUES PHYSICAL ADDRESSES.
 *
 * This process sees its own memory through the MTU, which remaps
 * 0x8000_0000 to wherever the kernel put it. Hand the mixer a pointer
 * unchanged and it fetches from whatever physically lives at that
 * offset -- on this SOC, the BIOS and the kernel. It plays, and it
 * plays the wrong memory. reg_mtu_base reads 0 where no translation is
 * active, so this is correct in both contexts.
 */
#define Z_APP_VIRT_BASE 0x80000000u

static uint32_t phys_of(const void *p) {
	return reg_mtu_base + ((uint32_t)p - Z_APP_VIRT_BASE);
}

/*
 * A sample for the hardware mixer to fetch.
 *
 * 256 bytes of square wave, signed 8-bit -- the format the mixer
 * reads. In .bss so it is resident and at a fixed physical address for
 * the whole run.
 *
 * This exists because until now NOTHING tested the mixer. audiotest
 * drove the FIFO and track drove the mixer, so when track sounded
 * wrong on a board where audiotest sounded right, there was no way to
 * tell whether the fault was in the mixer, in the bus path to main
 * memory, or in the MOD engine feeding it. This isolates the first
 * two: a known waveform, one channel, no tracker.
 */
#define MIXBUF_LEN 256
static int8_t mixbuf[MIXBUF_LEN];

// Push one frame, spinning until there is room. Returns the number of
// frames that were dropped, which should always be zero -- a nonzero
// count means the FIFO filled and this loop lost the race, which
// cannot happen while it is spinning but can if this is ever called
// from somewhere that yields.
static void push_blocking(int16_t l, int16_t r) {
	while (!z_audio_push(l, r))
		;
}

static void report_underruns(const char *what) {
	if (z_audio_underrun()) {
		printf("  %s: UNDERRUN -- the FIFO starved at least once.\n", what);
		printf("        With a polled loop this means something else\n");
		printf("        held the CPU. Not fatal; the last frame was\n");
		printf("        repeated.\n");
		z_audio_clear_underrun();
	} else {
		printf("  %s: ok, no underruns\n", what);
	}
}

static void show_info(void) {
	uint32_t fmt = z_audio_formats();
	uint32_t depth = z_audio_depth();

	printf("audio block:\n");
	printf("  magic    %08lx\n", (unsigned long)reg_audio_magic);
	printf("  config   %08lx\n", (unsigned long)reg_audio_config);
	printf("  clk      %lu Hz\n", (unsigned long)reg_audio_clkhz);
	printf("  rate div %lu -> %lu Hz\n",
		(unsigned long)(reg_audio_rate & 0xFF), (unsigned long)rate_hz);
	printf("  fifo     %lu frames (%lu ms at this rate)\n",
		(unsigned long)depth,
		(unsigned long)(rate_hz ? (depth * 1000) / rate_hz : 0));
	printf("  wmark    %lu frames\n", (unsigned long)reg_audio_wmark);

	printf("  dac      ");
	if (fmt & Z_AUDIO_FORMAT_SD) printf("1-bit sigma-delta ");
	if (fmt & Z_AUDIO_FORMAT_PT8211) printf("PT8211 ");
	if (fmt & Z_AUDIO_FORMAT_SPDIF) printf("S/PDIF ");
	if (!fmt) printf("none reported");
	printf("\n");
}

// Steady square wave at `hz`, for `seconds`. `chan` is a bitmask:
// 1 = left, 2 = right, 3 = both.
static void tone(uint32_t hz, uint32_t seconds, int chan) {
	uint32_t total = rate_hz * seconds;
	uint32_t half = rate_hz / (hz * 2);
	uint32_t i;
	uint32_t n = 0;
	int16_t v = AMPLITUDE;

	if (half == 0) half = 1;

	for (i = 0; i < total; i++) {
		push_blocking((chan & 1) ? v : 0, (chan & 2) ? v : 0);
		if (++n >= half) {
			n = 0;
			v = -v;
		}
	}
}

// Rising square wave. A single tone can be produced by a stuck
// counter; a sweep cannot.
static void sweep(uint32_t from_hz, uint32_t to_hz, uint32_t seconds) {
	uint32_t total = rate_hz * seconds;
	uint32_t i;
	uint32_t n = 0;
	uint32_t half;
	uint32_t hz;
	int16_t v = AMPLITUDE;

	for (i = 0; i < total; i++) {
		// linear in frequency, which is not linear to the ear -- fine,
		// this is a test, not music
		hz = from_hz + ((to_hz - from_hz) * i) / total;
		half = rate_hz / (hz * 2);
		if (half == 0) half = 1;

		push_blocking(v, v);
		if (++n >= half) {
			n = 0;
			v = -v;
		}
	}
}

/*
 * Play a static buffer through one hardware mixer channel.
 *
 * step is 1.0 in 18.14 fixed point, so the sample plays at the output
 * rate: MIXBUF_LEN bytes per cycle gives rate_hz/256 Hz, about 183 Hz
 * at 46875. A clean, steady tone means the mixer, the arbiter, the bus
 * and main memory are all delivering the right bytes.
 */
static void mixer_tone(int channels, uint32_t seconds)
{
	int c;

	for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;

	reg_audio_mixvol = 128;

	for (c = 0; c < channels; c++) {
		Z_AUDIO_CH_BASE(c)    = phys_of(mixbuf);
		Z_AUDIO_CH_LEN(c)     = MIXBUF_LEN;
		Z_AUDIO_CH_LOOPST(c)  = 0;
		Z_AUDIO_CH_LOOPLEN(c) = MIXBUF_LEN;     /* loop forever */
		Z_AUDIO_CH_STEP(c)    = 1u << Z_AUDIO_STEP_FRAC;
		Z_AUDIO_CH_CTRL(c)    = z_audio_ch_ctrl(96, 96, true, true, 0);
	}

	z_audio_mixer_enable(true);
	delay_ms(seconds * 1000);
	z_audio_mixer_enable(false);

	for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
}

int main(void) {

	printf("\naudiotest -- phase 1 audio path check\n\n");

	// Feature bit FIRST, then MAGIC. Reading MAGIC on a bitstream that
	// predates rtl/audio.v hangs the CPU -- see zaudio.h's header.
	if (!z_audio_present()) {
		if (!z_soc_has_feature(Z_FEATURE_AUDIO)) {
			printf("This bitstream has no audio block.\n");
			printf("Rebuild with `AUDIO in rtl/boards.vh for this board.\n");
		} else {
			printf("Audio feature bit is set but MAGIC reads %08lx,\n",
				(unsigned long)reg_audio_magic);
			printf("not %08lx. Something is decoding 0x7000_05xx that\n",
				(unsigned long)Z_AUDIO_MAGIC);
			printf("should not be. Check rtl/sysctl.v's cs_audio.\n");
		}
		return 1;
	}

	/* Start from the rate the BOARD came up with, not a constant.
	 *
	 * boards.vh picks a power-on rate the board's outputs can carry --
	 * Sergei sets 16 (46875Hz), the only rate whose S/PDIF half-cell
	 * is a whole number of sys_clk. Overwriting that with a hardcoded
	 * 44.1kHz is how this test ended up driving a digital output at a
	 * rate the board had deliberately avoided. */
	{
		uint32_t rd = reg_audio_rate & 0xFF;
		if (!z_audio_rate_ok(rd)) rd = Z_AUDIO_RATE_44K;
		z_audio_start(rd);
	}
	rate_hz = z_audio_rate_hz();

	show_info();

	/* A rate the board's own outputs cannot carry is worth shouting
	 * about: it makes every test below sound wrong for a reason that
	 * has nothing to do with what they are testing. */
	if ((z_audio_formats() & Z_AUDIO_FORMAT_SPDIF) &&
		!z_audio_rate_ok(reg_audio_rate & 0xFF)) {
		printf("\n  *** RATE %lu IS WRONG FOR S/PDIF ***\n",
			(unsigned long)(reg_audio_rate & 0xFF));
		printf("  Needs an EVEN divider of 23 or less (16 = 46875 Hz).\n");
		printf("  Odd rates need the fractional divider in\n");
		printf("  rtl/audio_spdif.v -- if you have only run\n");
		printf("  'make dev-flash', the BITSTREAM does not have it and\n");
		printf("  the line runs 6%% fast. Everything below will sound\n");
		printf("  like static over the signal.\n");
	}

	printf("\n");

	printf("1. 440 Hz, both channels, %d s\n", SECONDS_PER_TEST);
	tone(440, SECONDS_PER_TEST, 3);
	report_underruns("tone");

	printf("2. sweep 200 -> 2000 Hz, %d s\n", SECONDS_PER_TEST);
	sweep(200, 2000, SECONDS_PER_TEST);
	report_underruns("sweep");

	// The channel test. If these come out backwards the fix is
	// CTRL.SWAPLR, and the RIGHT fix is `AUDIO_CTRL_RESET in
	// rtl/boards.vh so the board comes up correct -- not a call to
	// z_audio_swap_lr() in every app that plays a sound.
	printf("3. LEFT channel only, %d s\n", SECONDS_PER_TEST);
	tone(440, SECONDS_PER_TEST, 1);
	printf("4. RIGHT channel only, %d s\n", SECONDS_PER_TEST);
	tone(880, SECONDS_PER_TEST, 2);
	printf("   (left should be the lower tone. If not, see\n");
	printf("    z_audio_swap_lr() in sw/common/zaudio.h.)\n");
	report_underruns("stereo");

	// Enabled and idle. Underrun is EXPECTED here -- the FIFO is
	// deliberately starved -- so clear it afterwards rather than
	// reporting it as a failure.
	printf("5. silence (enabled, nothing queued), 1 s\n");
	delay_ms(1000);
	z_audio_clear_underrun();
	printf("   should be silent, not a buzz\n");

	/* ---- hardware mixer ---- */
	if (z_audio_mixer_present()) {
		int i;

		for (i = 0; i < MIXBUF_LEN; i++)
			mixbuf[i] = (i < MIXBUF_LEN / 2) ? 100 : -100;

		/*
		 * Does the buffer survive being written?
		 *
		 * 6b found the CPU reading 00000000 from mixbuf when it should
		 * hold 64646464, while the mixer saw live-looking values at
		 * the same physical address -- small integers and a pointer
		 * equal to phys_of(mixbuf), which is a local from that loop.
		 * Locals live on the stack.
		 *
		 * So before blaming the mixer, check whether the buffer is
		 * still ours at all. Read it back with no intervening call,
		 * then again after a printf and a delay. If the first is right
		 * and the second is not, something -- most likely the stack --
		 * is writing over .bss, and every audio symptom downstream of
		 * that is a consequence rather than a cause.
		 */
		{
			uint32_t immediate = *(volatile uint32_t *)mixbuf;
			uint32_t after;
			int sp_probe;

			printf("\n6a. does mixbuf survive?\n");
			printf("   &mixbuf  %08lx   a stack local is near %08lx\n",
				(unsigned long)(uint32_t)mixbuf,
				(unsigned long)(uint32_t)&sp_probe);
			printf("   distance from buffer to stack: %ld bytes\n",
				(long)((uint32_t)&sp_probe - (uint32_t)mixbuf));
			printf("   immediately after filling : %08lx\n",
				(unsigned long)immediate);

			delay_ms(50);
			after = *(volatile uint32_t *)mixbuf;
			printf("   after a printf and a delay: %08lx\n",
				(unsigned long)after);

			if (immediate != 0x64646464u)
				printf("   *** the write itself did not stick ***\n");
			else if (after != immediate)
				printf("   *** something overwrote it -- .bss is not safe ***\n");
			else
				printf("   buffer is intact; the fault is downstream\n");
		}

		printf("\n6. HARDWARE MIXER, 1 channel, %d s\n", SECONDS_PER_TEST);
		printf("   sample at phys %08lx, %d bytes, looped\n",
			(unsigned long)phys_of(mixbuf), MIXBUF_LEN);
		printf("   expect a steady %lu Hz tone. This is the first test\n",
			(unsigned long)(rate_hz / MIXBUF_LEN));
		printf("   that exercises the mixer's reads from main memory --\n");
		printf("   if 1-5 were clean and this is not, the fault is in\n");
		printf("   the mixer or the bus path, not the output stage.\n");
		mixer_tone(1, SECONDS_PER_TEST);
		printf("   mixstat after: %02lx (expect 00, one-shot ended or\n",
			(unsigned long)(reg_audio_mixstat & 0xFF));
		printf("   still looping depending on timing)\n");

		/* Self-checking: does the mixer see what the CPU sees?
		 *
		 * The probe reports the address and full word of the mixer's
		 * last fetch. Translating that address back through the MTU
		 * lets the CPU read the same location and compare. A mismatch
		 * says the mixer's path to main memory is delivering wrong
		 * data -- which no amount of listening can distinguish from
		 * the mixer mixing correct data badly. */
		{
			int i, c, bad = 0, checked = 0, outside = 0;
			uint32_t lo = phys_of(mixbuf);
			uint32_t hi = lo + MIXBUF_LEN;

			printf("\n6b. bus read check (mixer vs CPU, same address)\n");

			/* Prove the buffer and the back-translation FIRST.
			 *
			 * Without this the test cannot be trusted: a mismatch
			 * could equally mean the mixer read wrong data, or that
			 * this code computed the wrong virtual address and
			 * compared against unrelated memory. Both look identical
			 * in the output. */
			printf("   mixbuf virt %08lx phys %08lx..%08lx, mtu_base %08lx\n",
				(unsigned long)(uint32_t)mixbuf,
				(unsigned long)lo, (unsigned long)hi,
				(unsigned long)reg_mtu_base);
			printf("   first word via pointer   : %08lx\n",
				(unsigned long)*(volatile uint32_t *)mixbuf);
			printf("   first word via phys->virt: %08lx  (must match)\n",
				(unsigned long)*(volatile uint32_t *)
					((lo - reg_mtu_base) + Z_APP_VIRT_BASE));
			printf("   expect 64646464 (+100 run). If the two differ,\n");
			printf("   the translation is wrong and the verdict below\n");
			printf("   is meaningless -- say so rather than trusting it.\n");

			for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;
			Z_AUDIO_CH_BASE(0)    = lo;
			Z_AUDIO_CH_LEN(0)     = MIXBUF_LEN;
			Z_AUDIO_CH_LOOPST(0)  = 0;
			Z_AUDIO_CH_LOOPLEN(0) = MIXBUF_LEN;
			Z_AUDIO_CH_STEP(0)    = 1u << Z_AUDIO_STEP_FRAC;
			Z_AUDIO_CH_CTRL(0)    = z_audio_ch_ctrl(96, 96, true, true, 0);
			z_audio_mixer_enable(true);

			for (i = 0; i < 32; i++) {
				uint32_t a, d, virt, mine;

				delay_ms(3);
				a = reg_audio_mixdbg_adr;
				d = reg_audio_mixdbg_dat;
				if (a == 0) continue;

				/* Is the mixer even fetching from our buffer? If not,
				 * the fault is in the address, not the data, and that
				 * is a completely different bug. */
				if (a < lo || a >= hi) {
					if (outside < 3)
						printf("   OUTSIDE BUFFER @%08lx (buf %08lx..%08lx)\n",
							(unsigned long)a, (unsigned long)lo,
							(unsigned long)hi);
					outside++;
					continue;
				}

				virt = (a - reg_mtu_base) + Z_APP_VIRT_BASE;
				mine = *(volatile uint32_t *)virt;

				checked++;
				if (mine != d) {
					if (bad < 4)
						printf("   MISMATCH @%08lx mixer=%08lx cpu=%08lx\n",
							(unsigned long)a, (unsigned long)d,
							(unsigned long)mine);
					bad++;
				}
			}

			z_audio_mixer_enable(false);
			for (c = 0; c < 8; c++) Z_AUDIO_CH_CTRL(c) = 0;

			if (outside)
				printf("   %d fetch(es) OUTSIDE the buffer -- the mixer's\n"
				       "   ADDRESS is wrong, not its data\n", outside);
			if (checked == 0)
				printf("   no in-range fetches observed\n");
			else if (bad)
				printf("   %d of %d in-range fetches returned wrong data\n",
					bad, checked);
			else
				printf("   %d of %d match -- mixer reads memory correctly\n",
					checked, checked);
		}

		printf("\n7. HARDWARE MIXER, 4 channels, %d s\n", SECONDS_PER_TEST);
		printf("   same sample on 4 channels -- 4x the bus traffic.\n");
		mixer_tone(4, SECONDS_PER_TEST);

		printf("\n8. HARDWARE MIXER, 8 channels, %d s\n", SECONDS_PER_TEST);
		printf("   worst case for bus contention.\n");
		mixer_tone(8, SECONDS_PER_TEST);
	} else {
		printf("\n6-8. skipped: no hardware mixer in this bitstream\n");
	}

	z_audio_stop();
	printf("\ndone.\n");

	return 0;
}
