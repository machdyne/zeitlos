#ifndef ZRNG_H
#define ZRNG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Randomness, system-wide: a ChaCha20 CSPRNG seeded from rtl/trng.v.
 * See docs/trng.md for the hardware and the reasoning; this header is
 * the contract every consumer uses.
 *
 * -- Two questions, and they are not the same question --
 *
 *   z_rng_present()  -- is there a TRNG in this bitstream at all?
 *   z_rng_secure()   -- is it present, healthy, and did we seed from
 *                       it successfully?
 *
 * ALMOST NOTHING SHOULD CALL THE FIRST ONE. The second is what
 * determines whether the bytes coming out of here are fit to be a key,
 * and it can be false on a bitstream that does have the hardware --
 * a ring oscillator bank that a toolchain optimised away, or one that
 * has stopped oscillating, reports itself unhealthy and gets refused
 * here rather than quietly producing predictable output. That refusal
 * is the entire point of the health monitor existing.
 *
 * -- The generator always works. Its QUALITY is what varies --
 *
 * z_rng_bytes() never fails and never blocks. On a board with no
 * TRNG, or one whose TRNG is unhealthy, it falls back to seeding from
 * cycle-counter jitter and whatever else it can scrape together, and
 * sets z_rng_secure() false forever after. That fallback is genuinely
 * useful -- a game wants unpredictable-to-a-human, not
 * unpredictable-to-an-adversary, and forcing every caller to handle a
 * failure return would mean most of them handling it badly.
 *
 * So the rule is:
 *
 *   shuffling, games, jitter, backoff, ids  ->  just call z_rng_bytes()
 *   keys, nonces, anything an attacker sees ->  CHECK z_rng_secure()
 *                                               FIRST AND REFUSE
 *
 * sw/apps/net's SSH client is the second case and refuses to connect
 * when this is false. It does not warn and continue: a session key
 * derived from a cycle counter is not a degraded session, it is an
 * open one, and the user cannot tell the difference by looking.
 *
 * -- Why software generates every byte, when the hardware is right
 *    there --
 *
 * rtl/trng.v produces about 1,400 words per second. That is fine for a
 * seed and useless for anything else, and it is slow BY DESIGN --
 * sampling a ring oscillator quickly gets you lots of bits and very
 * little entropy. Raw oscillator output is also biased and
 * autocorrelated in ways that no amount of reading more of it fixes.
 *
 * So the hardware is treated as an entropy source, not a random number
 * generator: 256 bytes of it are hashed into a ChaCha20 key here, and
 * every actual byte the system uses comes from that at memory speed.
 * This is the same split Linux uses with RDSEED, and it is why
 * z_rng_bytes() can be called in a loop without thinking about it.
 *
 * The construction is fast key erasure: each call generates a fresh
 * key for the next call along with its output, and forgets the old
 * one. An attacker who reads this process's memory learns nothing
 * about bytes already handed out.
 *
 * -- Where to link it --
 *
 * The register accessors below are inline MMIO, same as zsoc.h's and
 * zrtc.h's, because they are plain loads and stores at a fixed
 * physical address. The generator is real code -- link sw/common/
 * zrng.c to use it; --gc-sections drops it from anything that only
 * wanted the presence check.
 */

#include "zsoc.h"

// -- registers (rtl/trng.v) --

#define reg_trng_magic  (*(volatile uint32_t*)0x70000400)
#define reg_trng_data   (*(volatile uint32_t*)0x70000404)
#define reg_trng_status (*(volatile uint32_t*)0x70000408)
#define reg_trng_ctrl   (*(volatile uint32_t*)0x7000040c)
#define reg_trng_rate   (*(volatile uint32_t*)0x70000410)
#define reg_trng_health (*(volatile uint32_t*)0x70000414)

#define Z_TRNG_MAGIC          0x5A524E47u	// "ZRNG"

#define Z_TRNG_STATUS_READY   (1u << 0)
#define Z_TRNG_STATUS_HEALTH  (1u << 1)
#define Z_TRNG_STATUS_ENABLED (1u << 2)
#define Z_TRNG_STATUS_LEVEL_SHIFT 4
#define Z_TRNG_STATUS_LEVEL_MASK  0xffu

#define Z_TRNG_CTRL_ENABLE    (1u << 0)
#define Z_TRNG_CTRL_CLEAR     (1u << 1)

// -- presence --

// Is rtl/trng.v in this bitstream?
//
// THE FEATURE BIT IS CHECKED FIRST AND THAT ORDER IS NOT OPTIONAL.
// Reading reg_trng_magic on a bitstream built before rtl/trng.v
// existed touches an address nothing decodes, and an undecoded address
// on this bus gets no ack at all -- the CPU waits for it forever.
// That is a dead hang, not a garbage read. The CSR feature register at
// 0x7000_0008 is decoded by every bitstream ever built, so it is the
// only safe thing to look at first. Same hazard z_rtc_available()
// (zrtc.h) and z_icache_flush() (zsoc.h) already document.
static inline bool z_rng_present(void) {
	if (!z_soc_has_feature(Z_FEATURE_TRNG)) return false;
	return reg_trng_magic == Z_TRNG_MAGIC;
}

