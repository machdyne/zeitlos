#ifndef ZLINK_H
#define ZLINK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink -- the link layer. docs/zlink.md.
 *
 * Reliable, ordered messages on up to 256 channels between two
 * machines, over a TRANSPORT that moves whole frames and may lose or
 * damage them. Two transports exist (zlink_soft.c, zlink_stream.c); the
 * link layer does not care which it has, except that a half-duplex one
 * gets a polling discipline (below).
 *
 * Portable C with no OS calls: time comes in as an argument, randomness
 * as a seed, so sw/common/tests/test_zlink.c runs two links against
 * each other on the host with frames dropped and corrupted.
 *
 * -- Frames --
 *
 *   0     type           ZL_T_HELLO, ZL_T_DATA, ZL_T_ACK
 *   1     seq            DATA: this frame's sequence number
 *   2     ack            every frame: the next sequence number the
 *                        sender expects (a cumulative ack)
 *   3     chan           DATA: the channel
 *   4..5  len            payload length, little-endian
 *   6..   payload        up to ZL_PAYLOAD bytes
 *   then  CRC-32         IEEE 802.3, little-endian, over everything above
 *
 * A frame that fails its CRC, or is short, is dropped as if it never
 * arrived; retransmission covers it. The transport adds its own
 * delimiters (K symbols on the stream engine, SLIP-style bytes on the
 * soft link) and nothing else.
 *
 * -- Reliability --
 *
 * Go-back-N with a window of ZL_WINDOW frames (1 on a half-duplex
 * transport). The receiver takes only the next sequence number and
 * acks cumulatively, on its own data or in an ACK frame; the sender
 * resends everything unacked after the transport's timeout. A full
 * receive queue is flow control: the frame is not taken and not acked,
 * so the sender tries again later.
 *
 * -- Coming up --
 *
 * Both ends send HELLO -- a random 32-bit number, their capability
 * bits, the peer's number if they have heard it, and whether they are
 * up -- after a random delay. An end is up when it has the peer's HELLO
 * AND that HELLO carries its own number back. A HELLO from an end that
 * is not up yet is answered; one from an up end is not, which is what
 * stops two up ends answering each other for ever. A HELLO that does
 * not carry our number means the peer restarted: we go down with it. The larger number is the
 * PRIMARY; on a half-duplex transport only the primary speaks first
 * (below). A link that hears nothing for ZL_DEAD_MS goes down, and
 * sequence numbers restart when it comes up again, and messages the app
 * had not yet taken are dropped: they belonged to the link that ended.
 *
 * -- Half duplex --
 *
 * The soft transport has one turn: two ends talking at once is a
 * collision. So once up, the primary sends a frame (its data, or an
 * ACK as a poll) and the secondary answers every frame it receives
 * with exactly one -- its own data, or an ACK. The primary polls every
 * ZL_POLL_MS when idle, at once when it has data or just got some.
 */

#include <stdint.h>
#include <stdbool.h>

#define ZL_PAYLOAD   448		// per message; two frames fit a 1024-entry FIFO
#define ZL_HDR       6
#define ZL_FRAME_MAX (ZL_HDR + ZL_PAYLOAD + 4)
#define ZL_WINDOW    2			// frames in flight, full duplex
#define ZL_RXQ       4			// received messages waiting for the app

#define ZL_T_HELLO   1
#define ZL_T_DATA    2
#define ZL_T_ACK     3

// HELLO payload
#define ZL_MAGIC0    'Z'
#define ZL_MAGIC1    'L'
#define ZL_VERSION   1

// capability bits, in HELLO
#define ZL_CAP_STREAM  0x01		// has a GPIO stream engine with zlink
#define ZL_CAP_SOFT    0x02		// can do the soft transport
#define ZL_CAP_FILES   0x04		// serves files (zlink app)
#define ZL_CAP_SHELL   0x08		// serves a shell (zlink app)
#define ZL_CAP_WRITE   0x10		// its files may be written too (zlink app)

#define ZL_DEAD_MS     3000		// silence before the link goes down
#define ZL_POLL_MS     20		// half duplex: the primary's idle poll

