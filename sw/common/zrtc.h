#ifndef ZRTC_H
#define ZRTC_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Wall-clock time: reading and setting the RTC (rtl/rtc.v), and
 * turning the seconds it holds into a date somebody can read. See
 * docs/rtc.md for the whole picture, including where the time
 * actually comes from (sw/apps/net's SNTP client, sw/apps/net/ntp.c).
 *
 * -- what this is NOT --
 *
 * z_uptime_ticks() (sw/common/zeitlos.h) is still the right thing for
 * "how long since X", and nothing here replaces it. That counter is
 * monotonic, costs a syscall-free read, and never jumps; this one has
 * an epoch and CAN jump, backwards even, the moment an NTP reply
 * lands. Timing a transfer with wall-clock time is how you end up with
 * a negative duration.
 *
 *   elapsed time, timeouts, retries  ->  z_uptime_ticks()
 *   dates, clocks, timestamps        ->  this file
 *
 * -- THE RTC IS OPTIONAL. CHECK FOR IT, AND CHECK THE RIGHT WAY --
 *
 * Read this before calling anything below from a new app.
 *
 * `RTC in rtl/boards.vh is defined at the universal level, so every
 * board has a clock by default -- but it can be turned off, and
 * bitstreams built before it existed have no clock either. Both cases
 * are real and software handles them the same way: don't use it.
 *
 * WHICH probe you use to find that out matters, though, because the
 * two cases behave differently at the hardware level:
 *
 *   `RTC off in this build -- sysctl.v hands 0x7000_03xx to csrs.v,
 *                     which acks it and reads back 0. Harmless: the
 *                     MAGIC check fails and everything below
 *                     correctly concludes there is no clock.
 *
 *   bitstream predates
 *   rtl/rtc.v, `ICACHE
 *   board            -- nothing decodes 0x7000_03xx, and an undecoded
 *                     address on this bus gets NO ACK AT ALL. The CPU
 *                     waits for it forever. That is a dead hang, not
 *                     a read of undefined data -- the same hazard
 *                     rtl/cache.v and z_icache_flush() (zsoc.h)
 *                     already document for the flush register.
 *
 * So z_rtc_present() is NOT a safe first probe on an arbitrary
 * bitstream: on a Lakritz running an older build it would hang before
 * it could return anything. Gate on the CSR feature bit instead,
 * which lives at 0x7000_0008 and is decoded by every bitstream ever
 * built:
 *
 *     if (!z_soc_has_feature(Z_FEATURE_RTC)) {
 *         // no clock on this bitstream -- either `RTC is off in
 *         // rtl/boards.vh, or this build predates rtl/rtc.v. Either
 *         // way it is a gateware change: `make flash`, not
 *         // `make dev-flash`.
 *     }
 *
 * z_rtc_available() below is exactly that check followed by the magic
 * check, in the safe order, and is what an app should actually call.
 * Everything else here assumes it has already returned true.
 *
 * -- header-only for the registers, zrtc.c for the calendar --
 *
 * The register accessors are inline MMIO, same as zsoc.h's, for the
 * same reason: they are plain loads and stores at a fixed physical
 * address and there is nothing for a kernel round trip to add. The
 * calendar conversions are not inline -- they are real code with a
 * table, and an app that only wants to read a timestamp shouldn't
 * carry them. Link sw/common/zrtc.c to use those; --gc-sections drops
 * them from anything that doesn't.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"

// -- registers -- see rtl/rtc.v's header for the authoritative map
#define reg_rtc_magic (*(volatile uint32_t*)0x70000300)
#define reg_rtc_sec   (*(volatile uint32_t*)0x70000304)
#define reg_rtc_sub   (*(volatile uint32_t*)0x70000308)
#define reg_rtc_ctrl  (*(volatile uint32_t*)0x7000030c)
#define reg_rtc_rate  (*(volatile uint32_t*)0x70000310)

// Sixteen bits of signed storage the hardware itself never consults,
// intended for a local-time offset in minutes east of UTC. NOTHING
// CURRENTLY READS OR WRITES IT -- Zeitlos is UTC throughout for now,
// see this file's "no timezones" note below. Kept mapped rather than
// removed so the slot is already in flashed gateware when a timezone
// story does arrive; adding accessors then is a software change alone.
#define reg_rtc_tz    (*(volatile int32_t*)0x70000314)

#define Z_RTC_MAGIC 0x5A525443u	// "ZRTC" -- see rtl/rtc.v

// CTRL bit 0. Set by a SEC write, cleared by reset or by writing it
// back as 0. Means "somebody has told this counter what time it is" --
// see rtl/rtc.v on why that is a separate fact from SEC being nonzero.
#define Z_RTC_CTRL_VALID (1u << 0)

// Sub-second ticks per second, as built. Read reg_rtc_rate() rather
// than assuming this, for anything doing arithmetic on a sub-second
// value -- this constant is here for sizing and for the one case
// where a compile-time value is genuinely needed.
#define Z_RTC_RATE_DEFAULT 1024u

// -- seconds per unit, for anything doing its own arithmetic --
#define Z_SECS_PER_MIN  60u
#define Z_SECS_PER_HOUR 3600u
#define Z_SECS_PER_DAY  86400u

// Safe on any bitstream: one built with `RTC, one built without it,
// and one predating rtl/rtc.v entirely -- see this file's header
// comment on why the order matters. Call this FIRST, once, and keep
// the answer; everything else here assumes it.
//
// The feature bit alone would nearly do, but the magic check is kept
// as well: the bit says "this bitstream was built with `RTC", the
// magic says "and the block is really answering at the address this
// header thinks it is". Reaching the magic check at all means the bit
// was set, which means the window is decoded, which means the read
// cannot hang. Together they also cover a bitstream whose CSR block is
// missing entirely, where z_soc_has_feature() already returns false
// and nothing is touched.
static inline bool z_rtc_available(void) {
	if (!z_soc_has_feature(Z_FEATURE_RTC)) return false;
	return reg_rtc_magic == Z_RTC_MAGIC;
}

