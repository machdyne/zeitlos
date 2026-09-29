#ifndef SOCK_H
#define SOCK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Raw TCP for another process: an arbitrary host, an arbitrary port,
 * and not one byte added or removed in either direction.
 *
 * Layered directly on tcp.h, and structurally almost identical to
 * telnet.c -- the outbound queue, the poll-driven flush, the three
 * callbacks. What it does NOT have is telnet.c's option negotiation
 * and its IAC escaping, and that absence is the entire reason this
 * file exists rather than reusing telnet.c with a port argument:
 *
 *     telnet_send() rewrites every literal 0xFF byte as IAC IAC.
 *
 * In a terminal session that is a rare, harmless correctness detail.
 * In a TLS record it is fatal, and quietly so: ciphertext is uniform
 * random, so roughly one byte in 256 is 0xFF, which means every
 * record over a few hundred bytes gets silently corrupted and the
 * failure surfaces as a Poly1305 tag mismatch several layers away
 * from the cause.
 *
 * -- one connection at a time --
 *
 * NET_SOCK_SLOTS sockets at a time (default 2), each a slot with its
 * own connection and transmit queue: the browser can fetch while an
 * IRC client (sw/apps/irc) holds a connection open for hours. Every
 * call below takes the slot index. It used to be one socket, and before
 * that one connection of ANY kind, when tcp.c had a single TCB; tcp.c
 * has a pool now, and telnet, SSH and sockets coexist.
 * docs/networking.md, "More than one socket".
 *
 * -- compiled out with NET_SOCK=0 --
 *
 * `net` is a core app in the ZAR archive (sw/os/zar.h) and has to
 * stay small enough for a 1MB board, so this is optional at build
 * time. With NET_SOCK=0, sock.o is not linked and a Z_PORT_CONNECT
 * carrying a map is refused with a message that says which build
 * option is missing, rather than falling through to telnet and
 * failing somewhere confusing.
 */

#include <stdint.h>
#include <stdbool.h>
#include "tcp.h"

// Outbound bytes buffered while tcp.c's single unacknowledged segment
// is in flight.
//
// 2048 rather than something larger because of what actually goes
// through here: an HTTP request line with headers is a few hundred
// bytes, and the largest thing TLS 1.3 sends from the client side is
// a ClientHello at roughly 300. The receive direction is not buffered
// here at all -- it is handed straight to the port.
#define SOCK_TX_QUEUE_LEN 2048

#ifndef NET_SOCK_SLOTS
#define NET_SOCK_SLOTS 2
#endif

// Same borrowed-buffer lifetime as tcp.h's TCP_EVENT_DATA: valid only
// for the duration of the callback (docs/messaging.md). `slot` says
// which socket.
typedef void (*sock_data_handler_t)(int slot, const uint8_t *data, uint16_t len);
typedef void (*sock_established_handler_t)(int slot);
typedef void (*sock_closed_handler_t)(int slot);

// A free slot, or -1 if every socket is in use.
int sock_free_slot(void);

// Starts a connection in `slot`. The handlers are shared by every slot
// and told which one an event is for. False if the slot is busy or
// tcp.c has no connection to give. See net.c's sock_on_established().
bool sock_connect(int slot, uint32_t ip, uint16_t port,
	sock_established_handler_t on_established,
	sock_data_handler_t on_data,
	sock_closed_handler_t on_closed);

// Queues bytes for sending. False if the slot is not connected or its
// queue is full; nothing is queued then.
bool sock_send(int slot, const uint8_t *data, uint16_t len);
// Room left in a socket's send queue; -1 if it is not established.
int sock_send_room(int slot);

// -- client -> peer backpressure, as relay.c does it --
//
// A client's DATA goes into the send queue; what does not fit is HELD,
// unacked -- its bytes stay valid in the client's memory until acked
// (zport.h) -- and the client, having as many sends unacked as zport
// lets it (SOCK_HOLD), waits. Held bytes move in as TCP drains them;
// each message acked once all of it is in. It used to be dropped, and
// the connection with it: a session sending a few KB over a slow link
// ended mid-way (fed on a board, to a server).
#define SOCK_HOLD 8						// Z_PORT_MAX_PENDING_SENDS
typedef void (*sock_ack_fn)(uint32_t pid, uint32_t tag);
typedef bool (*sock_alive_fn)(int slot);	// its client still running?
typedef void (*sock_gone_fn)(int slot);		// ... it is not: close it
enum { SOCK_QUEUED = 1, SOCK_HELD = 0, SOCK_NOT_OPEN = -1, SOCK_OVER = -2 };
// SOCK_QUEUED: all in, ack it now. SOCK_HELD: acked later, by the pump.
// SOCK_NOT_OPEN: nowhere to put it. SOCK_OVER: more than SOCK_HOLD held.
int sock_offer(int slot, const uint8_t *data, uint32_t len, uint32_t pid, uint32_t tag);
// Once a loop: held bytes into the queue; acks through `ack`. Before
// touching a client's memory, `alive`; if not, the held data is forgotten
// unread and unacked, and `gone` closes the socket.
void sock_pump_held(sock_ack_fn ack, sock_alive_fn alive, sock_gone_fn gone);
// Whatever a slot still holds: acked (`ack`), or forgotten (NULL).
void sock_drop_held(int slot, sock_ack_fn ack);
int sock_held(int slot);

// Graceful close: the queue drains first -- call sock_poll() enough
// times first if that matters.
void sock_close(int slot);

// Immediate: the queue is dropped and the connection reset.
void sock_abort(int slot);

// The slot's tcp.c connection, or NULL.
tcp_conn_t *sock_tcp(int slot);

// Moves queued bytes onto the wire, for every slot. Call from the main
// loop.
void sock_poll(void);

#endif
