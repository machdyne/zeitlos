#ifndef SLIDE_SCRIPT_H
#define SLIDE_SCRIPT_H

/*
 * slide -- Scheme in a deck (Phase 4). docs/slide_scripting.md.
 *
 * Machdyne Scheme (sw/ext/ms) with the whole Zeitlos API
 * (sw/common/zapi) and slide's own procedures: (go 'next), (show
 * "hint"), (get-text "answer"), (after 500 ...), (every 1000 ...),
 * (when-clicked "ok" ...). The interpreter starts the first time a deck
 * needs it -- an action that is more than act.h's verbs, a script, the
 * message box -- so a deck without scripts never pays for it.
 *
 * Where scripts live in a deck:
 *   the deck's script   run when the deck starts being used (browse,
 *                       a show, an app): definitions, (when-clicked ...)
 *   a card's script     run on arriving at the card, after its action:
 *                       its timers ((every ...)) stop when it is left
 *   an action           Scheme forms, as before -- run on a click
 *
 * Esc stops a script that runs too long (ms's step hook); an error is
 * said in the status line, with the object or card it came from.
 */

#include <stdbool.h>
#include <stdint.h>

#include "deck.h"
#include "act.h"

// What scripts do to the app goes through the host the verbs use
// (act.h, act_host()).

// Asked every few thousand evaluations: true stops the script (the app
// looks for Esc without blocking, keeping other messages for later).
void script_stop_check(bool (*fn)(void));

// Whether Scheme is running (started by the first script), and starting
// it. False if it could not start (no memory).
bool script_ready(void);
bool script_start(void);

// Evaluates forms in the context of card `cur` of `d`: actions, a
// card's script, the message box. `out` (if not NULL) gets the last
// value printed, or the error. False on an error, or if stopped.
bool script_eval(deck_t *d, int cur, const char *src, char *out, int cap);

// The deck starts being used: its handlers and timers cleared, its own
// script run. And arriving at a card: timers of the card left are
// cancelled, the card's script run.
void script_deck(deck_t *d, int cur);
void script_card(deck_t *d, int cur);

// A control was clicked: its (when-clicked ...) handler, if any.
void script_clicked(deck_t *d, int cur, obj_t *o);

// Timers: whether any are waiting, and running those due (main loop).
bool script_timers(void);
void script_tick(void);

// Stops every timer (leaving a show, the deck closed, Esc).
void script_stop_timers(void);

// Reads src without evaluating it: false, and the error, if it is not
// a sequence of whole forms (the script editor's check).
bool script_check(const char *src, char *err, int cap);

// For a Scheme-made window's messages (zapi.h): true if the message
// was one of theirs and is dealt with.
bool script_window_msg(void *msg);

#endif
