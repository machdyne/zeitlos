#ifndef SSH_WIRE_H
#define SSH_WIRE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SSH wire encoding (RFC 4251 section 5): the byte/uint32/string/mpint
 * types every SSH message is built out of, plus bounds-checked readers
 * and writers for them.
 *
 * -- Why readers and writers are cursor objects and not macros --
 *
 * Every field in an SSH packet is length-prefixed and attacker
 * controlled. A parser written as pointer arithmetic gets exactly one
 * missing bounds check away from reading off the end of a buffer that
 * a remote server chose the contents of, and the SSH handshake happens
 * BEFORE the host key is verified -- so that server is not yet known
 * to be the one we meant to talk to.
 *
 * So reads go through a cursor that carries its own limit and latches
 * a `bad` flag on the first overrun. Nothing after that point can
 * produce data, and callers check the flag once at the end rather than
 * after every field. That shape makes the safe way also the least
 * typing, which is the only kind of safety convention that survives.
 *
 * The writers work the same way for the same reason in reverse: an
 * overlong field silently truncating into a fixed buffer would produce
 * a packet the server rejects for reasons nobody could diagnose.
 *
 * -- Borrowed strings --
 *
 * ssh_rd_string() does NOT copy. It hands back a pointer into the
 * packet buffer and a length, valid only until that buffer is reused
 * -- the same borrowed-data convention tcp.h's TCP_EVENT_DATA and
 * docs/messaging.md already use throughout this codebase. Copy what
 * you need to keep.
 *
 * SSH strings are byte strings, NOT C strings: they carry an explicit
 * length and may contain NUL. Never pass one to strcmp(). Use
 * ssh_str_eq() below, which compares against a C literal with the
 * length taken into account.
 */

// -- reader --

typedef struct {
	const uint8_t *buf;
	uint32_t len;
	uint32_t pos;
	bool bad;			// sticky: set by the first overrun, never cleared
} ssh_rd;

void ssh_rd_init(ssh_rd *r, const void *buf, uint32_t len);

uint8_t  ssh_rd_u8(ssh_rd *r);
uint32_t ssh_rd_u32(ssh_rd *r);
bool     ssh_rd_bool(ssh_rd *r);

// Borrowed pointer into the reader's buffer -- see the header comment.
// On overrun sets `bad`, and returns NULL with *len set to 0.
const uint8_t *ssh_rd_string(ssh_rd *r, uint32_t *len);

// Skips a length-prefixed string without looking at it. For the
// name-lists in KEXINIT we do not negotiate on, and for fields of
// messages we intend to ignore.
void ssh_rd_skip_string(ssh_rd *r);

// Skips `n` raw bytes.
void ssh_rd_skip(ssh_rd *r, uint32_t n);

// Copies a fixed-size blob out, e.g. a 32-byte key. Returns false (and
// sets `bad`) if fewer than `n` bytes remain.
bool ssh_rd_bytes(ssh_rd *r, void *out, uint32_t n);

static inline uint32_t ssh_rd_left(const ssh_rd *r) {
	return r->bad ? 0 : (r->len - r->pos);
}

// True if a borrowed SSH string equals a C literal. The length check
// comes first, so this is also what stops a server sending
// "ssh-ed25519\0evil" from matching.
bool ssh_str_eq(const uint8_t *s, uint32_t len, const char *lit);

// True if `lit` appears as a comma-separated element of an SSH
// name-list (RFC 4251 section 5). Used to check that a server's
// KEXINIT actually offers what we require -- we do not implement
// preference ordering, because we support exactly one algorithm per
// slot, so the only question is "is it in the list at all".
bool ssh_namelist_has(const uint8_t *list, uint32_t len, const char *lit);

// -- writer --

typedef struct {
	uint8_t *buf;
	uint32_t cap;
	uint32_t pos;
	bool bad;			// sticky: set by the first overflow
} ssh_wr;

void ssh_wr_init(ssh_wr *w, void *buf, uint32_t cap);

void ssh_wr_u8(ssh_wr *w, uint8_t v);
void ssh_wr_u32(ssh_wr *w, uint32_t v);
void ssh_wr_bool(ssh_wr *w, bool v);
void ssh_wr_bytes(ssh_wr *w, const void *data, uint32_t len);
void ssh_wr_string(ssh_wr *w, const void *data, uint32_t len);
void ssh_wr_cstr(ssh_wr *w, const char *s);

// mpint (RFC 4251 section 5): a big-endian two's complement integer,
// with leading zero bytes stripped, and a single leading 0x00 added
// back if the top bit of the first byte would otherwise be set.
//
// BOTH HALVES OF THAT MATTER AND BOTH ARE EASY TO GET WRONG. The
// shared secret K from X25519 is a 32-byte value that is uniformly
// random, so about half the time its first byte has the high bit set
// (needing the extra 0x00) and about 1 time in 256 its first byte is
// zero (needing a byte stripped). An implementation that skips either
// case works most of the time and produces a wrong exchange hash
// otherwise -- which shows up as a signature that fails to verify
// against a server that is behaving perfectly, roughly one connection
// in two. See ssh_wr_mpint_test in the host tests.
void ssh_wr_mpint(ssh_wr *w, const uint8_t *be, uint32_t len);

static inline uint32_t ssh_wr_len(const ssh_wr *w) { return w->pos; }
static inline bool ssh_wr_ok(const ssh_wr *w) { return !w->bad; }

#endif
