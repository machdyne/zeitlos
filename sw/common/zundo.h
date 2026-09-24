#ifndef ZUNDO_H
#define ZUNDO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Undo and redo for an editor that keeps its document in one byte
 * buffer -- sw/apps/text first (docs/text_editor.md, "Undo"). Header-only
 * and free of any app's types: the editor tells it every edit as it
 * makes it, and hands it the buffer to put things back.
 *
 * -- memory --
 *
 * An app has 16KB for heap and stack together, so nothing here
 * allocates, and the document is never copied. What is kept is the
 * EDITS: a ring of records (where, how many bytes, insert or delete)
 * and a ring of the bytes each one inserted or deleted, both sized by
 * the caller. When either fills, the oldest edits are forgotten --
 * undo goes back as far as it can, not forever.
 *
 * -- groups --
 *
 * One Ctrl+Z undoes a GROUP: every record carries the group number the
 * editor gave it, and undo takes back all the records of the latest
 * group together. The editor decides what a group is -- a run of
 * typing, a paste that replaced a selection. Consecutive inserts in
 * the same group that continue each other are also merged into one
 * record as they come, so a typed word costs one record, not one a
 * letter.
 *
 * -- the saved point --
 *
 * z_undo_saved() marks "the file on disk looks like this"; after undo
 * or redo, z_undo_at_saved() says whether the document is back there.
 * Undo and redo stop at that point even inside a group, so it can
 * always be reached,
 * so the editor can clear its modified mark. If the edits that led to
 * the saved state are forgotten, it never is again, which is the
 * truth.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define Z_UNDO_INS  1
#define Z_UNDO_DEL  2

typedef struct {
	int32_t		pos;		// where in the document
	int32_t		n;			// bytes inserted or deleted
	uint32_t	log;		// where its bytes start in the byte ring (monotonic)
	int32_t		cursor;		// the caret before this edit
	uint16_t	group;
	uint8_t		op;			// Z_UNDO_INS / Z_UNDO_DEL
} z_undo_rec_t;

typedef struct {
	z_undo_rec_t	*rec;
	int				rec_cap;
	char			*bytes;
	uint32_t		bytes_cap;

	// Records [first, top) can be undone and [top, end) redone, as
	// monotonic counters; the ring index is (counter % rec_cap).
	uint32_t		first, top, end;

	// The byte ring: [bfirst, bend) holds the bytes of records
	// [first, end), as monotonic counters.
	uint32_t		bfirst, bend;

	// z_undo_saved()'s point, as a record counter; UINT32_MAX = none.
	uint32_t		saved;
} z_undo_t;

static inline void z_undo_init(z_undo_t *u, z_undo_rec_t *rec, int rec_cap,
	char *bytes, uint32_t bytes_cap) {
	memset(u, 0, sizeof(*u));
	u->rec = rec;
	u->rec_cap = rec_cap;
	u->bytes = bytes;
	u->bytes_cap = bytes_cap;
}

// Forgets everything -- a new or newly opened document -- and marks it
// saved.
static inline void z_undo_reset(z_undo_t *u) {
	u->first = u->top = u->end = 0;
	u->bfirst = u->bend = 0;
	u->saved = 0;
}

static inline void z_undo_saved(z_undo_t *u) { u->saved = u->top; }
static inline bool z_undo_at_saved(const z_undo_t *u) { return u->saved == u->top; }
static inline bool z_undo_can_undo(const z_undo_t *u) { return u->top > u->first; }
static inline bool z_undo_can_redo(const z_undo_t *u) { return u->end > u->top; }

static inline z_undo_rec_t *z_undo_at(const z_undo_t *u, uint32_t i) {
	return &u->rec[i % (uint32_t)u->rec_cap];
}

static inline void z_undo_bytes_put(z_undo_t *u, uint32_t at, const char *s, int32_t n) {
	for (int32_t i = 0; i < n; i++) u->bytes[(at + (uint32_t)i) % u->bytes_cap] = s[i];
}

static inline void z_undo_bytes_get(const z_undo_t *u, uint32_t at, char *d, int32_t n) {
	for (int32_t i = 0; i < n; i++) d[i] = u->bytes[(at + (uint32_t)i) % u->bytes_cap];
}

// Drops the oldest record.
static inline void z_undo_drop_oldest(z_undo_t *u) {
	z_undo_rec_t *r = z_undo_at(u, u->first);
	u->bfirst = r->log + (uint32_t)r->n;
	u->first++;
	if (u->top < u->first) u->top = u->first;
	if (u->saved != UINT32_MAX && u->saved < u->first) u->saved = UINT32_MAX;
}

