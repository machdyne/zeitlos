#ifndef RELAY_H
#define RELAY_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Accepted connections, relayed to the process that listens for them:
 * Z_NET_LISTEN (sw/common/znet.h). docs/netserve.md has the design.
 *
 * net's other sessions (telnet.c, ssh.c, sock.c) are connections this
 * machine MAKES, one of each kind. These are connections it ACCEPTS,
 * several at once, each relayed byte for byte to the listener -- which
 * is the zport provider, with net as its client. Nothing is added to or
 * removed from the stream: the listener speaks telnet or HTTP itself.
 */
#include <stdint.h>
#include <stdbool.h>
#include "../../common/zmsg.h"

void relay_init(void);

void relay_listen(const z_msg_t *msg);          // Z_NET_LISTEN
void relay_unlisten(const z_msg_t *msg);        // Z_NET_UNLISTEN

// Every message from a listening process -- CONNECTED, REFUSED, DATA,
// DATA_ACK, CLOSE -- is a relay's, and is handled here: true. Called
// BEFORE net's own port handlers, which match DATA by tag alone and
// would otherwise claim a relay's.
bool relay_msg(const z_msg_t *msg);

// Every main-loop pass: moves bytes both ways, applies backpressure.
void relay_poll(void);

// Relays in use, for the startup banner and diagnostics.
int relay_count(void);

#endif
