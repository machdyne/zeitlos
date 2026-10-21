#ifndef ZAUDIO_H
#define ZAUDIO_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Audio: pushing 16-bit stereo frames at the DAC. See docs/audio.md
 * for the whole picture and rtl/audio.v for the hardware.
 *
 * This is PHASE 1 of the audio subsystem. Software mixes; the hardware
 * owns the sample clock and the serial format, and nothing else. There
 * is no per-channel state in the hardware yet -- that is phase 3, and
 * when it lands it will be an addition to this register window rather
 * than a replacement for it, so code written against this header keeps
 * working.
 *
 * -- THE AUDIO BLOCK IS OPTIONAL. CHECK FOR IT, AND CHECK THE RIGHT
 *    WAY --
 *
 * Read this before calling anything below from a new app.
 *
 * `AUDIO in rtl/boards.vh is PER-BOARD, unlike `RTC and `TRNG which
 * are universal -- audio needs pins and a DAC on the other end of
 * them, so a board without it is normal rather than stripped. Two
 * different absences are possible and they do NOT behave the same:
 *
 *   `AUDIO off in this build -- rtl/sysctl.v hands 0x7000_05xx to
 *                     csrs.v, which acks it and reads back 0.
 *                     Harmless: the MAGIC check fails and everything
 *                     below correctly concludes there is no audio.
 *
 *   bitstream predates
 *   rtl/audio.v    -- nothing decodes 0x7000_05xx, and an undecoded
 *                     address on this bus gets NO ACK AT ALL. The CPU
 *                     waits for it forever. That is a dead hang, not
 *                     a read of undefined data -- the same hazard
 *                     rtl/cache.v, rtl/rtc.v and rtl/trng.v already
 *                     document for their own windows.
 *
 * So the FIRST probe must be Z_FEATURE_AUDIO, which lives in csrs.v at
 * 0x7000_0008 -- an address every bitstream ever built decodes.
 * z_audio_present() does it in that order and is what apps should
 * call. Do not read reg_audio_magic directly.
 *
 * -- which DAC am I driving? --
 *
 * You are not, and you should not care. The 1-bit sigma-delta output
 * (Obst, Lakritz) and the PT8211 (Mozart) take exactly the same frames
 * through exactly the same registers; only rtl/audio_out.v's last
 * stage differs. CONFIG's format bits exist so a diagnostic can SAY
 * which one is wired, not so playback code can branch on it.
 *
 * -- the shape of a player --
 *
 * Two ways to feed it, and the choice is about how much latency you
 * can afford, not about correctness.
 *
 * POLLED, for a beep or a test tone:
 *
 *     z_audio_start_hz(z_audio_preferred_hz());
 *     while (playing) {
 *         while (!z_audio_full())
 *             z_audio_push(left, right);
 *         // ... do other work, come back soon ...
 *     }
 *     z_audio_stop();
 *
 * INTERRUPT-DRIVEN, for anything that has to keep running while other
 * things happen. Enable Z_IRQ_AUDIO (sw/os/kernel.h) and fill from the
 * handler. THE HANDLER MUST EITHER FILL THE FIFO OR CLEAR IRQEN BEFORE
 * RETURNING: the interrupt is level-sensitive and stays asserted while
 * the FIFO is below its watermark, so a handler that returns having
 * done neither is re-entered immediately and the machine makes no
 * forward progress. This is the same rule the UART's interrupt already
 * follows.
 *
 * -- underruns --
 *
 * An underrun is not fatal and does not need handling to keep playing;
 * the hardware repeats the last frame and sets a sticky bit. It IS
 * worth checking during development, because it is the difference
 * between "the mixer is fast enough" and "the mixer is fast enough
 * most of the time", and by ear those sound identical until they
 * don't. z_audio_underrun() answers it; z_audio_clear_underrun()
 * resets it.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"

// rtl/audio.v, 0x7000_05xx -- the sixth tenant of nibble 7, alongside
// csrs (00xx), cache (01xx), socctl (02xx), rtc (03xx) and trng (04xx).
#define reg_audio_magic  (*(volatile uint32_t*)0x70000500)
#define reg_audio_ctrl   (*(volatile uint32_t*)0x70000504)
#define reg_audio_status (*(volatile uint32_t*)0x70000508)
#define reg_audio_data   (*(volatile uint32_t*)0x7000050c)
#define reg_audio_rate   (*(volatile uint32_t*)0x70000510)
#define reg_audio_wmark  (*(volatile uint32_t*)0x70000514)
#define reg_audio_config (*(volatile uint32_t*)0x70000518)
#define reg_audio_clkhz  (*(volatile uint32_t*)0x7000051c)
#define reg_audio_mixvol (*(volatile uint32_t*)0x70000520)
#define reg_audio_mixstat (*(volatile uint32_t*)0x70000524)

/* Bus read probe (rtl/audio_mixer.v): the address and the full 32-bit
 * word of the mixer's LAST fetch. Read the same address from the CPU
 * and compare -- if they differ, the mixer is not seeing what the CPU
 * sees, and the fault is in the bus path rather than in the mixing. */
#define reg_audio_mixdbg_adr (*(volatile uint32_t*)0x70000528)
#define reg_audio_mixdbg_dat (*(volatile uint32_t*)0x7000052c)

