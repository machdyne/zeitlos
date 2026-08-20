#ifndef TELNET_H
#define TELNET_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Telnet (RFC 854), client-only, layered directly on tcp.h -- one
 * connection at a time, following tcp.c's own constraint (there's
 * only one TCB, so there can only ever be one telnet session either).
 *
 * Deliberately minimal option handling: refuses (WONT/DONT) every
 * option a server proposes TO us (we don't implement terminal-type,
 * NAWS, linemode, or anything else a real telnetd might ask for), but
 * *accepts* ECHO and SUPPRESS-GO-AHEAD when the server offers them
 * (WILL ECHO / WILL SUPPRESS-GO-AHEAD -> DO). This isn't symmetric
 * "refuse everything" by accident -- a real interactive session needs
 * the server to actually echo typed characters back (term, the
 * eventual consumer of this data via zport.h, does no local character
 * echo of its own once connected to a port -- see term.c), so
 * refusing WILL ECHO would leave the user typing blind. Almost every
 * common telnetd (Linux/BSD `telnetd`, busybox's) offers exactly
 * these two options unprompted at connect time and works fine once
 * they're accepted.
 *
 * Deliberately NEVER replies to an unsolicited DONT/WONT from the
 * peer (only DO/WILL get an answer) -- this isn't an oversight, it's
 * what keeps this loop-safe. A telnet implementation that always
 * answers every negotiation message, including replies to its own
 * replies, is a classic way to build an infinite DO/WONT/DONT/WONT
 * ping-pong with a peer that does the same. Since our own option
 * state never actually changes (we're stateless -- every DO/WILL from
 * the peer gets exactly one answer, and we never propose anything
 * ourselves), there's genuinely nothing to acknowledge in a DONT/WONT
 * and any negotiation chain terminates in one round trip.
 *
 * Escapes literal 0xFF bytes in outgoing application data as IAC IAC
 * per the protocol (telnet_send()) -- rare from a keyboard, cheap to
 * get right regardless.
 */

#include <stdint.h>
#include <stdbool.h>

// data arrived, with any telnet negotiation bytes already stripped --
// same borrowed-buffer lifetime as tcp.h's TCP_EVENT_DATA (valid only
// for the duration of this callback).
typedef void (*telnet_data_handler_t)(const uint8_t *data, uint16_t len);

typedef void (*telnet_established_handler_t)(void);
typedef void (*telnet_closed_handler_t)(void);

// starts a connection to ip:23. returns false if a telnet/TCP
// connection is already in progress (see "one connection at a time"
// above).
bool telnet_connect(uint32_t ip, telnet_established_handler_t on_established,
	telnet_data_handler_t on_data, telnet_closed_handler_t on_closed);

// queues application data for sending -- escapes any literal 0xFF
// (IAC) byte first (see above). returns false if not connected, or
// if the internal send queue doesn't have room (the queue exists
// specifically so a burst of option-negotiation replies and app data
// don't fight over tcp_send()'s single-outstanding-segment limit
// (tcp.h); it's still finite, though -- treat false the same as
// z_port_send()/tcp_send() failing: the caller should hold the data
// and try again on a later poll, not treat it as a hard error).
bool telnet_send(const uint8_t *data, uint16_t len);

// begins a graceful close (tcp_close() underneath). NOTE: does not
// itself wait for any data still sitting in the internal send queue
// to actually go out first -- call telnet_poll() enough times to
// drain it beforehand if that ordering matters to the caller.
void telnet_close(void);

// forces the connection down immediately (tcp_abort() underneath),
// discarding anything still queued to send.
void telnet_abort(void);

// call every main-loop iteration alongside tcp_poll() (this does NOT
// call tcp_poll() itself -- net.c calls both explicitly, same
// explicit-per-layer-poll style eth_poll()/tftp_poll() already use).
// flushes queued outbound bytes (negotiation replies and/or
// telnet_send() data) through tcp_send() as room allows.
void telnet_poll(void);

#endif