// Whether the RTC block answers at all. Only call this once
// z_rtc_available() has returned true, or on a bitstream you know has
// the block -- see this file's header comment. On a build with `RTC
// off this is false and harmless; on an older `ICACHE build it hangs,
// which is the whole reason z_rtc_available() exists.
static inline bool z_rtc_present(void) {
	return reg_rtc_magic == Z_RTC_MAGIC;
}

// Has anything set the time since power-on? False means the counter
// is running but its epoch is meaningless -- show "--:--:--", not
// 1970. Not persistent across a power cycle; there is no battery.
static inline bool z_rtc_valid(void) {
	return (reg_rtc_ctrl & Z_RTC_CTRL_VALID) != 0;
}

static inline uint32_t z_rtc_rate(void) {
	return reg_rtc_rate;
}

// Reads seconds and the sub-second fraction as a consistent pair.
//
// The two registers are independent and live, so a naive read of one
// then the other can straddle a second boundary and produce a time
// that is up to a second wrong in either direction -- 12:00:00.999
// read as 12:00:01.999, say. Reading SEC either side of SUB and
// retrying on a change closes that: if both SEC reads agree, no
// rollover happened between them, so the SUB in the middle belongs to
// that second.
//
// The loop runs twice at most in practice (a rollover cannot happen
// twice in the handful of cycles this takes), but is written as a
// loop rather than a single retry because "at most twice" is an
// argument about timing, and a while is free.
//
// Either pointer may be NULL.
static inline void z_rtc_get(uint32_t *sec, uint32_t *sub) {

	uint32_t s0, s1, u;

	do {
		s0 = reg_rtc_sec;
		u = reg_rtc_sub;
		s1 = reg_rtc_sec;
	} while (s0 != s1);

	if (sec) *sec = s0;
	if (sub) *sub = u;

}

// Just the seconds, for the common case that doesn't care about the
// fraction. No retry needed -- a single register read is already
// self-consistent.
static inline uint32_t z_rtc_seconds(void) {
	return reg_rtc_sec;
}

// Sets the clock to `sec` seconds since the Unix epoch (UTC) plus
// `sub` sub-second ticks, atomically: the hardware adopts the
// fraction and restarts its prescaler as part of the same SEC write,
// so the new second begins exactly at the store. Also sets VALID.
//
// `sub` must be < z_rtc_rate(); anything larger is truncated by the
// register's width rather than rejected, so clamp before calling if
// the value came from arithmetic that could overflow.
//
// Write order matters and is why this is a function rather than two
// stores at the call site: SUB must be staged BEFORE SEC, because the
// SEC write is what consumes it.
static inline void z_rtc_set(uint32_t sec, uint32_t sub) {
	reg_rtc_sub = sub;
	reg_rtc_sec = sec;
}

// Marks the current time untrustworthy without changing it -- for a
// sync that came back with a timestamp that failed a sanity check,
// where "stop claiming to know" is more honest than either keeping the
// old value silently or writing a bad one.
static inline void z_rtc_invalidate(void) {
	reg_rtc_ctrl = 0;
}

// -- no timezones: everything here is UTC --
//
// The RTC counts UTC, NTP delivers UTC, z_time_to_tm() below breaks
// down whatever it is given without applying anything, and
// sw/apps/clock displays UTC with the word "UTC" on screen. There is
// no conversion layer and no local-time API.
//
// That is a decision, not an omission. A timezone offset is easy; the
// rules that produce the right offset are not, and there is no zone
// database on this system and nothing in NTP that carries one. So the
// choice was between a correct time honestly labelled and a plausible
// time that is silently an hour out for half the year, which is a
// worse thing to have on a wall clock than an unfamiliar one.
//
// reg_rtc_tz above is the storage for when this is revisited. It is
// deliberately not wrapped in accessors yet: a getter nothing calls
// and a setter nothing sets is exactly the API that rots into being
// wrong.

// -- calendar (sw/common/zrtc.c) --

typedef struct {
	int32_t		year;	// full year, e.g. 2026
	uint8_t		month;	// 1-12
	uint8_t		day;	// 1-31
	uint8_t		hour;	// 0-23
	uint8_t		min;	// 0-59
	uint8_t		sec;	// 0-59, never 60 -- no leap second handling
	uint8_t		wday;	// 0 = Sunday
	uint16_t	yday;	// 0-365
} z_tm_t;

// Breaks `t` (seconds since the Unix epoch) into calendar fields.
// Applies no offset of its own, so UTC seconds in means UTC fields
// out -- which is the only thing anything here feeds it.
//
// Proleptic Gregorian, no leap seconds, no zone rules. The range that
// works is 1970 through 2106, which is what an unsigned 32-bit second
// count spans.
void z_time_to_tm(uint32_t t, z_tm_t *tm);

// The inverse, for setting a clock from fields somebody typed. Reads
// year/month/day/hour/min/sec only; wday and yday are outputs of the
// other direction and are ignored here. Out-of-range fields are not
// validated -- month 13 rolls into the next year, which is
// occasionally useful and never checked.
uint32_t z_tm_to_time(const z_tm_t *tm);

// "Sun".."Sat" and "Jan".."Dec". Always return a valid string,
// "???" for an out-of-range input, so a caller can print the result
// without checking first.
const char *z_wday_name(uint8_t wday);
const char *z_month_name(uint8_t month);

#endif