/* Playback position readback (rtl/audio_mixer.v's pos_sel/pos_o).
 *
 * MIXPOSSEL selects a channel, MIXPOS reports that channel's phase
 * accumulator in the same 18.14 fixed point CH_STEP uses -- so the
 * integer part is a BYTE OFFSET from that channel's CH_BASE.
 *
 * Why this exists: with CH_LOOPST 0 and CH_LOOPLEN set to a buffer
 * size, a channel walks a RING BUFFER forever, and the mixer becomes a
 * playback engine that costs the CPU nothing per frame. The one thing
 * software then cannot work out for itself is how much of the ring has
 * been consumed, which is exactly what it needs in order to know how
 * much is safe to overwrite. mixdbg_adr cannot answer it: that is the
 * last fetch made by WHICHEVER channel fetched most recently, so with
 * two channels running it reports one of them at random.
 */
#define reg_audio_mixpossel  (*(volatile uint32_t*)0x70000530)
#define reg_audio_mixpos     (*(volatile uint32_t*)0x70000534)

/* The SYSTEM volume (rtl/audio.v, register 14): 0..Z_AUDIO_VOLUME_UNITY,
 * applied to everything the block plays, after the FIFO/mixer select
 * and before every DAC. wm owns it (Super+Minus / Super+Equal, and the
 * keypad - and +); an app's own volume is relative to it, and an app
 * should not write it. Resets to 128, 50%. docs/audio.md, "System
 * volume". Probe with z_audio_has_volume() before touching it. */
#define reg_audio_volume     (*(volatile uint32_t*)0x70000538)
#define Z_AUDIO_VOLUME_UNITY 256u
#define Z_AUDIO_CONFIG_HAS_VOLUME (1u << 14)

/* FSDIV (rtl/audio.v, register 15): the sample clock as { MOD, STEP },
 * fs = CLKHZ * STEP / (128 * MOD). Present when CONFIG bit 15 is set
 * (z_audio_has_exact()). Do not write it directly: z_audio_start_hz()
 * and z_audio_set_rate_hz() do the arithmetic, and also keep RATE and
 * the S/PDIF channel-status code in step with it. docs/audio.md,
 * "Exact sample rates". */
#define reg_audio_fsdiv      (*(volatile uint32_t*)0x7000053c)
#define Z_AUDIO_CONFIG_HAS_EXACT (1u << 15)

/* MIXVOL's second byte: the S/PDIF channel-status rate code (AES3
 * byte 3: 0 = 44.1k, 2 = 48k, 3 = 32k, 4 = 22.05k). A byte store, so
 * MIXVOL itself is untouched. */
#define reg_audio_spdif_fs   (*(volatile uint8_t*)0x70000521)

/*
 * Hardware mixer channel registers (rtl/audio_mixer.v).
 *
 * Six words per channel, channel c at word 16 + c*6. WRITE ONLY --
 * these land in the mixer's own register file and do not read back.
 *
 * The whole point of this block is that audio stops depending on how
 * much CPU the player gets. Software's remaining job is to write these
 * when a tracker row changes -- about fifty times a second -- and
 * nothing else. There is no per-sample work left.
 */
#define Z_AUDIO_CH_BASE(c)    (*(volatile uint32_t*)(0x70000540u + (c)*24u + 0u))
#define Z_AUDIO_CH_LEN(c)     (*(volatile uint32_t*)(0x70000540u + (c)*24u + 4u))
#define Z_AUDIO_CH_LOOPST(c)  (*(volatile uint32_t*)(0x70000540u + (c)*24u + 8u))
#define Z_AUDIO_CH_LOOPLEN(c) (*(volatile uint32_t*)(0x70000540u + (c)*24u + 12u))
#define Z_AUDIO_CH_STEP(c)    (*(volatile uint32_t*)(0x70000540u + (c)*24u + 16u))
#define Z_AUDIO_CH_CTRL(c)    (*(volatile uint32_t*)(0x70000540u + (c)*24u + 20u))

#define Z_AUDIO_CH_EN    (1u << 16)
#define Z_AUDIO_CH_TRIG  (1u << 17)   /* restart from the offset field */

/*
 * Fetch 16-bit samples instead of 8-bit.
 *
 * Clear by default, so a channel comes up 8-bit and every app written
 * before this bit existed is unaffected -- z_audio_ch_ctrl() has never
 * set it and still does not.
 *
 * CHECK Z_AUDIO_MIXER_FMT16 FIRST. An older bitstream accepts this bit
 * and ignores it, so the channel plays a 16-bit buffer as 8-bit
 * garbage at half the intended rate. That is a considerably worse
 * failure than "not supported", and it is exactly why the capability
 * is reported in CONFIG rather than assumed.
 *
 * The sample position (CH_STEP, CH_BASE, CH_LEN, CH_LOOPST,
 * CH_LOOPLEN) is still counted in BYTES in this mode -- so a 16-bit
 * mono stream steps two bytes per source frame, and an interleaved
 * 16-bit stereo pair steps four.
 */
#define Z_AUDIO_CH_FMT16 (1u << 18)

/*
 * The speech channel.
 *
 * While the tts service is running (the pid name "tts0" is registered
 * -- sw/common/ztts.h), it streams speech on this mixer channel. An
 * app that programs or clears every channel must leave this one alone
 * while speech is on, so that someone who cannot see the screen can
 * still hear it with music playing. With speech off, every channel is
 * the app's, exactly as before. Check once, when claiming the mixer:
 *
 *     uint32_t pid;
 *     bool speech = z_pid_lookup(Z_TTS_SERVICE, &pid);
 *
 * See docs/tts.md, "Audio", and docs/audio.md, "Arbitration".
 */
