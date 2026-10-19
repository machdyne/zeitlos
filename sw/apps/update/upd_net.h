#ifndef UPD_NET_H
#define UPD_NET_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * update's mailbox and its TFTP source (docs/update.md, "Inside").
 *
 * One loop reads every message this process gets. Stream messages from
 * `net` go to the consumer (zstream.h) and become EVENTS in a small
 * queue -- several can arrive in one go: net sends a file's last chunk
 * and its end-of-file together, without waiting for a pull, and the
 * end-of-file must not be lost while the chunk is still waiting to be
 * taken. Everything else goes to the caller's `other` hook (port acks
 * for the terminal).
 *
 * Needs only z_msg_read(), z_proc_wait() and z_uptime_ticks() besides
 * zstream.c and zobj.c, so tests/test_net.c runs it on the host against
 * the real stream code and a `net` that behaves like sw/apps/net/tftp.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zmsg.h"

// Set before use: net's pid, the server, and where other messages go.
void upd_net_init(uint32_t net_pid, uint32_t server_ip,
	void (*other)(z_msg_t *m), uint32_t tick_hz);

// Reads and dispatches what is in the mailbox, waiting up to
// `wait_ticks` for the first message if there is none.
void upd_net_pump(uint32_t wait_ticks);

// upd_io_t's fetch_open / fetch_read / fetch_close (upd.h).
int upd_net_open(const char *name, char *err, int errlen);
int upd_net_read(uint8_t *buf, int cap, char *err, int errlen);
void upd_net_close(void);

#endif
