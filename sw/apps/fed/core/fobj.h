#ifndef FOBJ_H
#define FOBJ_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed objects: make, parse, sign, check, name. docs/fed.md, "Objects"
 * and "Encoding, exactly". Portable C99; no malloc.
 *
 *   ZFED1\n
 *   topic: <topic>\n
 *   type: <type>\n
 *   format: json | text | bytes\n
 *   kind: log | state\n
 *   origin: <64 lowercase hex: the Ed25519 public key>\n
 *   time: <decimal>\n
 *   seq: <decimal>\n
 *   key: <key>\n                    (state objects only)
 *   len: <decimal>\n
 *   \n
 *   <len bytes of payload>
 *   \nsig: <128 lowercase hex>\n
 *
 * The id is SHA-256 of everything from the magic through the payload.
 * The signature is Ed25519 over "zfed object\0" and the id: 44 bytes.
 */
#include <stdint.h>
#include <stdbool.h>

#define FOBJ_MAGIC        "ZFED1"
#define FOBJ_HEADER_MAX   1024			// the magic through the blank line
#define FOBJ_PAYLOAD_MAX  16384
#define FOBJ_SIGLINE      135			// "\nsig: " + 128 hex + "\n"
#define FOBJ_MAX          (FOBJ_HEADER_MAX + FOBJ_PAYLOAD_MAX + FOBJ_SIGLINE)
#define FOBJ_TOPIC_MAX    96
#define FOBJ_TYPE_MAX     64
#define FOBJ_KEY_MAX      64
#define FOBJ_INT_MAX      9007199254740992ULL		// 2^53
#define FOBJ_FUTURE       86400			// seconds ahead of our clock allowed

enum { FOBJ_JSON = 1, FOBJ_TEXT, FOBJ_BYTES };
enum { FOBJ_LOG = 1, FOBJ_STATE };

typedef struct {
	char topic[FOBJ_TOPIC_MAX + 1];
	char type[FOBJ_TYPE_MAX + 1];
	char key[FOBJ_KEY_MAX + 1];		// state objects; "" for log
	uint8_t format, kind;			// FOBJ_JSON .., FOBJ_LOG ..
	uint8_t origin[32];
	uint64_t time, seq;
	uint32_t len;
	const uint8_t *payload;			// parse: into the caller's buffer
	uint8_t id[32];
	uint8_t sig[64];
	uint32_t size;					// the whole object, signature line included
} fobj_t;

enum {
	FOBJ_OK = 0,
	FOBJ_E_MAGIC, FOBJ_E_LINE, FOBJ_E_FIELD, FOBJ_E_TOPIC, FOBJ_E_TYPE, FOBJ_E_FORMAT,
	FOBJ_E_KIND, FOBJ_E_ORIGIN, FOBJ_E_NUMBER, FOBJ_E_FUTURE, FOBJ_E_SEQ, FOBJ_E_KEY,
	FOBJ_E_LEN, FOBJ_E_HEADER, FOBJ_E_SHORT, FOBJ_E_SIGLINE, FOBJ_E_PAYLOAD, FOBJ_E_SIG,
	FOBJ_E_SPACE,
};

// Parses and checks every rule except the signature (fobj_verify()).
// `now`: this node's clock, 0 if it has none. On success o->size is the
// object's length: the buffer may hold more after it.
int fobj_parse(const uint8_t *buf, uint32_t n, uint64_t now, fobj_t *o, uint32_t *where);

// The signature: 0 if origin signed this id, -FOBJ_E_SIG if not.
int fobj_verify(const fobj_t *o);

// Makes an object from o's fields and `payload` (o->len bytes), signed
// with `secret_key` (Monocypher's 64-byte form), into out[cap]. Fills
// o->id, o->sig, o->size and o->origin (from the key). Returns the size,
// or -(error) if a field breaks a rule or it does not fit.
int fobj_make(fobj_t *o, const uint8_t *payload, const uint8_t secret_key[64], uint8_t *out, uint32_t cap);

const char *fobj_strerror(int err);
void fobj_hex(const uint8_t *b, uint32_t n, char *out);			// lowercase, NUL-terminated
void fobj_short_id(const uint8_t public_key[32], char out[17]);	// a node's short id

// 64-bit numbers as text, by hand. printf and scanf are never asked to
// do it: not every C library a Zeitlos program may be built with passes
// 64-bit arguments the same way, and a board built with such a one
// wrote garbled cursors (docs/fed.md, "Building it").
int fobj_u64_dec(char out[21], uint64_t v);			// digits, NUL; its length
void fobj_u64_hex(char out[17], uint64_t v);		// exactly 16 lowercase hex, NUL
bool fobj_hex_u64(const char *s, uint64_t *v);		// exactly 16 hex digits
bool fobj_dec_u64(const char *s, uint64_t *v);		// 1-20 digits, then anything not a digit

#endif
