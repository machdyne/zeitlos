#ifndef FNET_H
#define FNET_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed node lists: a network's whitelist. docs/fed.md, "Networks".
 *
 * A node list is a state object on <network>/nodes, type fed.nodes, key
 * "list", published by the network's one publisher key, its payload
 * strict JSON (sw/common/zjson.h):
 *
 *   {"network": "timeless",
 *    "nodes": [{"key": "<64 lowercase hex>", "name": "machdyne",
 *               "sysop": "phil", "addr": "bbs.machdyne.com:9070"}, ...]}
 *
 * "key" is required; "name", "sysop" and "addr" are optional; other
 * fields are ignored (a later version may add some -- none can change
 * who is a member). Anything wrong -- a duplicate key, a bad key, a bad
 * name or address, more than FNET_MAX_NODES, the wrong network -- and
 * the WHOLE list is refused: guessing what a broken list meant would let
 * nodes disagree about who is in.
 */
#include <stdint.h>
#include <stdbool.h>

#define FNET_MAX_NODES 200
#define FNET_NAME_MAX  32
#define FNET_SYSOP_MAX 64
#define FNET_ADDR_MAX  100
#define FNET_MODS      16
#define FNET_MOD_PATS  200			// a moderator's patterns, joined

// What membership needs, kept in memory: keys, and the addresses to
// check inbound connections against. Names and sysops stay in the store.
typedef struct {
	char network[FNET_NAME_MAX + 1];
	int n;
	uint8_t key[FNET_MAX_NODES][32];
	char addr[FNET_MAX_NODES][FNET_ADDR_MAX + 1];	// "" if none
	// moderators (docs/fed.md, "Moderation"): who may cancel others'
	// objects, on which topics -- patterns '\n'-separated, as sessions have them
	// its profile (docs/fed.md, "Network profiles"): the largest object on
	// its topics (0: no limit but the protocol's), and whether its
	// letters are sealed classically (X25519 alone)
	uint32_t max_object;
	bool classical;
	int nmods;
	uint8_t mod_key[FNET_MODS][32];
	char mod_pats[FNET_MODS][FNET_MOD_PATS];
} fnet_list_t;

// A network name: 1-32 bytes of a-z 0-9 -, a topic's first segment.
bool fnet_name_ok(const char *name);

// Parses a list's payload for `network`. 0, or -1 with a message.
int fnet_parse(const char *network, const uint8_t *json, uint32_t len, fnet_list_t *out, char *err, int errlen);

bool fnet_has(const fnet_list_t *l, const uint8_t key[32], const char **addr);
// The key a list's JSON gives the node called `name`. (Lists keep keys,
// not names; this reads the stored list again -- for a letter addressed
// by name, which is rare.)
bool fnet_find_name(const uint8_t *json, uint32_t len, const char *name, uint8_t key[32]);
// Is `key` a moderator of `topic` in this list?
// Shared working space, for callers that never parse at the same moment
// (fnode loading a list; fadmin checking one): the parser's tokens, and a
// list to parse into. Each is large; one of each in the image, not three.
void *fnet_tokens(int *cap);
fnet_list_t *fnet_scratch(void);

bool fnet_moderates(const fnet_list_t *l, const uint8_t key[32], const char *topic);

// "host:port": the host 1+ bytes of A-Z a-z 0-9 . -, the port 1-65535.
bool fnet_addr_ok(const char *addr);

#endif