#define Z_AUDIO_CH_SPEECH 7

/* Fractional bits in CH_STEP and in the mixer's phase accumulator.
 * MUST match FRAC_BITS in rtl/audio_mixer.v -- there is no shared
 * source, and a mismatch is a pitch error, not a failure. */
#define Z_AUDIO_STEP_FRAC 14



#define Z_AUDIO_MAGIC      0x5A415544u  // "ZAUD"
#define Z_AUDIO_CONFIG_SIG 0x5A41u      // "ZA", in CONFIG's top half

#define Z_AUDIO_CTRL_EN     (1u << 0)
#define Z_AUDIO_CTRL_IRQEN  (1u << 1)
#define Z_AUDIO_CTRL_SWAPLR (1u << 2)
#define Z_AUDIO_CTRL_FLUSH  (1u << 3)   // command bit, always reads 0
#define Z_AUDIO_CTRL_CLRUR  (1u << 4)   // command bit, always reads 0
#define Z_AUDIO_CTRL_MIXEN  (1u << 6)   // DAC reads the hardware mixer

#define Z_AUDIO_STATUS_LEVEL_MASK 0x0000FFFFu
#define Z_AUDIO_STATUS_EMPTY      (1u << 16)
#define Z_AUDIO_STATUS_FULL       (1u << 17)
#define Z_AUDIO_STATUS_BELOW      (1u << 18)
#define Z_AUDIO_STATUS_UNDERRUN   (1u << 19)

// CONFIG's format bits -- which DAC this board actually has pins for.
// Diagnostics only; playback code needs none of this.
#define Z_AUDIO_FORMAT_SD      (1u << 8)   // 1-bit sigma-delta stereo
#define Z_AUDIO_FORMAT_PT8211  (1u << 9)   // PT8211/TM8211
#define Z_AUDIO_MIXER_PRESENT  (1u << 12)  // hardware mixer is built
#define Z_AUDIO_MIXER_FMT16    (1u << 13)  // ...and its channels can
                                           // fetch 16-bit samples
#define Z_AUDIO_FORMAT_SPDIF   (1u << 10)  // optical S/PDIF (Sergei,
                                           // ULX3S)
#define Z_AUDIO_FORMAT_DDMI    (1u << 11)  // DDMI audio: also over the
                                           // DDMI connector, when
                                           // system.video.ddmi_audio is on
                                           // (docs/ddmi_audio.md)

/*
 * Sample rates, in Hz. Use these with z_audio_start_hz().
 *
 * On a bitstream with exact rates (z_audio_has_exact(), every board
 * since 0.0.7) each of these is EXACT: the sample clock is an
 * accumulator, not a divider, so 44100 Hz is 44100 Hz. On an older
 * bitstream z_audio_start_hz() picks the nearest of the old dividers
 * below -- 44117.6 Hz for 44100, 46875 Hz for 48000 -- and
 * z_audio_rate_hz() reports what was actually got. Code that asks
 * z_audio_rate_hz() for the real rate (every mixer STEP, every
 * resampler) is right on both without knowing which it has.
 */
#define Z_AUDIO_HZ_48K  48000u
#define Z_AUDIO_HZ_44K  44100u
#define Z_AUDIO_HZ_32K  32000u
#define Z_AUDIO_HZ_22K  22050u
#define Z_AUDIO_HZ_11K  11025u

/*
 * The OLD rate dividers: fs = CLKHZ / (64 * RATE). Still accepted by
 * z_audio_start(), which turns the four below into the exact rates
 * they always approximated (17 -> 44100, 16 -> 48000, 34 -> 22050,
 * 68 -> 11025); any other value is written to RATE as it always was.
 * New code should use z_audio_start_hz() and the Z_AUDIO_HZ_* values.
 *
 * 46875 Hz (RATE 16) was the S/PDIF boards' rate before exact rates:
 * the only one whose biphase half-cell was a whole number of sys_clk
 * cycles. With exact rates those boards run at a true 48000 instead.
 */
#define Z_AUDIO_RATE_44K  17   // 44117.6 Hz (44100 with exact rates)
#define Z_AUDIO_RATE_46K  16   // 46875.0 Hz (48000 with exact rates)
#define Z_AUDIO_RATE_22K  34   // 22058.8 Hz (22050 with exact rates)
#define Z_AUDIO_RATE_11K  68   // 11029.4 Hz (11025 with exact rates)


/*
 * True only if this bitstream actually has the audio block.
 *
 * Checks the CSR feature bit FIRST and only then the block's own
 * MAGIC, and that order is load-bearing rather than stylistic -- see
 * this file's header. Reading MAGIC first hangs the CPU on a bitstream
 * that predates rtl/audio.v.
 */
static inline bool z_audio_present(void) {
	if (!z_soc_has_feature(Z_FEATURE_AUDIO)) return false;
	return (reg_audio_magic == Z_AUDIO_MAGIC);
}

// FIFO depth in frames. Comes from the hardware rather than a constant
// here because rtl/boards.vh can override it per board.
static inline uint32_t z_audio_depth(void) {
	uint32_t cfg = reg_audio_config;
	if ((cfg >> 16) != Z_AUDIO_CONFIG_SIG) return 0;
	return 1u << (cfg & 0xFF);
}