// What the link layer needs from a transport. Every call is
// non-blocking; poll() is where a transport does its work.
typedef struct {
	const char *name;
	void *ctx;
	bool half_duplex;
	uint32_t rto_ms;			// resend after this long without an ack

	// Start sending one frame. false: still busy with the last one.
	bool (*send)(void *ctx, const uint8_t *frame, uint16_t len);

	// Nothing being sent.
	bool (*tx_idle)(void *ctx);

	// A whole frame received: its length, copied into buf. 0: none yet.
	// A frame longer than max is dropped by the transport.
	uint16_t (*recv)(void *ctx, uint8_t *buf, uint16_t max);

	// Do some work. `now_ms` for the transport's own timeouts.
	void (*poll)(void *ctx, uint32_t now_ms);
} zl_transport_t;

typedef struct {
	uint32_t tx_frames, rx_frames;
	uint32_t retransmits;
	uint32_t crc_errors;		// dropped: bad CRC, short or malformed
	uint32_t out_of_order;		// dropped: not the next sequence number
	uint32_t rx_full;			// dropped: the app's queue was full
	uint32_t ups, downs;
	uint32_t tx_bytes, rx_bytes;	// payload only
} zl_stats_t;

typedef struct {
	zl_transport_t *t;

	// identity and negotiation
	uint32_t my_rand, peer_rand;
	uint8_t my_caps, peer_caps;
	bool got_hello;			// the peer's HELLO has arrived
	bool hello_owed;		// a peer still coming up needs our HELLO
	bool up;
	bool primary;
	uint32_t hello_due;		// when to (re)send HELLO while down
	uint32_t last_rx;		// last valid frame, for ZL_DEAD_MS
	uint32_t last_tx;		// last frame sent, for the keepalive
	uint32_t now;			// the time zl_poll() was given

	// sender
	uint8_t next_seq;		// next sequence number to assign
	uint8_t base;			// oldest unacked
	uint8_t inflight;		// frames between base and next_seq
	struct {
		uint8_t chan;
		uint16_t len;
		uint8_t data[ZL_PAYLOAD];
	} win[ZL_WINDOW];
	uint8_t resend_from;	// window index to (re)send next; inflight if none
	uint32_t rto_at;		// when to go back to base
	bool ack_due;			// we owe the peer an ACK

	// half duplex
	bool turn;				// we may send (half duplex only)
	bool turn_fresh;		// the turn came in this zl_poll(): see tx_half_duplex()
	uint32_t turn_at;		// primary: when the turn came back / poll due

	// receiver
	uint8_t expect;			// next sequence number we will take
	struct {
		uint8_t chan;
		uint16_t len;
		uint32_t at;		// when its frame arrived, ms
		uint8_t data[ZL_PAYLOAD];
	} rxq[ZL_RXQ];
	uint8_t rxq_head, rxq_count;
	uint32_t recv_at;		// zl_recv_time()

	// scratch
	uint8_t frame[ZL_FRAME_MAX];

	uint32_t rng;
	zl_stats_t st;
} zlink_t;

// Start (or restart) a link on a transport. `seed` should be random:
// it decides who is primary, and the HELLO timing that breaks a tie.
void zl_init(zlink_t *l, zl_transport_t *t, uint32_t seed, uint8_t caps);

// Run the link: the transport, received frames, timers, sending. Call
// often -- every pass of the app's loop.
void zl_poll(zlink_t *l, uint32_t now_ms);

bool zl_up(const zlink_t *l);

// Queue one message of up to ZL_PAYLOAD bytes on a channel. Returns
// false if the window is full or the link is down: try again later.
bool zl_send(zlink_t *l, uint8_t chan, const void *buf, uint16_t len);

// Room for another zl_send() right now.
bool zl_can_send(const zlink_t *l);

// Nothing sent and still unacknowledged.
bool zl_tx_empty(const zlink_t *l);

// The next received message, in order. Its length (0 is a valid empty
// message), or -1 if there is none. The payload goes to buf (up to
// max bytes; more is cut off) and its channel to *chan.
int zl_recv(zlink_t *l, uint8_t *chan, void *buf, uint16_t max);

// When the message zl_recv() last returned arrived -- the zl_poll()
// time its frame came in. How old a message is, for an app that acts on
// time (sw/apps/zlink's upgrade refuses a stale offer).
uint32_t zl_recv_time(const zlink_t *l);

// The peer's capability bits (valid while up).
uint8_t zl_peer_caps(const zlink_t *l);

// CRC-32 (IEEE 802.3), exposed for the transports' tests.
uint32_t zl_crc32(const uint8_t *p, uint32_t n);

// The same CRC a piece at a time: start from 0, feed each piece with
// the last result. zl_crc32_update(zl_crc32_update(0, a, n), b, m) is
// zl_crc32() of a then b. sw/apps/zlink checks whole files with it.
uint32_t zl_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

#endif
