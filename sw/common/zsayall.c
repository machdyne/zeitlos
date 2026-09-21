/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading a document aloud, a line at a time. See zsayall.h.
 *
 * -- Marks --
 *
 * Every unit goes out with its own mark, from a counter that only
 * ever increases (skipping 0, which means "no mark"). Replies are
 * matched against the units still outstanding and anything else is
 * ignored -- in particular the CANCELLED replies for a previous read,
 * which arrive after a restart has already begun a new one. The
 * service answers every accepted mark exactly once and in order
 * (ztts.h), so the oldest outstanding unit is always the next to
 * finish.
 *
 * -- Stopping --
 *
 * Any CANCELLED for one of ours ends the read: something interrupted
 * the service (a Ctrl tap, another app's focus announcement, speech
 * turned off), and a reader that carried on regardless would talk
 * over it. The position is left at the last unit reported by at(),
 * which is the one the user was hearing.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zsoc.h"		// Z_TICK_HZ
#include "zspeak.h"
#include "zsayall.h"

// No line of text takes this long to say, at any rate.
#define SAYALL_TIMEOUT_TICKS	(30u * Z_TICK_HZ)

#ifndef Z_SAYALL_NOW
#define Z_SAYALL_NOW()	z_uptime_ticks()
#endif

static void finish(z_sayall_t *sa) {
	sa->active = false;
	sa->n_out = 0;
}

// Queue units until Z_SAYALL_AHEAD are outstanding or the text ends.
static void fill(z_sayall_t *sa, uint32_t first_flags) {

	while (sa->active && sa->n_out < Z_SAYALL_AHEAD) {

		const char *text = "";
		uint32_t flags = 0;
		int n = sa->get(sa->user, sa->next, &text, &flags);
		if (n < 0) break;

		if (++sa->seq == 0) sa->seq = 1;

		if (!z_speak_mark(text, (uint32_t)n, flags | first_flags, sa->seq)) {
			// Speech is off, or its mailbox is full. Either way there
			// is nobody to pace against.
			finish(sa);
			return;
		}
		first_flags = 0;

		sa->out_mark[sa->n_out] = sa->seq;
		sa->out_unit[sa->n_out] = sa->next;
		sa->n_out++;
		sa->next++;
		sa->deadline = Z_SAYALL_NOW() + SAYALL_TIMEOUT_TICKS;

	}

	if (sa->n_out == 0) finish(sa);	// ran off the end

}

bool z_sayall_start(z_sayall_t *sa, int from) {

	finish(sa);
	if (from < 0) from = 0;

	sa->active = true;
	sa->next = from;

	// Cut off whatever is being said: this is a new thing the user
	// asked for, not a continuation.
	fill(sa, Z_TTS_F_INTERRUPT);

	if (sa->active && sa->at) sa->at(sa->user, from);

	return sa->active;

}

void z_sayall_stop(z_sayall_t *sa) {
	if (!sa->active) return;
	finish(sa);
	z_speak_stop();
}

void z_sayall_toggle(z_sayall_t *sa, int from) {
	if (sa->active) z_sayall_stop(sa);
	else z_sayall_start(sa, from);
}

bool z_sayall_msg(z_sayall_t *sa, const z_msg_t *msg) {

	if (msg->subject != Z_TTS_MARK_DONE && msg->subject != Z_TTS_MARK_CANCELLED)
		return false;
	if (!sa->active) return false;

	uint16_t mark = (uint16_t)msg->tag;

	int i;
	for (i = 0; i < sa->n_out; i++)
		if (sa->out_mark[i] == mark) break;
	if (i == sa->n_out) return false;		// a previous read's, or not ours

	if (msg->subject == Z_TTS_MARK_CANCELLED) {
		finish(sa);
		return true;
	}

	// DONE. Always the oldest, but drop everything up to it in case a
	// reply was lost on the way.
	int drop = i + 1;
	for (int j = drop; j < sa->n_out; j++) {
		sa->out_mark[j - drop] = sa->out_mark[j];
		sa->out_unit[j - drop] = sa->out_unit[j];
	}
	sa->n_out -= drop;

	if (sa->n_out > 0 && sa->at) sa->at(sa->user, sa->out_unit[0]);

	fill(sa, 0);

	return true;

}

void z_sayall_poll(z_sayall_t *sa) {
	if (sa->active && (int32_t)(Z_SAYALL_NOW() - sa->deadline) > 0)
		finish(sa);
}

bool z_sayall_active(const z_sayall_t *sa) {
	return sa->active;
}