// Which DAC is wired: Z_AUDIO_FORMAT_*. Zero if this bitstream's audio
// block predates the CONFIG register -- the signature check is what
// tells those apart from a working block reporting no DACs.
static inline uint32_t z_audio_formats(void) {
	uint32_t cfg = reg_audio_config;
	if ((cfg >> 16) != Z_AUDIO_CONFIG_SIG) return 0;
	return cfg & 0x0000FF00u;
}

// Does this bitstream have the system volume (VOLUME, CONFIG bit 14)?
// In the order that cannot hang: the block first, then its CONFIG
// signature, then the bit -- a block older than VOLUME reads the bit as
// 0, and one older than CONFIG fails the signature.
static inline bool z_audio_has_volume(void) {
	uint32_t cfg;
	if (!z_audio_present()) return false;
	cfg = reg_audio_config;
	return (cfg >> 16) == Z_AUDIO_CONFIG_SIG && (cfg & Z_AUDIO_CONFIG_HAS_VOLUME);
}

/*
 * Does this bitstream have exact sample rates (FSDIV, CONFIG bit 15)?
 * Same order as z_audio_has_volume(), for the same reason.
 */
static inline bool z_audio_has_exact(void) {
	uint32_t cfg;
	if (!z_audio_present()) return false;
	cfg = reg_audio_config;
	return (cfg >> 16) == Z_AUDIO_CONFIG_SIG && (cfg & Z_AUDIO_CONFIG_HAS_EXACT);
}

static inline uint32_t z_audio__clkhz(void) {
	uint32_t clk = reg_audio_clkhz;
	return clk ? clk : 48000000u;
}

/*
 * The sample rate the hardware is running at now, in Hz -- the true
 * one, whichever way it was set. Compute every mixer STEP and every
 * resampling ratio from this, never from a constant: the rate is
 * shared by every app and is whatever the last z_audio_start*() said.
 *
 * Rounded to the nearest Hz; with exact rates that is the rate itself.
 */
static inline uint32_t z_audio_rate_hz(void) {
	uint32_t clk = z_audio__clkhz();
	if (z_audio_has_exact()) {
		uint32_t v = reg_audio_fsdiv;
		uint32_t step = v & 0xFFFFu, mod = v >> 16;
		/* fs = (clk / 128) * step / mod, in 32 bits: whole part and
		 * remainder separately, since (clk/128) * step overflows. The
		 * remainder product is < mod * step < 2^32. */
		uint32_t base = clk >> 7;
		if (!mod || !step) return 0;
		if (step >= mod) return base;	/* ticks every cycle */
		return (base / mod) * step + ((base % mod) * step + mod / 2) / mod;
	} else {
		uint32_t r = reg_audio_rate & 0xFF;
		if (r < 2) r = 2;
		return (clk + 32u * r) / (64u * r);
	}
}

/* Greatest common divisor, for reducing FSDIV. */
static inline uint32_t z_audio__gcd(uint32_t a, uint32_t b) {
	while (b) { uint32_t t = a % b; a = b; b = t; }
	return a;
}

/*
 * Best approximation to p/q (p < q) with a denominator no larger than
 * maxd, by continued fractions: the last convergent that fits, or the
 * semiconvergent beyond it when that is closer. Returns the
 * denominator and sets *num. All in 32 bits: every term is checked
 * against maxd before it is formed.
 */
static inline uint32_t z_audio__approx(uint32_t p, uint32_t q, uint32_t maxd,
	uint32_t *num)
{
	uint32_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
	while (q) {
		uint32_t a = p / q, r = p % q, t;
		if (k1 && a > (maxd - k0) / k1) {
			/* next convergent is too big: try a semiconvergent */
			t = (maxd - k0) / k1;
			if (2 * t > a) {
				h1 = t * h1 + h0;
				k1 = t * k1 + k0;
			}
			break;
		}
		t = a * h1 + h0; h0 = h1; h1 = t;
		t = a * k1 + k0; k0 = k1; k1 = t;
		p = q; q = r;
	}
	*num = h1;
	return k1;
}

/*
 * The FSDIV word, { MOD[31:16], STEP[15:0] }, for `hz` at this
 * board's clock: STEP/MOD = 128 hz / clk, reduced by their common
 * divisor. Every standard rate reduces to MOD < 65536 and is then
 * exact (48000 -> 16/125, 44100 -> 147/1250). An unusual rate that
 * does not reduce that far gets the closest STEP/MOD that fits, which
 * is within 1e-5 (a hundredth of a cent) -- far below what an ear or a
 * receiver can tell. 0 for a rate outside 1 Hz .. clk/200 (240 kHz at
 * 48MHz), which is as fast as the PT8211 and S/PDIF stages are meant
 * to run.
 */
static inline uint32_t z_audio_fsdiv_for(uint32_t hz) {
	uint32_t clk = z_audio__clkhz();
	uint32_t num, g, step, mod;
	if (!hz || hz > clk / 200u) return 0;
	num = hz << 7;			/* < clk, so fits easily */
	g = z_audio__gcd(num, clk);
	step = num / g;
	mod = clk / g;
	if (mod > 0xFFFFu)
		mod = z_audio__approx(num, clk, 0xFFFFu, &step);
	if (!step) step = 1;
	return (mod << 16) | step;
}

/* The nearest old-style divider to `hz` (fs = clk / (64 * RATE)),
 * 2..255. What a bitstream without exact rates is given. */
