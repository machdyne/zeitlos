#ifndef ZLISTEN_H
#define ZLISTEN_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A listener noticing that net has been replaced.
 *
 * net checks its listeners (relay.c, check_listeners) and drops a port
 * whose process has gone. Nothing does the opposite. A new net starts
 * with an empty listen table, and the kernel gives it the first free
 * slot, which is the one the old net just left -- the same pid. Once
 * that new net is running, z_proc_status() cannot tell it from the old
 * one. The only signal is the gap while the new net is being loaded,
 * and that gap is well under a second. A listener that sleeps for a
 * second can miss it, and then never asks for its ports again.
 *
 * Ask at least every Z_LISTEN_POLL_TICKS while a listen is held. On a
 * gap: forget every connection (the relays died with that net, and a
 * CLOSE would be delivered to whoever has the pid now) and listen
 * again. Listening again while the same net is still running resets
 * the connections it is already carrying, so that is not a retry.
 */

#include "zsoc.h"

/* ~100 ms. Short enough that a sleep cannot step over the load of a
 * new net, long enough that an idle listener is not a busy loop. */
#define Z_LISTEN_POLL_TICKS (Z_TICK_HZ / 10)

/* `running` is what z_proc_status() says about `net_pid` (true when
 * the state is Z_PROC_STATE_RUNNING). A pid of 0 has not been looked
 * up yet; that is not a death. */
static inline bool z_listen_lost(uint32_t net_pid, bool running)
{
	return net_pid != 0 && !running;
}

/* True once per poll. A viewer already wakes the listener many times
 * a second to scan the screen; the status call is the cost, so it
 * happens on this cadence and not on every wake. The wait is what
 * must stay at most this long, or a quiet listener sleeps over the gap. */
static inline bool z_listen_due(uint32_t now, uint32_t *next)
{
	if ((int32_t)(now - *next) < 0)
		return false;
	*next = now + Z_LISTEN_POLL_TICKS;
	return true;
}

#endif
