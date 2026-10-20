#ifndef SLIDE_ACT_H
#define SLIDE_ACT_H

/*
 * slide -- actions and the player. docs/slide_app.md, "Controls and
 * actions".
 *
 * An action is a few forms in Scheme notation, kept as text with the
 * object (or the card, for arriving at it):
 *
 *     (go 'next)  (go 'prev)  (go 'first)  (go 'last)  (go 'back)
 *     (go 3)  (go "results")
 *     (show "hint")  (hide "hint")  (toggle "hint")
 *     (set-text "answer" "Right!")  (set-value "volume" 50)
 *     (speak "Welcome")  (speak-text "answer")
 *     (run "clock")  (run "view /media/images/squirrel.jpg")
 *     (open "/data/slide/tour.zsl")
 *     (wait 1000)
 *
 * The C here does these verbs, so a deck of buttons never needs the
 * Scheme interpreter (Phase 4); the same text is valid Scheme, which
 * Phase 4 evaluates when a form is anything else. Until then anything
 * else says so in the status line and does nothing.
 *
 * (wait MS) does not block: the rest of the forms are kept and run
 * when act_tick() finds the time has come -- the main loop calls it.
 *
 * The player is what a click or a key does to the controls on a card in
 * browse mode, a show, and a deck run as an app: buttons, checks,
 * radios, sliders, lists and fields (typing into one).
 */

#include <stdint.h>
#include <stdbool.h>

#include "deck.h"

// What the app does for an action.
typedef struct {
	// To card `to` (an index, already within the deck).
	void	(*go)(void *u, int to);
	// The card's look changed (shown, hidden, a text or a value set).
	void	(*changed)(void *u);
	// Opens another deck.
	void	(*open_deck)(void *u, const char *path);
	// Something to say in the status line ("needs scripting").
	void	(*message)(void *u, const char *s);
	// Says it aloud (the speech service).
	void	(*speak)(void *u, const char *s);
	// Starts an app: "clock", "view /media/images/x.jpg".
	void	(*run)(void *u, const char *cmd);
	// Now, in Z_TICK_HZ ticks (the host's clock: the tests set it).
	uint32_t	(*now)(void *u);
	uint32_t	tick_hz;
	// Phase 4: forms that are not all these verbs go to Scheme
	// (script.h); NULL: they are refused with a message.
	void	(*eval)(void *u, deck_t *d, int cur, const char *forms);
	// A control was clicked (Scheme's (when-clicked ...) handlers),
	// before its own action runs.
	void	(*clicked)(void *u, deck_t *d, int cur, obj_t *o);
	// The card shown now (after a (go ...)).
	int		(*current)(void *u);
	void	*u;
} act_host_t;

void act_init(const act_host_t *h);
const act_host_t *act_host(void);

// Runs forms, in the context of card `cur` of `d`. A pending (wait)
// is dropped: the newest action wins.
void act_run(deck_t *d, int cur, const char *forms);

// Runs forms that are only the verbs above, now, without touching a
// pending (wait): what Scheme's (go ...) and the like call. False if
// one is not a verb here.
bool act_do(deck_t *d, int cur, const char *forms);

// Whether every form is one of the verbs (no Scheme needed).
bool act_simple(const char *forms);

// The app arrived at card `to` (any way: a key, a click, an action):
// remembered for (go 'back), and the card's own action run.
void act_arrived(deck_t *d, int to);

// Whether forms are waiting on a (wait); act_tick() runs them when due.
bool act_waiting(void);
void act_tick(void);

// Drops whatever is waiting and the history (another deck, leaving).
void act_reset(void);

// The object called `name` on card `cur` or its background, or NULL.
obj_t *act_find(deck_t *d, int cur, const char *name);

// An action as typed on the command line, in plain words, made into
// forms: "go next" -> (go 'next); "speak Hello there; go 3" ->
// (speak "Hello there") (go 3). Text that starts with "(" is taken as
// forms already. False (and why, in out) for a verb it does not know.
bool act_from_words(const char *in, char *out, int cap);

// -- the player ------------------------------------------------------------

typedef struct {
	deck_t	*d;
	int		cur;
	obj_t	*pressed;		// a button or check held down
	obj_t	*focus;			// the field being typed into
	obj_t	*drag;			// a slider being dragged
	bool	inside;			// the pointer is still over `pressed`
} player_t;

// The control at (x, y) units, topmost, visible: NULL if none.
obj_t *player_hit(player_t *p, int x, int y);

// A press, a move with the button down, a release (units). Each returns
// true when the card needs drawing again; *took says whether a control
// had the press (the app does its own thing otherwise: next card).
bool player_press(player_t *p, int x, int y, bool *took);
bool player_motion(player_t *p, int x, int y);
bool player_release(player_t *p, int x, int y);

// A key for the field being typed into. False if no field has the
// focus (the key is the app's); *redraw when the card needs drawing.
bool player_key(player_t *p, uint32_t key, bool *redraw);

// Forgets the press and the focus (another card, leaving).
void player_reset(player_t *p);

#endif
