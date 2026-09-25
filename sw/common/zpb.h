#ifndef ZPB_H
#define ZPB_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Protocol Buffers, the wire format and nothing else.
 *
 * Written from the public description of the encoding
 * (protobuf.dev, "Encoding"). No schema compiler, no generated code, no
 * reflection, no allocation: a reader is a cursor over a byte range that
 * hands back one field at a time, and a writer appends fields to a
 * caller's fixed buffer. What a field MEANS is the caller's business --
 * it switches on the field number it gets back.
 *
 * First user: sw/apps/mesh (docs/mesh_app.md), which speaks the
 * Meshtastic client API. That schema is GPL-3.0, and doing it this way
 * is what keeps it out of the tree: mesh needs field numbers, which are
 * interoperability facts, not the .proto files or anything generated
 * from them.
 *
 * -- the wire, briefly --
 *
 * A message is a run of fields. Each starts with a varint tag,
 * (field_number << 3) | wire_type:
 *
 *   0  VARINT  int32, int64, uint32, uint64, sint32/64 (zigzag), bool, enum
 *   1  I64     fixed64, sfixed64, double
 *   2  LEN     string, bytes, a nested message, a packed repeated field
 *   5  I32     fixed32, sfixed32, float
 *
 * (3 and 4 are the long-deprecated groups; a message containing one is
 * treated as malformed.)
 *
 * -- two things that bite --
 *
 * A NEGATIVE int32 is encoded as the 64-bit two's complement, so it is
 * ten bytes long, not five. The reader takes up to ten bytes for every
 * varint and truncates; stopping at five would desynchronise the rest
 * of the message on the first negative number (Meshtastic's rx_rssi is
 * always one). zpb_put_int32() writes it the same way, as the spec
 * requires.
 *
 * sint32 is NOT int32: it is zigzag-encoded so that small negatives are
 * short. The schema says which; zpb_sint32() and zpb_put_sint32() are
 * for those fields.
 *
 * -- errors --
 *
 * Everything is bounds-checked. A reader that meets a truncated field, a
 * varint longer than ten bytes, a group, a field number 0, or a LEN
 * running past the end stops: zpb_next() returns false and r->err is
 * set. A writer that runs out of room stops writing and sets w->err;
 * check it once at the end rather than after every put.
 */

#include <stdint.h>
#include <stdbool.h>

#define ZPB_VARINT	0
#define ZPB_I64		1
#define ZPB_LEN		2
#define ZPB_I32		5

// -- reading --

typedef struct {
	const uint8_t *p;
	const uint8_t *end;
	bool err;
} zpb_rd_t;

typedef struct {
	uint32_t num;		// field number
	uint8_t wt;			// wire type, ZPB_*
	uint64_t v;			// VARINT, I64 and I32 values
	const uint8_t *ptr;	// LEN: the bytes, inside the reader's buffer
	uint32_t len;		// LEN: how many
} zpb_field_t;

void zpb_rd_init(zpb_rd_t *r, const void *buf, uint32_t len);

// The next field into *f. false at the end of the message -- or on a
// malformed one, in which case r->err is also set.
bool zpb_next(zpb_rd_t *r, zpb_field_t *f);

// A reader over a LEN field's bytes: a nested message.
void zpb_sub(zpb_rd_t *sub, const zpb_field_t *f);

// A VARINT field as the schema's type. Each takes the low bits, as the
// spec says a parser must for a value wider than its field.
static inline uint32_t zpb_u32(const zpb_field_t *f) { return (uint32_t)f->v; }
static inline int32_t zpb_i32(const zpb_field_t *f) { return (int32_t)(uint32_t)f->v; }
static inline bool zpb_bool(const zpb_field_t *f) { return f->v != 0; }
static inline int32_t zpb_sint32(const zpb_field_t *f) {
	uint32_t u = (uint32_t)f->v;
	return (int32_t)((u >> 1) ^ (0u - (u & 1u)));
}

// An I32 field's bits as a float. (sfixed32 is zpb_i32(); fixed32 is
// zpb_u32().)
float zpb_float(const zpb_field_t *f);

// -- writing --

typedef struct {
	uint8_t *start;
	uint8_t *p;
	uint8_t *end;
	bool err;
} zpb_wr_t;

void zpb_wr_init(zpb_wr_t *w, void *buf, uint32_t cap);
static inline uint32_t zpb_wr_len(const zpb_wr_t *w) { return (uint32_t)(w->p - w->start); }

// Each writes one whole field, tag included.
void zpb_put_varint(zpb_wr_t *w, uint32_t num, uint64_t v);
void zpb_put_int32(zpb_wr_t *w, uint32_t num, int32_t v);	// ten bytes if negative
void zpb_put_sint32(zpb_wr_t *w, uint32_t num, int32_t v);	// zigzag
void zpb_put_fixed32(zpb_wr_t *w, uint32_t num, uint32_t v);
void zpb_put_float(zpb_wr_t *w, uint32_t num, float v);
void zpb_put_bytes(zpb_wr_t *w, uint32_t num, const void *data, uint32_t len);
void zpb_put_str(zpb_wr_t *w, uint32_t num, const char *s);

// A nested message that was built in its own writer: zpb_put_bytes() of
// its contents. (sub's error, if any, carries over.)
void zpb_put_msg(zpb_wr_t *w, uint32_t num, const zpb_wr_t *sub);

#endif
