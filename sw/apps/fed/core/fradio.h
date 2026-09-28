#ifndef FRADIO_H
#define FRADIO_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The radio link: zfed objects over a broadcast medium whose packets
 * hold 233 bytes (Meshtastic, through mesh0). docs/fed.md, "The radio
 * link". Nothing here knows Meshtastic or Zeitlos: the platform sends
 * what fradio gives it, and hands it what arrives.
 *
 * Three packets, each at most FRADIO_PKT bytes; an id is the store's own
 * 16-byte prefix of an object's id:
 *
 *   'I' n(1) n x id(16)                                    I have these
 *   'W' id(16) bitmap(1..32)                               these fragments of it, please
 *   'F' id(16) len(2) index(1) count(1) data(<= 212)       a fragment
 *
 * Objects are announced as they arrive here -- not those that came from
 * the radio (Meshtastic carries packets across hops itself: echoing them
 * would loop) -- and the newest are announced again now and then, for
 * nodes that were out of range. A node wants what it lacks; fragments
 * are broadcast, so one transmission serves every listener. An object
 * put back together is checked against its id first, then exactly as a
 * session checks one, and stored marked as the radio's.
 */
#include <stdint.h>
#include <stdbool.h>
#include "fobj.h"

#define FRADIO_PKT        233			// a Meshtastic packet's payload
#define FRADIO_ID         16
#define FRADIO_FRAG_DATA  (FRADIO_PKT - 1 - FRADIO_ID - 4)	// 212
#define FRADIO_INV_MAX    ((FRADIO_PKT - 2) / FRADIO_ID)		// 14
#define FRADIO_ASM        2				// objects being put back together at once
#define FRADIO_MAX        4096			// the largest object a radio link carries: 20
										// fragments, at a packet every few seconds
#define FRADIO_QUEUE      48			// packets waiting for the air

typedef struct {
	uint32_t max_object;				// the largest object over the radio (<= FRADIO_MAX)
	uint32_t pace_ms;					// at least this long between packets sent
	uint32_t announce_ms;				// the newest announced again this often
	uint32_t retry_ms;					// an incomplete object asked for again after this
	int retries;						// ... this many times, then given up
	// one packet into the air; false if it could not go (tried again later)
	bool (*send)(const uint8_t *p, uint32_t n, void *ctx);
	// Unix time, for the store and the future-time check
	uint32_t (*clock)(void *ctx);
	// the checks a session makes (fsess.h): may be NULL
	const char *wants;
	bool (*origin_ok)(const uint8_t origin[32], const char *topic, void *ctx);
	bool (*admit)(const fobj_t *o, void *ctx);
	void (*stored)(const fobj_t *o, void *ctx);
	void *ctx;
} fradio_cfg_t;

typedef struct {
	uint32_t sent_pkts, got_pkts, bad_pkts;
	uint32_t objects_in, objects_rejected, objects_out;
	uint32_t given_up;
} fradio_stats_t;

// Starts the link over the open store (fstore.h).
void fradio_init(const fradio_cfg_t *cfg, uint32_t now_ms);
void fradio_up(bool up);				// the radio came up, or went down
bool fradio_is_up(void);
void fradio_packet(const uint8_t *p, uint32_t n, uint32_t now_ms);
void fradio_poll(uint32_t now_ms);		// announce, ask again, send the next packet
const fradio_stats_t *fradio_stats(void);

#endif