static inline uint32_t z_audio_div_for(uint32_t hz) {
	uint32_t clk = z_audio__clkhz(), d;
	if (!hz) return 255;
	d = (clk + 32u * hz) / (64u * hz);
	if (d < 2) d = 2;
	if (d > 255) d = 255;
	return d;
}

/* IEC 60958 channel-status rate code (AES3 byte 3) for the standard
 * rate nearest `hz`: 0 = 44.1k, 2 = 48k, 3 = 32k, 4 = 22.05k,
 * 6 = 24k, 8 = 88.2k, 10 = 96k. */
static inline uint8_t z_audio_spdif_code(uint32_t hz) {
	if (hz < 23000) return 4;
	if (hz < 28000) return 6;
	if (hz < 38000) return 3;
	if (hz < 46000) return 0;
	if (hz < 68000) return 2;
	if (hz < 92000) return 8;
	return 10;
}

static inline uint32_t z_audio_level(void) {
	return reg_audio_status & Z_AUDIO_STATUS_LEVEL_MASK;
}

/*
 * Frames of free space, from ONE status read.
 *
 * z_audio_push() reads STATUS every call to check FULL, so a fill loop
 * costs two bus cycles per frame instead of one. At 22kHz that is
 * 44,000 MMIO reads a second spent asking a question whose answer the
 * caller could have worked out. Read the level once, push that many,
 * then ask again.
 */
static inline uint32_t z_audio_space(void) {
	uint32_t depth = z_audio_depth();
	uint32_t level = reg_audio_status & Z_AUDIO_STATUS_LEVEL_MASK;
	return (level >= depth) ? 0 : (depth - level);
}

/*
 * Push without checking FULL first. ONLY safe when the caller already
 * knows there is room -- i.e. inside a loop bounded by
 * z_audio_space(). A push to a full FIFO is dropped silently by the
 * hardware, so getting this wrong loses samples rather than failing.
 */
static inline void z_audio_push_unchecked(int16_t left, int16_t right) {
	reg_audio_data = (((uint32_t)(uint16_t)left) << 16)
		| ((uint32_t)(uint16_t)right);
}

static inline bool z_audio_full(void) {
	return (reg_audio_status & Z_AUDIO_STATUS_FULL) != 0;
}

static inline bool z_audio_empty(void) {
	return (reg_audio_status & Z_AUDIO_STATUS_EMPTY) != 0;
}

static inline bool z_audio_underrun(void) {
	return (reg_audio_status & Z_AUDIO_STATUS_UNDERRUN) != 0;
}

static inline void z_audio_clear_underrun(void) {
	reg_audio_ctrl = (reg_audio_ctrl & 0xFFu) | Z_AUDIO_CTRL_CLRUR;
}

/*
 * Push one stereo frame. Returns false if the FIFO was full, in which
 * case NOTHING WAS QUEUED -- the hardware drops the write rather than
 * stalling the bus, so a caller that ignores the return value silently
 * loses samples instead of hanging. Check it, or check z_audio_full()
 * first; do not do neither.
 */
static inline bool z_audio_push(int16_t left, int16_t right) {
	if (z_audio_full()) return false;
	reg_audio_data = (((uint32_t)(uint16_t)left) << 16)
		| ((uint32_t)(uint16_t)right);
	return true;
}

/*
 * DDMI audio's clock regeneration for the rate now running: the
 * display rebuilds 128 fs as 25.2 MHz * N / CTS. Exact: with the
 * sample clock at clk * STEP / (128 MOD) and each frame sent k times,
 * N / CTS = 40 k STEP / (21 MOD) at 48 MHz, reduced, then scaled to
 * the N sinks expect (6144 at 48 kHz, 6272 at 44.1 kHz, 4096 at 32
 * kHz), or near 128 fs / 1000 for any other rate. The same arithmetic
 * as rtl/audio_rates.vh, which gives the power-on values (always a
 * standard or old-divider rate, so always the exact branch).
 */
static inline void z_audio_ddmi_acr(uint32_t step, uint32_t mod, uint32_t k,
	uint32_t *n_out, uint32_t *cts_out)
{
	uint32_t clk = z_audio__clkhz();
	uint32_t n0 = k * step * (clk / 1200000u), c0 = mod * 21u, g, fs, std = 0, j;
	g = z_audio__gcd(n0, c0);
	n0 /= g;
	c0 /= g;
	/* fs on the DDMI side, and whether it is a whole number of Hz */
	{
		uint32_t base = clk >> 7, num_hi = (base / mod) * step * k;
		uint32_t rem = (base % mod) * step;		/* < mod * step < 2^32 */
		fs = num_hi + (rem / mod) * k + ((rem % mod) * k) / mod;
		if ((rem % mod) == 0) {
			switch (fs) {
			case 32000: std = 4096; break;
			case 44100: std = 6272; break;
			case 48000: std = 6144; break;
			case 88200: std = 12544; break;
			case 96000: std = 12288; break;
			case 176400: std = 25088; break;
			case 192000: std = 24576; break;
			}
		}
	}
	if (std && (std % n0) == 0) {
		j = std / n0;
	} else if (n0 > (fs * 128u) / 300u) {
		/* An unusual rate whose exact N is past what sinks take: the
		 * closest N/CTS with CTS at most 84000 -- 25.2 MHz / 300, so N
		 * stays inside the 128 fs / 300 a sink allows -- by continued
		 * fractions. Off by a few parts per million at worst
		 * (test_zaudio.c checks under 5) -- a source that measures CTS
		 * by counting, as most do, wobbles by 40 ppm. */
		uint32_t num, den = z_audio__approx(n0, c0, 84000u, &num);
		n0 = num;
		c0 = den;
		j = c0 ? (25200u + c0 / 2) / c0 : 1;
		if (!j) j = 1;
	} else {
		j = ((fs * 128u) / 1000u + n0 / 2) / n0;
		if (!j) j = 1;
	}
	*n_out = n0 * j;
	*cts_out = c0 * j;
}

