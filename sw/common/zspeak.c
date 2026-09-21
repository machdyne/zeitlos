/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Making an app speak. See zspeak.h for the contract and costs, and
 * docs/tts.md for the design.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zeitlos.h"
#include "zsoc.h"		// Z_SYSCLK_HZ
#include "zcycles.h"
#include "zspeak.h"

// -- the lookup cache --
//
// Both answers are cached, "present at pid N" and "absent", for half
// a second of wall-clock cycles. The absent case is the one that has
// to be cheap -- it is what every app pays, on every focus change,
// on a machine where nobody turned speech on -- and it is: a rdcycle,
// two loads, a subtract and a compare.
//
// rdcycle rather than z_uptime_ticks(): the latter is a syscall. The
// counter wraps every ~89s at 48MHz, and the unsigned subtraction
// below is correct across the wrap. A value that looks older than the
// window because it is more than one wrap old just triggers a lookup,
// which is the safe direction.
//
// Why a pid can go stale, and why that is tolerable: the service may
// exit, and its pid may be reused, inside the window. A send to a dead
// pid fails and we forget at once (below). A send to a REUSED pid
// lands on an unrelated process with a subject in 0x5454xxxx, which no
// other process has a case for (ztts.h). Bounded to half a second, and
// only after someone just turned speech off.

#ifndef Z_SPEAK_NOW
#define Z_SPEAK_NOW()		z_cycles()
#endif

#ifndef Z_SPEAK_CACHE_CYCLES
#define Z_SPEAK_CACHE_CYCLES	(Z_SYSCLK_HZ / 2)
#endif

#define ZS_UNKNOWN	0
#define ZS_PRESENT	1
#define ZS_ABSENT	2

static uint8_t zs_state = ZS_UNKNOWN;
static uint32_t zs_pid;
static uint32_t zs_when;

// The lookup, out of line: it is the rare path, and keeping it out of
// zs_resolve() is what lets the common path compile to a handful of
// instructions with no stack frame in the caller.
__attribute__((noinline))
static bool zs_lookup(uint32_t now) {

	uint32_t pid;
	if (z_pid_lookup(Z_TTS_SERVICE, &pid)) {
		zs_pid = pid;
		zs_state = ZS_PRESENT;
	} else {
		zs_state = ZS_ABSENT;
	}
	zs_when = now;

	return zs_state == ZS_PRESENT;

}

__attribute__((always_inline))
static inline bool zs_resolve(void) {

	uint32_t now = Z_SPEAK_NOW();

	if (zs_state != ZS_UNKNOWN && (now - zs_when) < Z_SPEAK_CACHE_CYCLES)
		return zs_state == ZS_PRESENT;

	return zs_lookup(now);

}

void z_speak_forget(void) {
	zs_state = ZS_UNKNOWN;
}

bool z_speak_available(void) {
	return zs_resolve();
}

bool z_speak_service_pid(uint32_t *pid) {
	if (!zs_resolve()) return false;
	if (pid) *pid = zs_pid;
	return true;
}

// One send. On failure the cache is dropped rather than trusted for
// the rest of its window: the likeliest cause is that the service has
// just exited, and the next call should find that out, not repeat the
// failure for half a second. (A merely full mailbox also lands here;
// the lookup that follows finds the service again and costs one scan.)
static bool zs_send(uint32_t subject, uint32_t tag, z_obj_t obj) {

	if (!zs_resolve()) return false;

	if (z_msg_new_send(zs_pid, subject, tag, obj) != Z_OK) {
		zs_state = ZS_UNKNOWN;
		return false;
	}

	return true;

}

// -- the text ring --
//
// See zspeak.h, "Why the text is copied".
static char zs_ring[Z_SPEAK_RING][Z_SPEAK_SLOT_MAX];
static uint8_t zs_next;

