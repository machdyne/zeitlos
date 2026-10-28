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

typedef struct z_undo_s_ {
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

// -- applying undo and redo --
//
// Through two operations the editor supplies, so that it can apply them
// its own way -- sw/apps/text through its gap buffer, where each costs
// only the text between it and the last one rather than the whole
// document after it. `ins` puts n bytes, read with z_undo_bytes_get()
// from the log at `log`, at `pos`, and returns false if they do not fit;
// `del` removes n bytes at `pos`.
typedef struct {
	bool	(*ins)(void *user, const struct z_undo_s_ *u, int pos, uint32_t log, int n);
	void	(*del)(void *user, int pos, int n);
	void	*user;
} z_undo_ops_t;

// Takes back the latest group, sets *cursor, and sets *from to the
// lowest offset it changed (so the editor can rewrap from there).
// Returns false if there was nothing to undo.
static inline bool z_undo_undo_ops(z_undo_t *u, const z_undo_ops_t *o,
	int *cursor, int *from) {

	if (!z_undo_can_undo(u)) return false;

	uint16_t g = z_undo_at(u, u->top - 1)->group;
	int lo = 0x7FFFFFFF;

	bool done = false;
	while (u->top > u->first && z_undo_at(u, u->top - 1)->group == g) {
		// A group never runs across the save: undo stops there, so the
		// document can always be brought back to what is on disk.
		if (done && u->top == u->saved) break;
		done = true;
		// A copy, not a pointer: the ops may move the rings (text
		// keeps them in its arena, which growing the document can
		// move), and the record is read after they return.
		const z_undo_rec_t rec = *z_undo_at(u, u->top - 1), *r = &rec;
		if (r->op == Z_UNDO_INS) o->del(o->user, r->pos, r->n);
		else if (!o->ins(o->user, u, r->pos, r->log, r->n)) break;	// cannot happen: it was there
		if (r->pos < lo) lo = r->pos;
		*cursor = r->cursor;
		u->top--;
	}

	*from = lo == 0x7FFFFFFF ? 0 : lo;
	return true;

}

// Puts back the group undo last took back. Same contract.
static inline bool z_undo_redo_ops(z_undo_t *u, const z_undo_ops_t *o,
	int *cursor, int *from) {

	if (!z_undo_can_redo(u)) return false;

	uint16_t g = z_undo_at(u, u->top)->group;
	int lo = 0x7FFFFFFF;

	bool done = false;
	while (u->top < u->end && z_undo_at(u, u->top)->group == g) {
		if (done && u->top == u->saved) break;	// as in z_undo_undo_ops()
		done = true;
		const z_undo_rec_t rec = *z_undo_at(u, u->top), *r = &rec;	// as above
		if (r->op == Z_UNDO_INS) {
			if (!o->ins(o->user, u, r->pos, r->log, r->n)) break;
			*cursor = r->pos + r->n;
		} else {
			o->del(o->user, r->pos, r->n);
			*cursor = r->pos;
		}
		if (r->pos < lo) lo = r->pos;
		u->top++;
	}

	*from = lo == 0x7FFFFFFF ? 0 : lo;
	return true;

}

// The same, applied to a plain buffer buf[0..*len) of capacity cap.

typedef struct { char *buf; int *len; int cap; } z_undo_flat_t;

static inline bool z_undo_flat_ins(void *user, const struct z_undo_s_ *u,
	int pos, uint32_t log, int n) {
	z_undo_flat_t *f = (z_undo_flat_t *)user;
	if (*f->len + n > f->cap) return false;
	memmove(&f->buf[pos + n], &f->buf[pos], (size_t)(*f->len - pos));
	z_undo_bytes_get((const z_undo_t *)u, log, &f->buf[pos], n);
	*f->len += n;
	return true;
}

static inline void z_undo_flat_del(void *user, int pos, int n) {
	z_undo_flat_t *f = (z_undo_flat_t *)user;
	memmove(&f->buf[pos], &f->buf[pos + n], (size_t)(*f->len - pos - n));
	*f->len -= n;
}

static inline bool z_undo_undo(z_undo_t *u, char *buf, int *len, int cap,
	int *cursor, int *from) {
	z_undo_flat_t f = { buf, len, cap };
	z_undo_ops_t o = { z_undo_flat_ins, z_undo_flat_del, &f };
	return z_undo_undo_ops(u, &o, cursor, from);
}

static inline bool z_undo_redo(z_undo_t *u, char *buf, int *len, int cap,
	int *cursor, int *from) {
	z_undo_flat_t f = { buf, len, cap };
	z_undo_ops_t o = { z_undo_flat_ins, z_undo_flat_del, &f };
	return z_undo_redo_ops(u, &o, cursor, from);
}

#endif