// Records an edit about to be made (DEL: call before removing the bytes,
// so they can be read; INS: call with the bytes being inserted). `s` is
// the n bytes; `cursor` the caret before the edit. An edit too large for
// the byte ring is not recorded, and everything before it is forgotten:
// undo cannot step past it without corrupting the document.
static inline void z_undo_record(z_undo_t *u, uint8_t op, int32_t pos,
	const char *s, int32_t n, int32_t cursor, uint16_t group) {

	if (n <= 0) return;

	// A new edit ends the redo history.
	if (u->end > u->top) {
		if (u->saved != UINT32_MAX && u->saved > u->top) u->saved = UINT32_MAX;
		u->end = u->top;
		u->bend = (u->top > u->first) ? z_undo_at(u, u->top - 1)->log +
			(uint32_t)z_undo_at(u, u->top - 1)->n : u->bfirst;
	}

	if ((uint32_t)n > u->bytes_cap) {
		z_undo_reset(u);
		u->saved = UINT32_MAX;
		return;
	}

	// Typing on: extend the last insert rather than add a record.
	if (op == Z_UNDO_INS && u->top > u->first && u->saved != u->top) {
		z_undo_rec_t *p = z_undo_at(u, u->top - 1);
		if (p->op == Z_UNDO_INS && p->group == group && p->pos + p->n == pos &&
		    u->bend - u->bfirst + (uint32_t)n <= u->bytes_cap) {
			z_undo_bytes_put(u, u->bend, s, n);
			u->bend += (uint32_t)n;
			p->n += n;
			return;
		}
	}

	while (u->top - u->first >= (uint32_t)u->rec_cap ||
	       u->bend - u->bfirst + (uint32_t)n > u->bytes_cap)
		z_undo_drop_oldest(u);

	z_undo_rec_t *r = z_undo_at(u, u->top);
	r->op = op;
	r->pos = pos;
	r->n = n;
	r->log = u->bend;
	r->cursor = cursor;
	r->group = group;
	z_undo_bytes_put(u, u->bend, s, n);
	u->bend += (uint32_t)n;
	u->top++;
	u->end = u->top;

}

// Takes back the latest group. Applies it to buf[0..*len) (capacity
// cap), sets *cursor, and sets *from to the lowest offset it changed
// (so the editor can rewrap from there). Returns false if there was
// nothing to undo.
static inline bool z_undo_undo(z_undo_t *u, char *buf, int *len, int cap,
	int *cursor, int *from) {

	if (!z_undo_can_undo(u)) return false;

	uint16_t g = z_undo_at(u, u->top - 1)->group;
	int lo = *len;

	bool done = false;
	while (u->top > u->first && z_undo_at(u, u->top - 1)->group == g) {
		// A group never runs across the save: undo stops there, so the
		// document can always be brought back to what is on disk.
		if (done && u->top == u->saved) break;
		done = true;
		z_undo_rec_t *r = z_undo_at(u, u->top - 1);
		if (r->op == Z_UNDO_INS) {
			memmove(&buf[r->pos], &buf[r->pos + r->n], (size_t)(*len - r->pos - r->n));
			*len -= r->n;
		} else {
			if (*len + r->n > cap) break;	// cannot happen: it was there
			memmove(&buf[r->pos + r->n], &buf[r->pos], (size_t)(*len - r->pos));
			z_undo_bytes_get(u, r->log, &buf[r->pos], r->n);
			*len += r->n;
		}
		if (r->pos < lo) lo = r->pos;
		*cursor = r->cursor;
		u->top--;
	}

	*from = lo;
	return true;

}

// Puts back the group undo last took back. Same contract as z_undo_undo().
static inline bool z_undo_redo(z_undo_t *u, char *buf, int *len, int cap,
	int *cursor, int *from) {

	if (!z_undo_can_redo(u)) return false;

	uint16_t g = z_undo_at(u, u->top)->group;
	int lo = *len;

	bool done = false;
	while (u->top < u->end && z_undo_at(u, u->top)->group == g) {
		if (done && u->top == u->saved) break;	// as in z_undo_undo()
		done = true;
		z_undo_rec_t *r = z_undo_at(u, u->top);
		if (r->op == Z_UNDO_INS) {
			if (*len + r->n > cap) break;
			memmove(&buf[r->pos + r->n], &buf[r->pos], (size_t)(*len - r->pos));
			z_undo_bytes_get(u, r->log, &buf[r->pos], r->n);
			*len += r->n;
			*cursor = r->pos + r->n;
		} else {
			memmove(&buf[r->pos], &buf[r->pos + r->n], (size_t)(*len - r->pos - r->n));
			*len -= r->n;
			*cursor = r->pos;
		}
		if (r->pos < lo) lo = r->pos;
		u->top++;
	}

	*from = lo;
	return true;

}

#endif
