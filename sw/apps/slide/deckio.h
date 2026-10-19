#ifndef SLIDE_DECKIO_H
#define SLIDE_DECKIO_H

/*
 * slide -- the .zsl file: a deck as S-expressions (docs/slide_app.md,
 * "The file"). Read as DATA by this reader, never evaluated, so opening
 * a deck runs nothing. The same notation carries cards for undo and
 * objects through the clipboard.
 */

#include <stdint.h>
#include <stdbool.h>

#include "deck.h"

// A growing text buffer in the pool.
typedef struct {
	char		*buf;
	uint32_t	len, cap;
	bool		oom;
} sbuf_t;

void sb_init(sbuf_t *b);
void sb_free(sbuf_t *b);
void sb_put(sbuf_t *b, const char *s, uint32_t n);
void sb_puts(sbuf_t *b, const char *s);
void sb_int(sbuf_t *b, long v);
void sb_str(sbuf_t *b, const char *s);		// quoted, escaped

// The whole deck. False if memory ran out (b->oom).
bool deckio_write(sbuf_t *b, const deck_t *d);
// One card (or background), as `(card ...)`.
bool deckio_write_card(sbuf_t *b, const deck_t *d, const card_t *c, bool is_bg);
// Objects, as `(objects ...)`, for the clipboard.
bool deckio_write_objs(sbuf_t *b, const deck_t *d, obj_t *const *o, int n);

// NULL on a malformed file, with a reason ("line 12: ...") in err.
deck_t *deckio_read(const char *s, uint32_t len, char *err, int errcap);
// A `(card ...)` written by deckio_write_card(), fonts resolved against d.
card_t *deckio_read_card(deck_t *d, const char *s, uint32_t len);
// `(objects ...)` into a card, on top; returns how many were added, -1
// if the text is not objects.
int deckio_read_objs(deck_t *d, card_t *into, const char *s, uint32_t len);

#endif