/*
 * Keep DDMI audio's settings matching the rate the hardware runs at:
 * frames repeated 2^REP times so the display sees at least 32 kHz,
 * N and CTS, the channel-status rate code. Nothing to do on a
 * bitstream without DDMI sound. z_audio_set_rate_hz() calls this.
 */
static inline void z_audio__ddmi_sync(void) {
	uint32_t step, mod, hz, rep = 0, n, cts, v;
	if (!z_ddmi_sound_available()) return;
	if (z_audio_has_exact()) {
		v = reg_audio_fsdiv;
		step = v & 0xFFFFu;
		mod = v >> 16;
	} else {
		uint32_t r = reg_audio_rate & 0xFF;
		step = 128;
		mod = 64u * (r < 2 ? 2 : r);
	}
	if (!step || !mod) return;
	hz = z_audio_rate_hz();
	while (rep < 2 && (hz << rep) < 32000u) rep++;
	z_audio_ddmi_acr(step, mod, 1u << rep, &n, &cts);
	reg_socctl_acrn = n;
	reg_socctl_acrcts = cts;
	v = reg_socctl_ddmi & (Z_DDMI_EN | Z_DDMI_SOUND);
	reg_socctl_ddmi = v | ((uint32_t)z_audio_spdif_code(hz << rep) << Z_DDMI_FS_SHIFT)
		| (rep << Z_DDMI_REP_SHIFT);
}

/*
 * Change the sample rate WITHOUT touching the FIFO, the mute or the
 * mixer -- for an app that changes rate while it plays (track's rate
 * key). Everything z_audio_start_hz() says about the rate applies.
 */
static inline void z_audio_set_rate_hz(uint32_t hz) {
	/* RATE first: it resets FSDIV to the old-style divider, which is
	 * then what a bitstream without exact rates keeps, and what an
	 * older binary reading RATE sees -- close, rather than stale. */
	reg_audio_rate = z_audio_div_for(hz);
	if (z_audio_has_exact()) {
		uint32_t fsdiv = z_audio_fsdiv_for(hz);
		if (fsdiv) reg_audio_fsdiv = fsdiv;
	}
	if (z_audio_present() && (z_audio_formats() & Z_AUDIO_FORMAT_SPDIF))
		reg_audio_spdif_fs = z_audio_spdif_code(z_audio_rate_hz());
	if (z_audio_present() && (z_audio_formats() & Z_AUDIO_FORMAT_DDMI))
		z_audio__ddmi_sync();
}

/*
 * Start playback at `hz` (a Z_AUDIO_HZ_* value, or any whole-Hz rate).
 *
 * Exact on a bitstream with exact rates; the nearest old divider on
 * one without. Either way z_audio_rate_hz() afterwards says what the
 * hardware is really doing, and that is the number to compute steps
 * and resampling from.
 *
 * Flushes first, deliberately: whatever is in the FIFO from a previous
 * run is stale by definition, and starting by playing it produces a
 * short burst of the last thing before the new thing. Cheaper to
 * discard than to explain.
 *
 * Leaves the interrupt DISABLED. An app that wants it enables IRQEN
 * once it has a handler installed -- doing it here would fire the
 * interrupt immediately (the FIFO is empty, so it is below the
 * watermark) into whatever handler happened to be there.
 */
static inline void z_audio_start_hz(uint32_t hz) {
	reg_audio_ctrl = Z_AUDIO_CTRL_FLUSH;
	z_audio_set_rate_hz(hz);
	reg_audio_ctrl = Z_AUDIO_CTRL_EN | Z_AUDIO_CTRL_CLRUR;
}

/*
 * Start playback at an OLD rate divider (a Z_AUDIO_RATE_* value).
 *
 * Kept so code written before z_audio_start_hz() still builds and
 * still does the right thing: the four named dividers become the exact
 * rates they stood for, so an app asking for Z_AUDIO_RATE_44K gets
 * 44100 Hz. Anything else goes to RATE unchanged.
 */
static inline void z_audio_start(uint32_t rate_div) {
	uint32_t hz = 0;
	switch (rate_div) {
	case Z_AUDIO_RATE_44K: hz = Z_AUDIO_HZ_44K; break;
	case Z_AUDIO_RATE_46K: hz = Z_AUDIO_HZ_48K; break;
	case Z_AUDIO_RATE_22K: hz = Z_AUDIO_HZ_22K; break;
	case Z_AUDIO_RATE_11K: hz = Z_AUDIO_HZ_11K; break;
	}
	if (hz) {
		z_audio_start_hz(hz);
		return;
	}
	reg_audio_ctrl = Z_AUDIO_CTRL_FLUSH;
	reg_audio_rate = rate_div;
	reg_audio_ctrl = Z_AUDIO_CTRL_EN | Z_AUDIO_CTRL_CLRUR;
}

