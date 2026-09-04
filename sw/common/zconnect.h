#ifndef ZCONNECT_H
#define ZCONNECT_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Working out WHAT to connect a terminal to, in one place.
 *
 * There are four kinds of thing a `term` window can be pointed at --
 * another local process's port, a serial port, a telnet host, an ssh
 * host -- and every one of them ends up in the same shape: a provider
 * name to look up in the pid registry, and a scalar argument that
 * travels with the CONNECT.
 *
 * Getting to that shape is the part that differs, and it is not
 * trivial for two of them. Telnet has to resolve a hostname. SSH has
 * to hand `net` a username first and get back a token, because a
 * string forwarded through Z_TERM_SET_PORT would have its pointer
 * translated twice and land on garbage (see sw/common/znet.h). That
 * knowledge lived in sw/apps/repl/repl.c, as four command handlers.
 *
 * -- Why it moved --
 *
 * Because `term` needs it too. repl's commands work by telling term
 * where to go (Z_TERM_SET_PORT), which is fine when you are already
 * typing at a repl and useless when you want an Open dialog in the
 * terminal itself. The alternative was term reimplementing the
 * hostname resolve and the ssh token dance, at which point there are
 * two of each and they drift -- and the ssh one in particular is a
 * bounded request/reply with three separate failure modes that are
 * each worth reporting differently.
 *
 * So repl and term now both build a target with z_conn_prepare() and
 * then do their own thing with it: repl hands it to a term window,
 * term connects to it directly.
 *
 * -- THIS BLOCKS, AND THE CALLER'S MAILBOX STOPS BEING SERVICED --
 *
 * z_conn_prepare() can wait several seconds: a DNS lookup that gets
 * no answer, or an ssh prepare to a `net` that does not recognise the
 * subject. Both waits are BOUNDED (nothing here spins forever), but
 * while one is running the calling process is not reading messages.
 *
 * That cost is not the same for both callers, and it is worth knowing
 * which one you are:
 *
 *   repl services EVERY connected term window from one mailbox, so a
 *   slow lookup from one window stalls every other window's output
 *   too. That was already true before this moved and is documented
 *   where the commands used to live.
 *
 *   term services ONE window, but it is also the process wm expects a
 *   Z_WM_REDRAW_DONE from. A long enough stall here and wm reports
 *   "timed out waiting for pid N to ack a redraw" -- which is exactly
 *   the bug term's own connect_port() grew a custom message pump to
 *   avoid. Callers in term should say what they are doing on screen
 *   BEFORE calling this, because the window will not repaint during
 *   it.
 *
 * Doing better means an asynchronous resolve-then-connect flow, which
 * is a real improvement and a bigger change than this one.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zobj.h"
#include "zmsg.h"

typedef enum {
	// Another process's port, by pid-registry name: "portdemo0",
	// "repl1", "serial0". The text IS the name.
	Z_CONN_PORT = 0,
	// UART1 through sw/apps/serial. Text is an optional baud rate;
	// empty means "whatever the port is already at", which is what
	// you want when reconnecting.
	Z_CONN_SERIAL,
	// A telnet host, via `net`. Text is an IP or a hostname.
	Z_CONN_TELNET,
	// An ssh host, via `net`. Text is [user@]host.
	Z_CONN_SSH
} z_conn_kind_t;

// How long to wait for CONNECTED/REFUSED once the CONNECT goes out.
//
// The network kinds get far longer than zport.h's default, and the
// reason is in term.c's own history: net's telnet provider does not
// answer until a TCP handshake resolves, and tcp.c's retry budget
// alone is ~31.5s. A 2s timeout meant every connect to an
// unreachable host failed on term's side before net had finished
// trying -- so the error you got was never the real one.
#define Z_CONN_TIMEOUT_LOCAL_TICKS   (732u * 2)
#define Z_CONN_TIMEOUT_NETWORK_TICKS (732u * 45)

typedef struct {

	z_conn_kind_t	kind;

	// pid-registry name of the provider to connect to.
	char			provider[24];

	// Travels with the CONNECT as z_port_connect_arg()'s argument.
	// Z_NONE for a plain port; a scalar for everything else -- a baud
	// rate, an IPv4 address, or an ssh token. ALWAYS A SCALAR when it
	// is set: a string here would have its pointer translated twice
	// on the way through Z_TERM_SET_PORT.
	z_obj_t			arg;

	uint32_t		timeout_ticks;

	// Human-readable, for the caller's own confirmation message --
	// "192.168.1.10 (myhost.local)", "9600 baud". Filled in because
	// the caller no longer has the parsed pieces to build one from,
	// and because showing the RESOLVED address is what tells you
	// whether the name went where you meant.
	char			detail[64];

} z_conn_target_t;

// "port", "serial", "telnet", "ssh" -> kind. false if it is not one.
bool z_conn_kind_from_word(const char *word, z_conn_kind_t *out);

// The word, for building usage messages and dialog labels.
const char *z_conn_kind_name(z_conn_kind_t kind);

// Does this kind need something after the word?
//
// `serial` is the one that does not: bare `serial` is meaningful and
// means "at the current rate". Everything else without an argument is
// a usage error, and a dialog should not offer an empty field for it.
bool z_conn_kind_needs_text(z_conn_kind_t kind);

// Work out where `text` points, resolving and negotiating as needed.
//
// Returns false with a message in `err` on anything that stops a
// connection being possible: a malformed baud rate, a name that does
// not resolve, `net` not running, `net` not answering an ssh prepare,
// or net refusing the request.
//
// SEE THE HEADER ON BLOCKING. Telnet and ssh can take seconds.
//
// On success the caller either connects to out->provider itself
// (term) or hands the target to a term window with z_conn_handoff()
// below (repl).
bool z_conn_prepare(z_conn_kind_t kind, const char *text,
	z_conn_target_t *out, char *err, size_t errlen);

// Tell `term_pid` to go and connect to this target -- the
// Z_TERM_SET_PORT Z_MAP form (sw/common/zterm.h).
//
// Fire-and-forget: there is no reply, and the map is deliberately
// never freed. A message with a heap payload has no safe point to
// free it at (docs/messaging.md), so this leaks one small map per
// call rather than risking a receiver reading freed memory. That is
// the same accepted trade the repl commands this replaced already
// documented, and it is bounded by how often a person types a
// connect command.
void z_conn_handoff(uint32_t term_pid, const z_conn_target_t *target);

#endif
