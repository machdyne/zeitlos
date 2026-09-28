#ifndef BBS_H
#define BBS_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- the core. docs/bbs.md.
 *
 * Portable C99 with no Zeitlos headers: the same core runs as a port
 * provider on a Zeitlos machine (sw/apps/bbs/zeitlos) and as a server
 * on Linux (sw/apps/bbs/linux), and the same data directory works on
 * either. Everything the core needs from the machine it asks of the
 * platform through the plat_*() functions below; everything the
 * platform needs from the core is the bbs_*() functions.
 *
 * One process, one state machine per caller ("node"), and nothing that
 * waits: input arrives with bbs_input(), output leaves through
 * bbs_output()/bbs_consumed(), time moves on with bbs_poll().
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// -- limits --
#define BBS_NODES_MAX      16          // the most bbs.cfg may ask for
#define BBS_HANDLE_MAX     20          // characters in a handle
// Bytes of output waiting per node. A platform picks it for its memory:
// 4 KB on Zeitlos (the Makefile), 16 KB on Linux. A page is cut short,
// with a "more" prompt, rather than let the ring fill.
#ifndef BBS_OUT_RING
#define BBS_OUT_RING       8192
#endif
#define BBS_PAGE_ROOM      1024        // what a page must leave free to go on
#define BBS_PATH_MAX       160

// -- what the platform provides: sw/common/zplat.h, shared with fed --
#include "../../../common/zplat.h"

// -- what the core provides --

// Who a connection is, as the platform knows it.
typedef struct {
	const char *transport;             // "telnet", "ssh", "local"
	const char *peer;                  // "192.0.2.7" or "" -- for the log and who's online
	const char *user;                  // what the client offered (SSH's user name), or ""
} bbs_conn_t;

// Reads bbs.cfg from `datadir` and prepares the files. False, with the
// reason logged, if the data directory cannot be used.
bool bbs_init(const char *datadir);

// A caller arrives. Returns its node (0-based), or -1 if every node is
// busy -- the platform then says so and closes (bbs_busy_text()).
int bbs_connect(const bbs_conn_t *who);
const char *bbs_busy_text(void);

// Bytes the caller typed.
void bbs_input(int node, const uint8_t *d, uint32_t n);

// The caller has gone (closed, died). The node is freed at once.
void bbs_hangup(int node);

// Timers: detection, timeouts, the idle limit. Call often -- every
// few hundred milliseconds at least.
void bbs_poll(void);

// Output waiting for a node: a pointer to the next contiguous bytes
// and how many (0: nothing). Send what you can, then report it with
// bbs_consumed(). Whatever is not consumed stays for next time.
uint32_t bbs_output(int node, const uint8_t **p);
void bbs_consumed(int node, uint32_t n);

// The core has finished with this node: send what is left, then close
// the connection and call bbs_hangup().
bool bbs_wants_close(int node);

// Is anything happening at all (for how long the platform may sleep).
bool bbs_busy(void);

int bbs_nodes(void);                   // how many nodes bbs.cfg allows

// -- the link to this node's fed: federated forums (docs/bbs.md,
//    "Federation"). The platform connects to bbs_fed_target() -- a socket
//    path on Linux, a port name on Zeitlos; "" when no forum is federated
//    -- says when it is up or down, and moves the bytes both ways.
const char *bbs_fed_target(void);
void bbs_fed_up(bool up);
void bbs_fed_input(const uint8_t *d, uint32_t n);
uint32_t bbs_fed_output(const uint8_t **p);
void bbs_fed_consumed(uint32_t n);

#endif
