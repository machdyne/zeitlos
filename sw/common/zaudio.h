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
 *     z_audio_start(Z_AUDIO_RATE_44K);
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
#define Z_AUDIO_FORMAT_SPDIF   (1u << 10)  // RESERVED -- optical S/PDIF
                                           // (Sergei). Nothing sets
                                           // this yet; see docs/audio.md.

/*
 * Sample rate dividers. fs = CLKHZ / (64 * RATE), so these are exact
 * at 48MHz and nowhere else -- read reg_audio_clkhz and compute rather
 * than assuming, if you care to a Hz.
 *
 * 44117.6Hz is 0.04% above 44100, a 0.7-cent pitch error. Inaudible,
 * and it is what you get without a PLL: both of the ECP5's are already
 * used for the system and pixel clocks, so every audio clock here is
 * counted down from the 48MHz sys_clk.
 *
 * Z_AUDIO_RATE_46K is the odd one out and is here for a specific
 * future reason: 46875Hz is the ONLY rate on this clocking whose
 * S/PDIF biphase half-cell is an exact integer number of sys_clk
 * cycles (8). A board with the optical transmitter (Sergei, not
 * supported yet) will want it. For a tracker it costs nothing -- MOD
 * playback resamples through a phase accumulator, so the output rate
 * is a free parameter and pitch is unaffected. For playing back
 * material authored at 44.1kHz it is 6.3% sharp unless you resample.
 */
#define Z_AUDIO_RATE_44K  17   // 44117.6 Hz
#define Z_AUDIO_RATE_46K  16   // 46875.0 Hz -- see above
#define Z_AUDIO_RATE_22K  34   // 22058.8 Hz
#define Z_AUDIO_RATE_11K  68   // 11029.4 Hz


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

// Exact sample rate in Hz for the divider currently programmed.
static inline uint32_t z_audio_rate_hz(void) {
	uint32_t r = reg_audio_rate & 0xFF;
	if (r < 2) r = 2;
	return reg_audio_clkhz / (64u * r);
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
 * Start playback at the given rate divider (a Z_AUDIO_RATE_* value).
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
static inline void z_audio_start(uint32_t rate_div) {
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
 * every existing caller (sw/apps/track, sw/apps/gamedemo,
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
 * Lowest rate divider a digital output can carry.
 *
 * IEC 60958 receivers are typically specified from 32kHz up, so the
 * 22kHz and 11kHz settings above are below what a DAC will lock to no
 * matter how clean the line is -- 48e6/(64*RATE) has to stay at or
 * above 32000, which means RATE <= 23.
 *
 * The transmitter itself does not care and will happily send 11kHz.
 * The receiver on the other end is the constraint, and it is the one
 * thing the FPGA cannot check.
 */
#define Z_AUDIO_RATE_MIN_SPDIF 23

/*
 * Is `rate_div` usable on this board?
 *
 * On a board with only analogue outputs, any rate is fine. With a
 * transmitter, anything below 32kHz will not lock. Apps that offer a
 * rate control should filter through this rather than assuming, and
 * they should START from whatever rate the board came up with --
 * `AUDIO_RATE_RESET` in boards.vh exists so a board can pick a rate
 * its outputs can actually carry, and an app that overwrites it with a
 * constant throws that away.
 */
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