/*
 * Stop playback.
 *
 * EN=0 mutes; it does not stop the DAC's clocks. That is deliberate in
 * the hardware (rtl/audio_out.v) -- a PT8211 with a stopped bit clock
 * is in an undefined state rather than a quiet one -- so this is the
 * correct and complete way to go silent.
 */
static inline void z_audio_stop(void) {
	reg_audio_ctrl = Z_AUDIO_CTRL_FLUSH;
}

/*
 * Set the interrupt watermark, in frames. The interrupt asserts while
 * the queue is BELOW this. Half the FIFO depth is the sensible default
 * and is what the hardware resets to: it wakes you with half a buffer
 * still queued and half a buffer of room, which is the widest margin
 * available on both sides at once.
 */
static inline void z_audio_set_watermark(uint32_t frames) {
	reg_audio_wmark = frames;
}

static inline void z_audio_irq_enable(bool on) {
	uint32_t c = reg_audio_ctrl & 0xFFu;
	if (on) c |= Z_AUDIO_CTRL_IRQEN;
	else c &= ~Z_AUDIO_CTRL_IRQEN;
	reg_audio_ctrl = c;
}

/*
 * Swap left and right in the output stage.
 *
 * Exists because PT8211 WS polarity is the one thing in this subsystem
 * that was not verifiable against a datasheet with confidence, so it
 * is a register write to fix rather than a rebuild. If a board turns
 * out to need it set, the right fix is `AUDIO_CTRL_RESET in
 * rtl/boards.vh so it comes up correct, not a call to this from every
 * app.
 */
static inline void z_audio_swap_lr(bool on) {
	uint32_t c = reg_audio_ctrl & 0xFFu;
	if (on) c |= Z_AUDIO_CTRL_SWAPLR;
	else c &= ~Z_AUDIO_CTRL_SWAPLR;
	reg_audio_ctrl = c;
}

/*
 * Build a CH_CTRL word.
 *
 * `offset` is in units of 256 bytes and only takes effect with
 * `trig` -- which is exactly ProTracker's 9xx sample-offset effect,
 * and why the field is that shape.
 *
 * TRIG RESTARTS THE SAMPLE; EN WITHOUT TRIG DOES NOT. That distinction
 * is the one to get right: a tracker changes volume mid-note far more
 * often than it starts a note, and restarting on every volume change
 * is the classic way a hardware channel ends up sounding clicky.
 */
static inline uint32_t z_audio_ch_ctrl(uint8_t gain_l, uint8_t gain_r,
	bool enable, bool trig, uint8_t offset)
{
	return ((uint32_t)offset << 24)
		| (trig ? Z_AUDIO_CH_TRIG : 0u)
		| (enable ? Z_AUDIO_CH_EN : 0u)
		| ((uint32_t)gain_r << 8)
		| (uint32_t)gain_l;
}

/*
 * Phase step for a sample played at `sample_hz` on a DAC running at
 * `out_hz`, in the mixer's fixed point.
 *
 * Computed as (sample_hz << 14) / out_hz. sample_hz for a MOD comes
 * from the Amiga period: 3546895 / period, at most ~31.4kHz, so the
 * shifted numerator stays under 2^30 and this needs no 64-bit
 * arithmetic on a 32-bit core.
 */
static inline uint32_t z_audio_step(uint32_t sample_hz, uint32_t out_hz) {
	if (!out_hz) return 0;
	return (sample_hz << Z_AUDIO_STEP_FRAC) / out_hz;
}

/*
 * True if the hardware mixer is built into this bitstream.
 *
 * Reads a dedicated CONFIG bit. The first version of this checked
 * MIXSTAT instead, which is always readable and reads 0 both when the
 * mixer is absent AND when it is present with every channel idle -- so
 * it answered true unconditionally, and a build without `AUDIO_MIXER
 * announced "mixing in HARDWARE" and then played nothing.
 */
static inline bool z_audio_mixer_present(void) {
	uint32_t cfg = reg_audio_config;
	if ((cfg >> 16) != Z_AUDIO_CONFIG_SIG) return false;
	return (cfg & Z_AUDIO_MIXER_PRESENT) != 0;
}

/*
 * Channel `c`'s current position, in bytes from its CH_BASE.
 *
 * Two bus transactions, because the hardware stores the selection
 * rather than decoding eight addresses -- see rtl/audio_mixer.v's
 * pos_o comment for why that trade was made in favour of timing.
 *
 * pos_o is registered and therefore one cycle behind ch_pos, and two
 * cycles behind the select write. Neither is reachable from here: the
 * read below is a whole separate bus transaction after the write, and
 * the value describes a position that only advances once per audio
 * frame -- 1088 sys_clk apart at 44.1kHz. A one-cycle-old answer is
 * not a different answer.
 *
 * Returns 0 on a build with no mixer, which is indistinguishable from
 * a channel sitting at offset 0. Check z_audio_mixer_present() first
 * if that matters, exactly as for every other mixer register here.
 */
static inline uint32_t z_audio_ch_pos(int c) {
	reg_audio_mixpossel = (uint32_t)c;
	return reg_audio_mixpos;
}

/*
 * The same, as a whole number of bytes consumed.
 *
 * Named _bytes rather than _frames deliberately: the phase counts
 * BYTES, so for a stereo stream interleaved in one ring -- where both
 * channels share a base and step two bytes per frame -- this is twice
 * the frame count, and a helper that quietly picked one interpretation
 * would be wrong for the other.
 */