// Everything after the "is anyone listening" check, out of line for
// the same reason as zs_lookup(): so the public entry points below
// test the cache and return without ever building a stack frame when
// speech is off.
__attribute__((noinline))
static bool zs_say_copy(const char *text, uint32_t len, uint32_t flags, uint16_t mark) {

	if (!text) return false;

	// Reduced on use as well as on update, so the index cannot reach
	// outside the ring whatever it starts as.
	char *slot = zs_ring[zs_next % Z_SPEAK_RING];
	zs_next = (uint8_t)((zs_next + 1) % Z_SPEAK_RING);

	uint32_t i;
	for (i = 0; i < len && i < Z_SPEAK_SLOT_MAX - 1 && text[i]; i++)
		slot[i] = text[i];
	slot[i] = 0;

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = slot;

	return zs_send(Z_TTS_SAY, Z_TTS_TAG(flags, mark), obj);

}

// The public entry points are written so that every exit is either a
// plain return or a TAIL call. That is what lets the compiler emit
// them with no stack frame: with speech off, z_speak() is a rdcycle,
// three loads, a subtract, two compares and a return. An ordinary call
// to zs_lookup() in there -- which is how this was first written --
// made gcc build the frame up front for every caller, spoken to or
// not, doubling the cost of the common case. Check the disassembly
// (`make` leaves a .dasm) if this is ever restructured.
#define ZS_FRESH() \
	(zs_state != ZS_UNKNOWN && (Z_SPEAK_NOW() - zs_when) < Z_SPEAK_CACHE_CYCLES)

__attribute__((noinline))
static bool zs_say_slow(const char *text, uint32_t len, uint32_t flags, uint16_t mark) {
	if (!zs_lookup(Z_SPEAK_NOW())) return false;
	return zs_say_copy(text, len, flags, mark);
}

// The check comes FIRST. With speech off nothing else runs: no copy,
// no strlen of a caller's possibly long string.
#define ZS_SAY(text, len, flags, mark) do { \
	if (ZS_FRESH()) { \
		if (zs_state != ZS_PRESENT) return false; \
		return zs_say_copy(text, len, flags, mark); \
	} \
	return zs_say_slow(text, len, flags, mark); \
} while (0)

bool z_speak_mark(const char *text, uint32_t len, uint32_t flags, uint16_t mark) {
	ZS_SAY(text, len, flags, mark);
}

bool z_speak_n(const char *text, uint32_t len, uint32_t flags) {
	ZS_SAY(text, len, flags, 0);
}

bool z_speak(const char *text, uint32_t flags) {
	// UINT32_MAX: bounded by the slot and by the NUL instead.
	ZS_SAY(text, UINT32_MAX, flags, 0);
}

bool z_speak_static(const char *text, uint32_t flags) {

	if (!zs_resolve()) return false;
	if (!text) return false;

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = (char *)text;

	return zs_send(Z_TTS_SAY, Z_TTS_TAG(flags, 0), obj);

}

void z_speak_stop(void) {
	zs_send(Z_TTS_STOP, 0, z_obj_none());
}

void z_speak_repeat(void) {
	zs_send(Z_TTS_REPEAT, 0, z_obj_none());
}

void z_speak_set(uint32_t param, uint32_t value) {
	zs_send(Z_TTS_SET, 0, z_obj_uint32(Z_TTS_SET_PACK(param, value)));
}

// -- building a line to say --

void z_speak_cat(char *buf, uint32_t size, const char *s) {
	if (!buf || !size || !s) return;
	uint32_t n = 0;
	while (n + 1 < size && buf[n]) n++;
	while (*s && n + 1 < size) buf[n++] = *s++;
	buf[n] = 0;
}

void z_speak_num(char *buf, uint32_t size, int32_t v) {

	char tmp[12];
	uint32_t n = 0;
	uint32_t u;

	if (v < 0) {
		z_speak_cat(buf, size, "-");
		u = (uint32_t)(-(int64_t)v);
	} else {
		u = (uint32_t)v;
	}

	do {
		tmp[n++] = (char)('0' + (u % 10));
		u /= 10;
	} while (u && n < sizeof(tmp) - 1);

	char out[13];
	uint32_t o = 0;
	while (n) out[o++] = tmp[--n];
	out[o] = 0;

	z_speak_cat(buf, size, out);

}