// Has a health test ever failed since the last acknowledgement?
//
// Sticky in hardware on purpose: a source that was dead for a
// millisecond produced words that somebody may already be holding, and
// "it's fine now" is not an answer to that. Only z_rng_hw_clear()
// clears it.
static inline bool z_rng_hw_healthy(void) {
	if (!z_rng_present()) return false;
	return (reg_trng_status & Z_TRNG_STATUS_HEALTH) != 0;
}

// Acknowledge a health failure: clears the sticky bit AND discards
// every word the FIFO is holding, since anything collected while the
// source was suspect must not outlive the acknowledgement.
//
// Note the ENABLE bit is written too. A CTRL write always loads it, so
// writing Z_TRNG_CTRL_CLEAR alone would switch the oscillators off and
// nothing would ever arrive again -- see rtl/trng.v's register map,
// where this exact mistake is recorded because rtl/tb/tb_trng.v made
// it first.
static inline void z_rng_hw_clear(void) {
	if (!z_rng_present()) return;
	reg_trng_ctrl = Z_TRNG_CTRL_ENABLE | Z_TRNG_CTRL_CLEAR;
}

// One raw hardware word, non-blocking. Returns false if the FIFO is
// empty or the source is unhealthy.
//
// RAW MEANS RAW: debiased, but not conditioned, and its bits are not
// independent. This is entropy to be hashed, NOT a random number. If
// you are reaching for this instead of z_rng_bytes() to "get better
// randomness", you have it backwards -- z_rng_bytes() is the output of
// hashing a great deal of this.
static inline bool z_rng_hw_word(uint32_t *out) {
	if (!z_rng_present()) return false;
	if ((reg_trng_status & (Z_TRNG_STATUS_READY | Z_TRNG_STATUS_HEALTH))
		!= (Z_TRNG_STATUS_READY | Z_TRNG_STATUS_HEALTH)) return false;
	*out = reg_trng_data;
	return true;
}

// Approximate words per second this source delivers, worst case. Read
// from the hardware rather than assumed, so software sizing a wait
// keeps working if rtl/trng.v's SAMPLE_DIV is ever retuned.
static inline uint32_t z_rng_hw_rate(void) {
	if (!z_rng_present()) return 0;
	return reg_trng_rate;
}

// -- the generator (sw/common/zrng.c) --

// Fill `buf` with `len` random bytes. Never fails, never blocks.
// Seeds itself on first use. See this header's opening comment on when
// the result is fit to be a key and when it merely isn't guessable by
// a person.
void z_rng_bytes(void *buf, uint32_t len);

// One uniformly random 32-bit word.
uint32_t z_rng_u32(void);

// A uniformly random value in [0, n). Rejection sampled, so it is
// genuinely uniform rather than `% n` with its bias toward small
// values -- which matters more often than people expect and is free to
// get right. Returns 0 for n == 0.
uint32_t z_rng_below(uint32_t n);

// True if this generator was seeded from a present, healthy hardware
// source. Anything producing a key, nonce, or session identifier must
// check this and refuse rather than degrade -- see the opening
// comment.
//
// Seeds on first call if it hasn't happened yet, so this is safe to
// call as the very first thing an app does.
bool z_rng_secure(void);

// Reseed from the hardware now, discarding the current key.
//
// Called automatically after 1MiB of output; the explicit version is
// for a caller that has a specific reason to want forward secrecy at a
// known point (a long-lived process about to generate a key, say).
// Harmless to call on a board with no TRNG -- it just re-runs the
// fallback and z_rng_secure() stays false.
void z_rng_reseed(void);

// Mix caller-supplied bytes into the pool. Cannot make the generator
// worse, whatever is passed in -- absorption is one-way -- so there is
// no requirement that `data` be unpredictable or of any particular
// length.
//
// For sources of real but unmeasurable entropy that only one caller
// can see: keystroke timings, packet arrival jitter, mouse movement,
// SD card response latency. NEVER counts toward z_rng_secure(),
// because nothing here can tell how much entropy a caller's bytes
// actually carried, and guessing generously is how systems end up
// believing they are seeded when they are not.
void z_rng_stir(const void *data, uint32_t len);

// Cheap per-event entropy: samples the cycle counter and folds it into
// an internal pool, absorbing into the generator only once every 32
// calls.
//
// Separate from z_rng_stir() because the call sites are hot. A packet
// arriving or a key going down is a good entropy event -- the TIME it
// happened is genuinely unpredictable, being a person's finger or
// another machine's scheduler -- but calling z_rng_stir() from those
// paths would run a full ChaCha20 block every time. This is a load,
// an XOR and a compare in the common case.
//
// Like z_rng_stir(), this NEVER makes z_rng_secure() true. It improves
// an already-seeded pool; it does not substitute for seeding one. Its
// real value is on a board with no TRNG, and in making two boards with
// identical hardware diverge.
void z_rng_stir_event(void);

#endif
