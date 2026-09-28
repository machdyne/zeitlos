#ifndef FNODE_H
#define FNODE_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A zfed node, portable: its configuration, who it talks to, when it
 * polls its peers, how it limits strangers, its sessions, and the local
 * interface apps use. docs/fed.md, "The local interface" and
 * "Configuration". The platform (linux/main.c; Zeitlos next) owns the
 * sockets and moves bytes; this decides everything else.
 */
#include <stdint.h>
#include <stdbool.h>
#include "fsess.h"

#define FNODE_PEERS     16
#define FNODE_MEMBERS   64
#define FNODE_BLOCKED   16
#define FNODE_RETAIN    8
#define FNODE_SESSIONS  3			// two in, one out (docs/fed.md, "The board's limits")
#define FNODE_CLIENTS   4
#define FNODE_LIMITS    32			// rate-limit entries
#define FNODE_NETWORKS  4

typedef struct {
	uint8_t key[32];
	char host[64];					// "" -- it connects to us
	uint16_t port;
	bool trusted;					// "trusted-relay": its objects' signatures not checked (D5)
	uint32_t max_object;			// "max=N": this link's limit (a radio link); 0 none
	uint32_t due_ms;				// next poll
	uint32_t backoff_s;
} fpeer_t;

typedef struct {
	char name[32];
	uint16_t listen;				// 0: outbound only
	uint32_t poll_s;
	fpeer_t peers[FNODE_PEERS];
	int npeers;
	uint8_t members[FNODE_MEMBERS][32];
	int nmembers;
	uint8_t blocked[FNODE_BLOCKED][32];
	int nblocked;
	char wants[FSESS_WANTS_MAX];
	struct { char pat[100]; uint32_t secs; } retain[FNODE_RETAIN];
	int nretain;
	uint32_t retain_default;
	// the networks this node takes part in, each with the ONE key its
	// node list is accepted from (docs/fed.md, "Networks")
	struct { char name[33]; uint8_t publisher[32]; } networks[FNODE_NETWORKS];
	int nnetworks;
	// the radio link (docs/fed.md, "The radio link"; Zeitlos only, through mesh0)
	bool radio;
	uint32_t radio_port;			// Meshtastic application port, 256 and up
	uint8_t radio_channel;
	uint32_t radio_max;				// the largest object over it
	uint32_t radio_pace_ms;			// at least this long between packets
} fcfg_t;

// Parses fed.cfg's text. False, with a message, on a line it cannot use.
bool fnode_config(const char *text, fcfg_t *c, char *err, int errlen);

// Starts the node: the configuration in `dir`/fed.cfg, the store in
// `dir`/store, the key from `seed` (32 bytes). 0, or -1 with a message.
int fnode_start(const char *dir, const uint8_t seed[32], char *err, int errlen);
void fnode_stop(void);
const fcfg_t *fnode_cfg(void);
const uint8_t *fnode_public_key(void);

// -- sessions --

// A new session from the pool, or NULL if all are in use. Outbound: the
// peer's index. Inbound: -1; `addr` identifies the connection's source
// for rate limits.
fsess_t *fnode_session(bool initiator, int peer, const char *addr, uint32_t now_ms);
// It ended (in any state); the pool slot is free again.
void fnode_session_end(fsess_t *s, uint32_t now_ms);

// May a connection from `addr` be taken now? Asked before reading a byte.
bool fnode_admit(const char *addr, uint32_t now_ms);

// The next peer due a poll, or -1. The caller connects to it, then
// makes an initiating session.
int fnode_due(uint32_t now_ms);

// -- the local interface --
//
//   PUB <topic> <type> <format> <kind> <key or -> <len>\n  then len bytes
//       -> OK <id hex> <position>\n    or   ERR <why>\n
//   LIST <network>\n
//       -> OK <len>\n then that network's current list, as published (OK 0: none yet)
//   KEY\n
//       -> OK <this node's key, hex> <its name>\n
//   SUB <name> <pattern>\n
//       -> OBJ <position> <len>\n then the object's bytes, one at a time;
//          each held until ACK <position>\n, which is also remembered as
//          <name>'s place, so a client that returns resumes after it
//
int fnode_client_open(void);				// an id, or -1
void fnode_client_input(int id, const uint8_t *d, uint32_t n);
uint32_t fnode_client_output(int id, const uint8_t **p);
void fnode_client_consumed(int id, uint32_t n);
void fnode_client_close(int id);

// Periodic work: deliveries to subscribers, retention. Call often.
void fnode_tick(uint32_t now_ms);

// -- networks (docs/fed.md, "Networks") --

// The platform's name resolver, for the address check: a host's IPv4
// address as text into ip[cap]. NULL (or a failure): that entry's
// address is not checked -- the key still is.
void fnode_set_resolver(bool (*resolve)(const char *host, char *ip, int cap));

// Loads each network's current list from the store (fnode_start() does
// this; a new list arriving reloads its network). How many are loaded.
int fnode_reload_lists(void);

// The two decisions, exposed for tests: may an object from `origin` on
// `topic` be stored; may `key` connect from `addr`.
bool fnode_origin_ok(const uint8_t origin[32], const char *topic);
bool fnode_connect_ok(const uint8_t key[32], const char *addr);

// -- the radio link: the platform brings the packets --
// Before fnode_start(): how to put one packet on the air. A platform
// without a radio does not call it, and a configured radio: is said to
// be unused.
void fnode_radio_attach(bool (*send)(const uint8_t *p, uint32_t n, void *ctx), void *ctx);
void fnode_radio_up(bool up);
void fnode_radio_packet(const uint8_t *p, uint32_t n, uint32_t now_ms);

#endif