static inline uint32_t z_audio_ch_pos_bytes(int c) {
	return z_audio_ch_pos(c) >> Z_AUDIO_STEP_FRAC;
}

/*
 * True if this bitstream's mixer can fetch 16-bit samples.
 *
 * Separate from z_audio_mixer_present() and checked separately: a
 * bitstream can have the mixer and predate the 16-bit path, and the
 * failure mode of assuming otherwise is audible garbage rather than
 * silence. Same reasoning, same shape, as the mixer's own bit.
 */
static inline bool z_audio_mixer_fmt16(void) {
	uint32_t cfg = reg_audio_config;
	if ((cfg >> 16) != Z_AUDIO_CONFIG_SIG) return false;
	return (cfg & Z_AUDIO_MIXER_FMT16) != 0;
}

/*
 * CH_CTRL with the sample format chosen explicitly.
 *
 * z_audio_ch_ctrl() is unchanged and still builds an 8-bit channel --
 * every existing caller (sw/apps/track, sw/apps/mausrun,
 * sw/apps/audiotest) keeps working with no edit. This is the one to
 * call when you want 16 bits, and only after z_audio_mixer_fmt16()
 * says the hardware has it.
 */
static inline uint32_t z_audio_ch_ctrl_fmt(uint8_t gain_l, uint8_t gain_r,
	bool enable, bool trig, uint8_t offset, bool fmt16)
{
	return z_audio_ch_ctrl(gain_l, gain_r, enable, trig, offset)
		| (fmt16 ? Z_AUDIO_CH_FMT16 : 0u);
}

static inline void z_audio_mixer_enable(bool on) {
	uint32_t c = reg_audio_ctrl & 0xFFu;
	if (on) c |= Z_AUDIO_CTRL_MIXEN;
	else c &= ~Z_AUDIO_CTRL_MIXEN;
	reg_audio_ctrl = c;
}

/*
 * Lowest rate a digital output can carry: IEC 60958 receivers are
 * typically specified from 32kHz up, so 22.05 kHz and 11.025 kHz are
 * below what a DAC will lock to no matter how clean the line is. The
 * transmitter itself does not care and will happily send 11kHz; the
 * receiver on the other end is the constraint, and it is the one
 * thing the FPGA cannot check.
 */
#define Z_AUDIO_HZ_MIN_SPDIF 32000u

/*
 * Is `hz` a rate this board's outputs can all carry?
 *
 * On a board with only analogue outputs, anything the stage can make
 * is fine. With a digital transmitter, nothing below 32 kHz. Apps that
 * offer a rate control should filter through this; apps that just
 * play should use z_audio_preferred_hz().
 */
static inline bool z_audio_rate_ok_hz(uint32_t hz) {
	uint32_t clk = z_audio__clkhz();
	if (hz < 1000u || hz > clk / 200u) return false;
	if (z_audio_formats() & Z_AUDIO_FORMAT_SPDIF) {
		if (hz < Z_AUDIO_HZ_MIN_SPDIF) return false;
		/* Without exact rates the old rule applies too: an EVEN
		 * divider, because a bitstream from before the fractional
		 * S/PDIF divider truncated an odd one and ran the line 6%
		 * fast. Preferring even costs nothing and is right on both. */
		if (!z_audio_has_exact() && (z_audio_div_for(hz) & 1u))
			return false;
	}
	return true;
}

/*
 * The rate an app that just wants to play should start at: the rate
 * the hardware is running at now if every output can carry it,
 * otherwise this board's natural one -- 48 kHz with a digital output,
 * 44.1 kHz without.
 *
 * "Now" matters. The rate is shared by every app, and the board comes
 * up at the one its outputs want (`AUDIO_FS_RESET in rtl/boards.vh);
 * an app that overwrote it with a constant would throw that away, and
 * would retune whatever else is playing.
 */
static inline uint32_t z_audio_preferred_hz(void) {
	uint32_t hz = z_audio_rate_hz();
	if (z_audio_rate_ok_hz(hz)) return hz;
	/* 48 kHz with a digital output: exact with FSDIV, and divider 16
	 * (46875 Hz, even) on an older bitstream, as those boards always
	 * ran. */
	if (z_audio_formats() & Z_AUDIO_FORMAT_SPDIF)
		return Z_AUDIO_HZ_48K;
	return Z_AUDIO_HZ_44K;
}

/*
 * OLD: is the rate divider `rate_div` usable on this board? Kept for
 * code written against dividers; new code uses z_audio_rate_ok_hz().
 */
#define Z_AUDIO_RATE_MIN_SPDIF 23

static inline bool z_audio_rate_ok(uint32_t rate_div) {
	if (rate_div < 2) return false;
	if (z_audio_formats() & Z_AUDIO_FORMAT_SPDIF) {
		/* Below 32kHz no receiver will lock. */
		if (rate_div > Z_AUDIO_RATE_MIN_SPDIF) return false;
		/* And EVEN, because a half-cell is rate/2. The transmitter
		 * handles an odd rate with a fractional divider, but only in
		 * a bitstream new enough to have it -- an older one truncates
		 * and runs the line 6% fast, which is heard as static laid
		 * over the music. Preferring even costs nothing and is
		 * correct on both. */
		if (rate_div & 1u) return false;
	}
	return true;
}

#endif
