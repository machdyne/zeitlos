#ifndef ZSPEAK_H
#define ZSPEAK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Making an app speak. Link zspeak.o.
 *
 *     z_speak("Save, button", Z_TTS_F_INTERRUPT);
 *
 * That is all most callers need. If speech is off -- the tts service
 * is not running -- the call returns false and costs about ten
 * instructions, so there is no reason to guard it with a setting of
 * your own. Nothing is ever reported to the user when speech is off;
 * silence IS the disabled state.
 *
 * -- What it costs --
 *
 * Absent, cache fresh:   a cycle-counter read and a compare.
 * Absent, cache stale:   one z_pid_lookup() table scan, at most twice
 *                        a second, and only when something speaks.
 * Present:               a copy of up to Z_SPEAK_SLOT_MAX bytes and
 *                        one z_msg_send() of a 24-byte envelope.
 * Never called:          nothing -- section GC drops it.
 *
 * Memory: Z_SPEAK_RING * Z_SPEAK_SLOT_MAX bytes of .bss, only in an
 * app that calls z_speak() or z_speak_mark().
 *
 * -- Why the text is copied --
 *
 * A Z_STR payload is BORROWED (docs/messaging.md): the service reads
 * the sender's bytes in place, later, whenever it next runs. A caller
 * speaking from a stack buffer would have returned and reused that
 * stack long before then. So z_speak() copies into a small static
 * ring and sends a pointer into the ring. A slot is only reused
 * Z_SPEAK_RING sends later, and the service copies on read and is
 * woken by every send -- the same "narrow the window to something
 * that does not happen" trade wm makes for Z_WM_WINDOW_MOVED.
 *
 * Text that is already long-lived -- a string literal, a static
 * buffer you will not rewrite -- can skip the copy with
 * z_speak_static(), which also has no length limit.
 *
 * -- Length --
 *
 * z_speak() truncates at Z_SPEAK_SLOT_MAX - 1 bytes. That is a line
 * of text, which is the unit anything reading a document should be
 * sending anyway (see Z_TTS_MARK_DONE in ztts.h for pacing a long
 * read). The service itself holds up to Z_TTS_UTTER_MAX per utterance.
 */

#include <stdint.h>
#include <stdbool.h>

#include "ztts.h"

#define Z_SPEAK_RING		4
#define Z_SPEAK_SLOT_MAX	128

// Speak `text`. flags: Z_TTS_F_* (ztts.h). Returns true if the
// request reached the service, false if speech is off or the
// service's mailbox is full. There is nothing useful to do about
// false; ignoring it is correct.
bool z_speak(const char *text, uint32_t flags);

// The same, for `len` bytes that need not be NUL-terminated -- a
// line out of a larger buffer.
bool z_speak_n(const char *text, uint32_t len, uint32_t flags);

// The same, and ask to be told when it has been spoken: the service
// sends Z_TTS_MARK_DONE or Z_TTS_MARK_CANCELLED with this mark back
// to the calling process. `mark` must be non-zero.
bool z_speak_mark(const char *text, uint32_t len, uint32_t flags, uint16_t mark);

// No copy: `text` must stay unchanged until the service has read it
// (a literal, or a static buffer). No length limit below
// Z_TTS_UTTER_MAX.
bool z_speak_static(const char *text, uint32_t flags);

// Stop speaking, discard the queue.
void z_speak_stop(void);

// Hold (lease_s > 0) or release (0) the narrator lease: while held,
// nobody else's speech is heard -- Z_TTS_NARRATE, ztts.h. Every
// z_speak*() from the holder renews it. Returns false if speech is off.
bool z_speak_narrate(uint32_t lease_s);

// Say the last thing again.
void z_speak_repeat(void);

// Change a voice setting (Z_TTS_PARAM_*, ztts.h).
void z_speak_set(uint32_t param, uint32_t value);

// Is speech on? Same cache as everything above, so as cheap. Useful
// only to skip building an expensive string, never to decide whether
// calling z_speak() is safe -- it always is.
bool z_speak_available(void);

// The pid the cache currently believes is the service, if it believes
// there is one. For wm, which has to tell "launch it" from "tell it
// to quit".
bool z_speak_service_pid(uint32_t *pid);

// Appends to a NUL-terminated buffer, never past its end. The small
// thing every caller building a spoken line needs, in one place rather
// than in each of them.
void z_speak_cat(char *buf, uint32_t size, const char *s);

// Appends a signed number, for the announcements that carry one
// ("slider, 40", "3 of 12").
void z_speak_num(char *buf, uint32_t size, int32_t v);

// Forget the cached lookup, so the next call asks the kernel again.
// Call after starting or stopping the service yourself.
void z_speak_forget(void);

#endif
