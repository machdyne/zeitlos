#ifndef FSESS_H
#define FSESS_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A zfed session: the handshake, encrypted frames, and both sides
 * pulling from each other over one connection. docs/fed.md, "Peers and
 * sessions" and "Sessions, exactly". Portable C99, no malloc: the caller
 * owns an fsess_t (~45 KB) and moves bytes, as with the BBS core --
 *
 *   fsess_input()     bytes that arrived
 *   fsess_output()    bytes to send (then fsess_consumed())
 *   fsess_poll()      the time: handshake and idle limits
 *
 * It works on the store (fstore.h) the process has open.
 */
#include <stdint.h>
#include <stdbool.h>
#include "fobj.h"
#include "../../../common/zmlkem.h"

// The first two messages, in the clear (docs/fed.md, "Sessions, exactly")
#define FSESS_M0 (5 + 32 + 32 + ZMLKEM_EK_BYTES)		// 1253
#define FSESS_M0_WHO (5 + 32)							// enough to decide on the key
#define FSESS_M1 (5 + 32 + ZMLKEM_CT_BYTES)				// 1125

#define FSESS_FRAME_MAX   20480			// a frame's plaintext, at most
#define FSESS_WIRE_MAX    (2 + FSESS_FRAME_MAX + 16)
#define FSESS_WANTS_MAX   1024
#define FSESS_SHAKE_MS    30000			// to finish the handshake
#define FSESS_IDLE_MS     60000			// nothing arriving, after it

typedef struct {
	const uint8_t *secret_key;			// this node's Ed25519 key, Monocypher's 64-byte form
	// Initiator: the peer's key to expect (it must be this one).
	// Responder: NULL -- `allowed` decides.
	const uint8_t *peer;
	// Responder: may this key connect? Asked before any cryptography.
	bool (*allowed)(const uint8_t key[32], void *ctx);
	// May an object from this origin, on this topic, be stored? (the
	// networks' lists: docs/fed.md, "Networks")
	bool (*origin_ok)(const uint8_t origin[32], const char *topic, void *ctx);
	const char *wants;					// the patterns we want, '\n'-separated: "net/*\nfed/*"
	uint32_t now;						// Unix time, for objects' time check; 0 if unknown
	void *ctx;
	// "trusted relay" (docs/fed.md, D5): this peer's objects are stored
	// without checking their signatures -- every other check still runs.
	// The responder may set it from `allowed`, once it knows the key.
	bool trusted;
	// Told of each object this session stored new (a node list arriving
	// changes who is a member). May be NULL.
	void (*stored)(const fobj_t *o, void *ctx);
	// Asked of each object that passed every check, before it is stored:
	// false refuses it (one a cancel names -- docs/fed.md, "Moderation").
	// May be NULL.
	bool (*admit)(const fobj_t *o, void *ctx);
	// This link's limit (docs/fed.md, "Network profiles"): objects larger
	// are not sent over it, and refused from it -- node lists excepted,
	// which members need whatever their size. 0: none.
	uint32_t max_object;
} fsess_cfg_t;

enum { FS_RUNNING = 0, FS_DONE, FS_FAILED };

typedef struct {
	uint32_t got_new, got_have, got_rejected, sent;
	uint32_t withheld;				// not sent: larger than this link takes
} fsess_stats_t;

typedef struct {
	fsess_cfg_t cfg;
	bool initiator;
	int state;							// FS_*
	int phase;							// inside the handshake / the exchange
	char error[80];

	uint8_t peer_key[32];				// who, once known
	uint8_t eph_sk[32], eph_pk[32], peer_eph[32];
	uint8_t th[32];						// the transcript hash
	uint8_t key_out[32], key_in[32];
	uint64_t n_out, n_in;				// nonce counters
	uint8_t m0[FSESS_M0], m1[FSESS_M1];
	uint8_t kem_dk[ZMLKEM_DK_BYTES];	// the initiator's, until the reply
	uint8_t kem_ss[ZMLKEM_SS_BYTES];

	// the exchange
	uint64_t peer_epoch;
	uint8_t peer_slot;					// what the peer delivers is marked with it
	char peer_wants[FSESS_WANTS_MAX];
	uint32_t pull_from;					// our GET: after this, in the peer's store
	bool hello_sent, get_sent, got_end, bye_sent, got_bye;
	bool serving;						// answering the peer's GET
	uint32_t serve_pos, serve_end;
	bool end_sent;

	uint32_t last_in_ms, start_ms;
	bool timed;

	fsess_stats_t stats;

	uint8_t in[FSESS_WIRE_MAX];
	uint32_t in_len;
	uint8_t out[FSESS_WIRE_MAX + 512];
	uint32_t out_head, out_len;
} fsess_t;

// Starts a session. Initiator: cfg->peer is the key it must reach.
void fsess_init(fsess_t *s, const fsess_cfg_t *cfg, bool initiator);

void fsess_input(fsess_t *s, const uint8_t *d, uint32_t n);
uint32_t fsess_output(fsess_t *s, const uint8_t **p);
void fsess_consumed(fsess_t *s, uint32_t n);
void fsess_poll(fsess_t *s, uint32_t now_ms);

// The connection closed (by the peer, or broke). A session not yet
// finished has failed.
void fsess_closed(fsess_t *s);

// Does `topic` match one of the '\n'-separated patterns? A pattern is a
// topic, or a path ending "/*" for everything below it.
bool fsess_wanted(const char *patterns, const char *topic);

#endif
