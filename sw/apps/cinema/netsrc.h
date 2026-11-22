#ifndef CINEMA_NETSRC_H
#define CINEMA_NETSRC_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netsrc -- a film on a web server, for cinema (docs/cinema_app.md,
 * "From a web server"). One TCP connection through `net`, one HTTP
 * range request on it at a time (httpc.c):
 *
 *   - the STREAM: the film's records, asked for NS_REQ bytes at a time
 *     as the ring has room, ahead of the player, which takes them with
 *     ns_stream_read() as it would read a file;
 *   - a READ: bytes somewhere else in the file -- the header, the
 *     index, the colour layer -- either waited for (ns_read_at()) or
 *     asked for and left to arrive (ns_read_later()).
 *
 * Nothing here blocks unless the call says it waits, and a call that
 * waits reads only the network's own messages from the mailbox: keys
 * and wm's messages stay queued, in order, for the caller's loop.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../../common/zmsg.h"

/* "http://host[:port]/path". False if it is not one, or `net` is not
 * running (ns_error() says which). Nothing is sent yet. */
bool ns_open(const char *url);
void ns_close(void);
bool ns_is_open(void);

/* A message from the app's loop: true if it was the network's. */
bool ns_msg(const z_msg_t *msg);

/* Once a pass of the app's loop: the next request, timeouts. */
void ns_poll(void);

/* n bytes at off, waited for. The bytes read (fewer at the file's end),
 * or -1. */
int ns_read_at(uint32_t off, void *buf, uint32_t n);

/* The same without waiting: *got counts up as they arrive and *done is
 * set at the end (fewer than n: the file's end, or it failed). One at a
 * time; false if one is already wanted. The buffer must stay. */
bool ns_read_later(uint32_t off, void *buf, uint32_t n, uint32_t *got, bool *done);
/* is that one still on its way? (wait: until it is not) */
bool ns_read_pending(bool wait);

/* The stream from `off`, up to `limit` (0: the file's end): what was
 * buffered is dropped. */
void ns_stream_seek(uint32_t off, uint32_t limit);
/* Up to max bytes of it; with `wait`, not 0 unless it has ended. */
uint32_t ns_stream_read(uint8_t *dst, uint32_t max, bool wait);
bool ns_stream_ended(void);         /* the file's end, or the connection's: nothing more will come */
uint32_t ns_stream_buffered(void);

/* "" while all is well */
const char *ns_error(void);

/* for the timing lines */
typedef struct {
	uint32_t bytes, requests, connects, retries;
	uint32_t wait_ticks;            /* spent in calls that wait */
} ns_stats_t;
void ns_stats(ns_stats_t *out, bool reset);

#endif
