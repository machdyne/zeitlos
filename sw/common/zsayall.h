#ifndef ZSAYALL_H
#define ZSAYALL_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading a document aloud, a line at a time. Link zsayall.o and
 * zspeak.o. See docs/tts.md, "Reading a window".
 *
 * The app supplies two callbacks -- "give me unit n" and "unit n is
 * being spoken now" -- and routes three things here: Super+A
 * (Z_WM_READ), the service's Z_TTS_MARK_* replies, and "the user did
 * something" (any key, a click). This file does the rest: keeps
 * Z_SAYALL_AHEAD units queued at the service so there is no gap
 * between lines, advances when one is spoken, and stops when the text
 * runs out, when the user interrupts, or when a reply never comes.
 *
 * A "unit" is whatever the app finds natural: a display line in an
 * editor, a line of a terminal screen. It must fit in a zspeak ring
 * slot (Z_SPEAK_SLOT_MAX - 1 bytes); longer is truncated. A unit that
 * continues into the next -- a wrapped line mid-sentence -- should
 * carry Z_TTS_F_CONTINUES, so the voice does not pause and fall in
 * pitch as if a sentence had ended.
 *
 *     static z_sayall_t sa = { get_line, at_line, NULL };
 *
 *     case Z_WM_READ:           z_sayall_toggle(&sa, first_line); break;
 *     case Z_TTS_MARK_DONE:
 *     case Z_TTS_MARK_CANCELLED: z_sayall_msg(&sa, &msg); break;
 *     case Z_WM_KEY:            z_sayall_stop(&sa); ...
 *
 * and z_sayall_poll(&sa) now and then, for the timeout.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zmsg.h"

#define Z_SAYALL_AHEAD	2

typedef struct {

	// Unit `n`: set *text and *flags (Z_TTS_F_* -- normally 0 or
	// Z_TTS_F_CONTINUES) and return its length in bytes, which may be
	// 0 for a blank line. Return -1 past the end.
	int (*get)(void *user, int n, const char **text, uint32_t *flags);

	// Unit `n` is the one being spoken now. Move the caret or scroll
	// to it, so the view follows the voice and, when reading stops,
	// the position is where the user stopped listening. May be NULL.
	void (*at)(void *user, int n);

	void *user;

	// -- private --
	bool		active;
	uint16_t	seq;
	int			n_out;
	uint16_t	out_mark[Z_SAYALL_AHEAD];
	int			out_unit[Z_SAYALL_AHEAD];
	int			next;
	uint32_t	deadline;

} z_sayall_t;

// Start reading at unit `from`. Interrupts whatever is being said.
// Returns false if there was nothing to read or speech is off.
bool z_sayall_start(z_sayall_t *sa, int from);

// Stop reading, and stop the voice. A no-op if not reading, so it is
// safe to call on every keystroke.
void z_sayall_stop(z_sayall_t *sa);

// Super+A: start if stopped, stop if reading.
void z_sayall_toggle(z_sayall_t *sa, int from);

// Feed every Z_TTS_MARK_DONE / Z_TTS_MARK_CANCELLED here. Returns
// true if it was one of ours.
bool z_sayall_msg(z_sayall_t *sa, const z_msg_t *msg);

// Gives up if the service has gone quiet (a lost reply, or speech
// turned off mid-read). Call from the app's loop; cheap.
void z_sayall_poll(z_sayall_t *sa);

bool z_sayall_active(const z_sayall_t *sa);

#endif
