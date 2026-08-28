#ifndef SSH_H
#define SSH_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SSH client, layered on tcp.h -- the same shape telnet.h has, and for
 * the same reason: net.c relays a single interactive session to a
 * `term` window through a zport, and it should not care which protocol
 * is underneath.
 *
 * One connection at a time, following tcp.c's own single-TCB
 * constraint. SSH AND TELNET ARE THEREFORE MUTUALLY EXCLUSIVE -- there
 * is one TCB, so there is one session of either kind.
 *
 * -- What lives here and what lives in ssh_proto.c --
 *
 * ssh_proto.c is the protocol, and knows nothing about TCP, Zeitlos,
 * or randomness (see its header for why that matters). This file is
 * the binding:
 *
 *   - tcp_connect()/tcp_send() underneath, with an outbound queue,
 *     since tcp.c sends one unacknowledged segment at a time
 *   - z_rng_bytes() for the engine's randomness callback, with the
 *     z_rng_secure() gate in front of it
 *   - the interactive bits that are neither protocol nor transport:
 *     rendering the host key fingerprint, collecting a yes/no, and
 *     collecting a password without echoing it
 *
 * -- Prompts happen IN BAND, over the port --
 *
 * There is no separate channel for asking the user something, and
 * adding one would mean new message types, changes to zterm.h, and a
 * `term` that knows what a password is. Instead the prompt is written
 * to the same port the session will use, and the reply arrives as
 * ordinary port data which this file intercepts before it becomes
 * channel traffic.
 *
 * That works because `term` does no local echo once connected to a
 * port (term.c), so a password is invisible for free rather than by
 * arrangement. It is also why ssh_input() exists rather than a plain
 * ssh_send(): everything the user types goes through one door, and
 * this file decides whether it is an answer or a keystroke.
 */

// Text for the user (prompts, progress, errors) and channel data both
// arrive here -- net.c relays both to the port identically. Borrowed
// buffer, valid only for this call.
typedef void (*ssh_out_handler_t)(const uint8_t *data, uint16_t len);

// The interactive session is up. Distinct from "connected": net.c
// accepts the zport immediately so prompts can be shown, and this
// fires later, once there is a shell.
typedef void (*ssh_ready_handler_t)(void);

// Session over, for any reason. `reason` is human-readable and has
// already been sent to the port as text.
typedef void (*ssh_closed_handler_t)(const char *reason);

// Starts a connection. `user` may be NULL or empty, in which case a
// username is prompted for in band.
//
// RETURNS FALSE IF z_rng_secure() IS FALSE, and that is not a
// formality. Every ephemeral key in the exchange comes from the system
// CSPRNG; seeded from cycle-counter jitter rather than the TRNG, a
// passive observer can reconstruct the session key and neither end
// shows any symptom. A weak SSH session is not a degraded session, it
// is an open one, so this refuses rather than warning. See
// sw/common/zrng.h and docs/trng.md.
bool ssh_connect(uint32_t ip, uint16_t port, const char *user,
	ssh_out_handler_t on_out, ssh_ready_handler_t on_ready,
	ssh_closed_handler_t on_closed);

// Everything the user typed. Routed to whatever is currently being
// collected (a username, a yes/no, a password) or, once the session is
// open, sent as channel data.
void ssh_input(const uint8_t *data, uint16_t len);

// Graceful close, then abort. Same pair telnet.h has.
void ssh_close(void);
void ssh_abort(void);

// Call every main-loop iteration alongside tcp_poll(), exactly like
// telnet_poll(). Drains the outbound queue through tcp_send() as room
// allows; this does NOT call tcp_poll() itself.
void ssh_poll(void);

bool ssh_is_active(void);

#endif
