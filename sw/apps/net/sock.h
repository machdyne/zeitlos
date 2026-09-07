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
 * tcp.c has a single TCB, so a socket, a telnet session and an SSH
 * session are mutually exclusive system-wide. net.c checks all three
 * before starting any of them. That is not a new restriction: telnet
 * and SSH already had it, and this adds a third participant rather
 * than a new problem.
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

// Outbound bytes buffered while tcp.c's single unacknowledged segment
// is in flight.
//
// 2048 rather than something larger because of what actually goes
// through here: an HTTP request line with headers is a few hundred
// bytes, and the largest thing TLS 1.3 sends from the client side is
// a ClientHello at roughly 300. The receive direction is not buffered
// here at all -- it is handed straight to the port.
#define SOCK_TX_QUEUE_LEN 2048

// Same borrowed-buffer lifetime as tcp.h's TCP_EVENT_DATA: valid only
// for the duration of the callback (docs/messaging.md).
typedef void (*sock_data_handler_t)(const uint8_t *data, uint16_t len);
typedef void (*sock_established_handler_t)(void);
typedef void (*sock_closed_handler_t)(void);

// Starts an active open to ip:port. Returns false if tcp.c already
// has a connection.
//
// Note that on_established fires when the TCP handshake completes,
// which is the point net.c defers its Z_PORT_CONNECTED to -- see
// net.c's sock_on_established().
bool sock_connect(uint32_t ip, uint16_t port,
	sock_established_handler_t on_established,
	sock_data_handler_t on_data,
	sock_closed_handler_t on_closed);

// Queues bytes for sending, verbatim. Returns false if not connected
// or if the queue has no room; a false return is not a hard error --
// hold the data and try again on a later poll, exactly as
// tcp_send()/telnet_send() ask.
//
// Either the whole buffer is queued or none of it is. A partial
// write here would mean the caller had to track how much of its own
// record went out, which for a TLS record is not a thing it can do.
bool sock_send(const uint8_t *data, uint16_t len);

// Graceful close (tcp_close()). Does not wait for the queue to drain
// -- call sock_poll() enough times first if that matters.
void sock_close(void);

// Immediate teardown (tcp_abort()), discarding anything queued.
void sock_abort(void);

// Call every main-loop iteration alongside tcp_poll(). Flushes the
// queue through tcp_send() as room allows.
void sock_poll(void);

#endif
