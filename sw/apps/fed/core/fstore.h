#ifndef FSTORE_H
#define FSTORE_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed's store: the objects a node holds, in arrival order, each at a
 * position. docs/fed.md, "Storage". One writer (fed), portable C99 on
 * sw/common/zplat.h; files opened briefly, one at a time.
 *
 *   <dir>/store.hdr          the epoch, the watermark (atomic rewrite)
 *   <dir>/seg/XXXXXXXX.log   records: u32 length, the object's bytes
 *   <dir>/seg/XXXXXXXX.idx   16-byte header; 12 bytes a record
 *   <dir>/ids.tbl            id -> position: open addressing
 *   <dir>/state.tbl          (topic, origin, key) -> the current object
 *   <dir>/cursors.txt        per peer: its epoch and our position in it
 *   <dir>/peers.txt          per peer: the slot its deliveries are marked with
 *   <dir>/consumers.txt      per local app: the position it has reached
 *
 * XXXXXXXX is a segment's first position, in hex. Positions start at 1.
 */
#include <stdint.h>
#include <stdbool.h>
#include "fobj.h"

#define FSTORE_SEG_BYTES   (1024u * 1024u)		// a new segment past 1 MB ...
#define FSTORE_SEG_SECONDS (30u * 86400u)		// ... or 30 days
#define FSTORE_MAX_SEGS    1024
#define FSTORE_RETAIN      (90u * 86400u)		// the default retention

// Retention for a topic, in seconds: the node's setting (docs/fed.md,
// "Retention"). NULL: FSTORE_RETAIN for everything.
typedef uint32_t (*fstore_retain_fn)(const char *topic);

enum {
	FSTORE_NEW = 1,			// stored
	FSTORE_HAVE = 2,		// already here: nothing to do
	FSTORE_STALE = 3,		// a state object older than the one we have
	FSTORE_EXPIRED = 4,		// already past its retention: not stored
};
enum { FSTORE_E_IO = 1, FSTORE_E_OBJ, FSTORE_E_FULL, FSTORE_E_CORRUPT };

// Opens the store in dir -- making it if it is not there -- and brings it
// up to date after a crash: records the index lacks are indexed, a torn
// record at the end is dropped, and everything past the watermark is
// applied to the id and state tables again. `ids_slots` is the id
// table's size for a new store (a power of two; 0 for the default).
int fstore_open(const char *dir, uint32_t now, fstore_retain_fn retain, uint32_t ids_slots);
void fstore_close(void);

uint64_t fstore_epoch(void);
uint32_t fstore_last(void);			// the newest position; 0 if empty
uint32_t fstore_first(void);		// the oldest position still held

// Stores a whole object (n bytes, one object: parsed and checked by the
// caller -- the signature is the caller's). FSTORE_NEW and its position
// in *pos, or FSTORE_HAVE / _STALE / _EXPIRED, or -error.
int fstore_put(const uint8_t *obj, uint32_t n, uint32_t now, uint32_t *pos);

// The same, marking the object as delivered by the peer in `slot`
// (fstore_peer_slot()); 0 is "made here".
int fstore_put_from(const uint8_t *obj, uint32_t n, uint32_t now, uint8_t slot, uint32_t *pos);

// A peer's slot, for its current epoch: 1..255, or 0 if none is free.
// A new epoch (the peer lost its store) gets a NEW slot, so what it
// delivered before is no longer assumed to be there.
uint8_t fstore_peer_slot(const uint8_t peer[32], uint64_t epoch);

bool fstore_have(const uint8_t id[32]);

// The current state object for (topic, origin, key), into buf[cap]: its
// size, 0 if there is none, -error.
int fstore_state_get(const char *topic, const uint8_t origin[32], const char *key, uint8_t *buf, uint32_t cap);

// The object at pos into buf[cap]: its size, 0 if the position is not
// held (expired, or never), -error.
int fstore_get(uint32_t pos, uint8_t *buf, uint32_t cap);

// Cancels (docs/fed.md, "Moderation"): an object by id -- its length,
// 0 if not held or cancelled; marking one cancelled -- 1, 0 if not held
// or already, <0 on error -- after which it is not delivered or relayed
// again, as a superseded state object is not; and asking.
int fstore_get_id(const uint8_t id[32], uint8_t *buf, uint32_t cap);
int fstore_cancel(const uint8_t id[32]);
bool fstore_cancelled(const uint8_t id[32]);

// The next position after `after` worth sending: held, and not a state
// object since superseded. 0 if there is none.
uint32_t fstore_next(uint32_t after);

// The same, also skipping what the peer in `slot` delivered: it has it.
// (slot 0: nothing skipped.) Without this, everything a peer sends comes
// back to it in the next session.
uint32_t fstore_next_for(uint32_t after, uint8_t slot);

// Makes everything stored so far durable and moves the watermark: after
// this, a crash loses nothing. Call before recording a cursor.
int fstore_sync(void);

// Drops segments whose log objects have all expired, current state
// objects in them first copied forward. The number dropped.
int fstore_expire(uint32_t now);

// A peer's epoch and how far we have pulled from it; false if none.
bool fstore_cursor(const uint8_t peer[32], uint64_t *epoch, uint32_t *pos);
int fstore_set_cursor(const uint8_t peer[32], uint64_t epoch, uint32_t pos);
// Every peer asked from the start next time: what was refused, it may
// take now (a new node list -- docs/fed.md, "Membership changes").
int fstore_clear_cursors(void);

// A local app's position (docs/fed.md, "The local interface").
uint32_t fstore_consumer(const char *name);
int fstore_set_consumer(const char *name, uint32_t pos);

#endif
