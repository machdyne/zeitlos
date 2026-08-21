#ifndef ZPORT_H
#define ZPORT_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Ports -- a small client/provider protocol so an app like `term`
 * doesn't need to know or care whether it's talking to a real
 * hardware UART, a telnet-over-UDP proxy, or a test harness. See
 * docs/ports.md for the full design writeup (why this exists, why
 * it's plain z_msg_t and not zstream.h, the flow-control tradeoff)
 * -- this header is deliberately just the protocol + thin helpers,
 * not a repeat of that reasoning.
 *
 * A provider can be reached at a fixed, documented pid (the
 * Z_PID_PORTDEMO convention below), started before any client that
 * wants it -- or, better, registered by name (sw/os/pidreg.h) and
 * looked up by whoever wants to connect, same migration Z_PID_WM/
 * Z_PID_NET already went through (see docs/networking.md). The fixed
 * pid still exists as a fallback for whichever path a given provider/
 * client pair doesn't (yet) use.
 *
 *   CONNECT   client -> provider   tag=0         obj=Z_NONE
 *   CONNECTED provider -> client   tag=0         obj=Z_UINT32(conn_id)
 *   REFUSED   provider -> client   tag=0         obj=Z_STR(reason)
 *   DATA      either direction     tag=conn_id   obj=Z_BLOB
 *   DATA_ACK  either direction     tag=conn_id   obj=Z_NONE
 *   CLOSE     either direction     tag=conn_id   obj=Z_NONE
 *
 * DATA/CLOSE aren't wrapped in a blocking helper -- a connected app's
 * own message loop should recognize Z_PORT_DATA/Z_PORT_CLOSE by
 * subject directly (same as it already does for e.g. Z_WM_KEY),
 * using z_blob_data()/z_blob_len() (zobj.h) on a DATA message's
 * payload, and msg.tag to find which z_port_t it belongs to (matters
 * for a provider juggling more than one connection; a client with
 * exactly one connection can just check msg.tag == port.conn_id, or
 * skip the check if there's genuinely only ever one).
 *
 * DATA_ACK: sent by whichever side just finished READING a DATA
 * message's payload, back to whoever sent it -- see
 * z_port_send_ack()/z_port_handle_ack() below, and docs/messaging.md's
 * "Known limitations" for the full design writeup: why this exists
 * (z_port_send()'s own z_obj_blob() allocation used to just leak,
 * forever, on every call -- see that function's own comment), why it
 * has to be sent by the receiving app's own code once it's genuinely
 * done reading the payload rather than automatically the moment
 * z_msg_read() returns, and why the sender matches an incoming ack to
 * a pending send by FIFO ORDER (the oldest still-outstanding one),
 * not by any value the ack carries -- a pointer isn't safely
 * comparable across the process boundary here (see z_port_t's own
 * comment on `pending` for exactly why). The payload is just Z_NONE:
 * nothing needs identifying, since which specific send an ack "is
 * for" is never actually in question -- it's whichever one's oldest.
 * What that DOES depend on -- that an ack, once sent, reliably
 * arrives -- z_port_send_ack() itself guarantees with a short bounded
 * retry, since this is the one place a lost message would silently
 * break that ordering guarantee (see its own comment for the
 * real-hardware bug that motivated this).
 * Every current DATA sender/receiver in this codebase (`sw/apps/net`'s
 * telnet relay, `repl`, `term`, `portdemo`) sends/handles this now --
 * a future DATA consumer should too, or z_port_send() to it will
 * eventually backpressure (see Z_PORT_MAX_PENDING_SENDS below) once
 * enough sends go unacked.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"

#define Z_PORT_CONNECT    120
#define Z_PORT_CONNECTED  121
#define Z_PORT_REFUSED    122
#define Z_PORT_DATA       123
#define Z_PORT_CLOSE      124
#define Z_PORT_DATA_ACK   125

// fallback pid for the demo virtual port (sw/apps/portdemo) if name
// lookup ("portdemo0") fails -- same convention as Z_PID_WM (zwm.h) /
// Z_PID_NET (znet.h). Stale as an actual fallback since `repl`
// replaced portdemo in sh.c's init() boot sequence (see
// sw/apps/repl/repl.c, sw/common/zrepl.h's Z_PID_REPL -- 3, the same
// slot this constant documents, since repl now starts where portdemo
// used to): portdemo is no longer started automatically at boot, so
// nothing actually lands here anymore unless you `run portdemo`
// manually, at whatever pid happens to be free at the time -- this
// constant is only meaningful again if something goes back to
// starting portdemo at a fixed, predictable point in the boot order.
// Left in place, not removed -- still accurate documentation of the
// convention itself, just not a live guarantee right now.
#define Z_PID_PORTDEMO   3

// how many z_port_send() calls can be outstanding (sent, not yet
// acked) at once, per connection -- see z_port_send()/
// z_port_handle_ack() (zport.c). Small and fixed on purpose: once
// full, z_port_send() itself refuses new sends (returns Z_FAIL)
// rather than growing this without bound -- see that function's own
// comment for why. In practice this rarely holds more than one or two
// entries at a time: a receiver like `term` consumes a DATA message's
// bytes synchronously, in the very call that reads it (vt_feed(),
// term.c), so the matching ack is usually only a scheduling slice or
// two behind the send -- this headroom is for a peer that's fallen
// further behind than that, not the common case.
#define Z_PORT_MAX_PENDING_SENDS 8

typedef struct {
	uint32_t peer_pid;	// who DATA/CLOSE go to -- the provider if
						// we're the client, the client if we're the
						// provider (z_port_connect()/z_port_accept()
						// each set this to the right one)
	uint32_t conn_id;	// used as the message tag for DATA/CLOSE
	bool connected;

	// z_port_send()'s own outstanding-blob bookkeeping -- a genuine
	// FIFO queue, freed in the order it was sent, on the strength of
	// two guarantees this codebase already provides: mailboxes are a
	// strict FIFO ring per sender/receiver pair (sw/os/msg.c), and
	// every current DATA receiver acks exactly once, in read order.
	// So the Nth ack to arrive always corresponds to the Nth still-
	// outstanding send -- "just free the oldest pending entry on any
	// ack" is correct without needing to identify which one by any
	// transmitted value at all (deliberately not by the payload's own
	// pointer: a pointer z_obj_blob() returns is only meaningful in
	// the SENDING process's own address space -- P.base + (vaddr -
	// 0x80000000) in physical terms, sw/os/msg.c's own header comment
	// -- and there's no way for the sender to redo that translation
	// itself later to check an ack against). The one thing this
	// approach genuinely depends on -- that an ack, once its receiver
	// decides to send one, actually arrives -- is z_port_send_ack()'s
	// own job to guarantee; see its comment for how. Not meant to be
	// read or written directly by callers, same as peer_pid/conn_id
	// aren't -- this table starts correctly empty the same way they
	// start correctly zeroed: every current caller relies on plain
	// `static`/global `z_port_t` storage (portdemo.c's `conn`,
	// term.c's `port`, each of repl.c's `conns[].port`) getting
	// zero-initialized `.bss`, confirmed reliable for every app binary
	// here (each one's own Makefile zero-pads its `.bin` up through
	// `_end` -- see docs/networking.md's "objcopy truncation bug" for
	// the one place that assumption was ever actually false, and why
	// it isn't anymore).
	struct {
		void *header;	// the z_blob_t* z_obj_blob() returned via
						// obj.val.ptr at send time -- the only thing
						// actually needed to free it (z_obj_free()
						// derives ->data from this itself)
	} pending[Z_PORT_MAX_PENDING_SENDS];
	uint32_t pending_head;	// index of the OLDEST outstanding entry --
							// only meaningful while pending_count > 0
	uint32_t pending_count;	// how many entries are currently queued,
							// 0..Z_PORT_MAX_PENDING_SENDS

} z_port_t;

// -- client side --

// ~2 seconds at the kernel tick rate (~732Hz -- see sw/os/kernel.c's
// z_kernel_ticks comment). Public (not zport.c-private) specifically
// so a caller needing a longer timeout for one particular connect
// (z_port_connect_arg_timeout() below) can still reference this exact
// default for every other connect it makes, rather than having to
// duplicate the number. See z_port_connect_arg_timeout()'s own
// comment for why one blanket timeout doesn't fit every provider.
#define Z_PORT_CONNECT_TIMEOUT_TICKS (732 * 2)

// connects to a provider at a well-known pid. blocks briefly (a
// bounded ~2 second timeout, NOT forever -- unlike z_msg_wait()'s own
// unbounded blocking, used elsewhere in this codebase for RPCs like
// z_win_create(), a port provider isn't guaranteed to even be running,
// so hanging indefinitely isn't acceptable here) waiting for
// CONNECTED or REFUSED. Like z_win_create(), call this during
// startup, before your own main message loop -- any unrelated message
// that arrives while waiting is silently discarded (same accepted
// limitation z_msg_wait() already has), so this isn't safe to call
// once you're also expecting other messages to arrive.
z_rv z_port_connect(z_port_t *port, uint32_t provider_pid);

// same as z_port_connect(), but lets the caller send a
// provider-specific argument as CONNECT's payload instead of the
// default Z_NONE -- see the protocol sketch above ("obj=Z_NONE (or
// provider-specific args)"). First use: sw/apps/net's telnet port
// provider, which needs to know a target IP before it can decide
// whether to accept the connection at all -- see docs/ports.md and
// sw/common/zterm.h's Z_TERM_SET_PORT (which carries this argument
// from whoever originated the request, e.g. `repl`'s `telnet <ip>`
// command, through to term's own z_port_connect_arg() call). Same
// borrowed-payload lifetime rule as any other message payload
// (docs/messaging.md) applies to `arg` -- it only needs to stay valid
// until this call returns (it's read once, synchronously, when
// building the CONNECT message).
//
// Uses the default ~2 second timeout (zport.c's own
// Z_PORT_CONNECT_TIMEOUT_TICKS) -- fine for a provider that's simply
// slow to get scheduled, wrong for one whose own CONNECT handling
// involves a slow async operation before it can reply either way. Use
// z_port_connect_arg_timeout() below instead for a provider like
// that (net.c's telnet port is exactly this case -- see that
// function's own comment).
z_rv z_port_connect_arg(z_port_t *port, uint32_t provider_pid, z_obj_t arg);

// same as z_port_connect_arg(), but with an explicit timeout instead
// of the default ~2 seconds -- for a provider whose own CONNECT
// handling can legitimately take a while before it knows whether to
// reply CONNECTED or REFUSED, rather than one that either answers
// almost immediately or isn't running at all. net.c's telnet port is
// the motivating case: it doesn't reply until an actual TCP handshake
// to a remote server resolves one way or the other, which can take up
// to tcp.c's own worst-case retry budget (~31.5s, TCP_RTO_TICKS_BASE/
// _MAX_SHIFT/_MAX_RETRIES there) -- the default 2s timeout meant
// `term`'s own connect always gave up locally before net's TCP layer
// ever got a chance to answer, even once net was working correctly on
// its own end. Found and fixed on real hardware: this is what that
// looked like from the outside (a telnet connect that always "timed
// out", regardless of how long tcp.c's own retries were given).
z_rv z_port_connect_arg_timeout(z_port_t *port, uint32_t provider_pid,
	z_obj_t arg, uint32_t timeout_ticks);

// -- client OR provider side, once connected --

// sends a chunk of data. fire-and-forget from the CALLER's
// perspective -- see docs/ports.md's "Flow control: an explicit,
// deliberate gap for v1" for why this still doesn't wait for
// anything or retry on its own; a caller with nothing better to do on
// Z_FAIL can just drop the chunk, same as before.
//
// Internally builds a Z_BLOB (z_obj_blob(), zobj.c -- a real heap
// copy of `data`) and tracks it in *port's own small pending-sends
// table until the peer's own z_port_send_ack() (called once ITS
// handler for the resulting Z_PORT_DATA has genuinely finished
// reading the payload) reports it's safe to free -- see
// z_port_handle_ack() below for the other half of this, and
// docs/messaging.md's "Known limitations" for the full design
// writeup, including why an earlier "just free the previous call's
// blob at the start of the next one" scheme was tried and reverted
// (a real, confirmed-on-hardware corruption bug: that assumption
// broke whenever a caller made several sends back-to-back with
// nothing in between to force a scheduler switch, e.g. `repl`'s own
// handle_connect() sending a banner then a prompt immediately).
//
// Returns Z_FAIL, and doesn't even attempt the send, if the pending
// table is already full (Z_PORT_MAX_PENDING_SENDS) -- a real, if
// rarely hit, backpressure case: it means the peer has fallen far
// enough behind on acking that letting this grow further would just
// reproduce the original unbounded-leak problem under a different
// name. Also frees the blob immediately, right here, if the
// underlying z_msg_send() itself fails (peer's mailbox full) -- that
// message was never delivered, so no ack for it will ever arrive; see
// z_port_handle_ack()'s own comment for why this is the one case that
// mechanism can't cover on its own.
z_rv z_port_send(z_port_t *port, const void *data, uint32_t len);

// tells the peer this connection is done. does not wait for any
// acknowledgment.
void z_port_close(z_port_t *port);

// call once your own handler for a received Z_PORT_DATA message has
// GENUINELY finished reading its payload -- not right after
// z_msg_read() returns. Those aren't the same moment: z_msg_read()
// only resolves the pointer, it doesn't mean your own handler code
// has actually read through the bytes yet, and under this system's
// preemptive scheduling (sw/os/kernel.c, KTIMER-driven round-robin)
// your handler could in principle be interrupted mid-read -- if the
// ORIGINAL SENDER got scheduled next and freed on an ack sent that
// early, it could free memory your own handler hasn't finished
// reading, reintroducing the exact corruption class z_port_send()'s
// own header comment describes. Call this at the point your handler
// would otherwise just return -- e.g. right after term.c's vt_feed()
// call, not right after the z_msg_read() that produced `data_msg`.
//
// Retries internally, with a short bounded timeout, if the underlying
// send fails -- unlike z_port_send() itself, this one CAN'T just give
// up and drop it. z_port_handle_ack() (below) trusts that every DATA
// message eventually gets exactly one ack, in order, to know which
// pending send to free -- a lost ack isn't just one missed
// notification, it permanently shifts that ordering for every send
// after it, since there's no longer a 1:1 correspondence between
// sends and acks for the sender to fall back on. Confirmed on real
// hardware: `z_port_send_ack()` originally didn't check its own
// z_msg_new_send() call's return value at all, and a receiver's
// mailbox being momentarily full (the same kind of keystroke burst
// documented in docs/ports.md's "Flow control" section) could drop an
// ack silently -- from that point on, every later ack the sender
// received got matched against the WRONG pending entry, compounding
// rather than self-correcting, and eventually reproducing the
// original unbounded-backpressure problem by a different path (each
// lost ack permanently costs one slot of Z_PORT_MAX_PENDING_SENDS,
// forever, even though nothing was actually still outstanding). The
// retry here is short and bounded on purpose -- an ack is tiny (no
// allocation at all, see below), and its only realistic failure mode
// is the receiving mailbox being transiently busy, which resolves
// within the receiver's own next scheduling slice or two, not
// something worth a long wait over. If it's still failing once the
// timeout's up, something worse than transient congestion is going
// on -- this logs it and gives up, at which point z_port_send()'s own
// backpressure (Z_PORT_MAX_PENDING_SENDS) is the correct backstop,
// same as it already is for a peer that's stopped acking entirely.
//
// Payload is just Z_NONE -- nothing needs to be identified (see
// z_port_handle_ack()'s own comment for why), so there's nothing to
// send beyond the bare fact that an ack happened.
//
// Safe to call for a zero-length or malformed DATA message too (a
// no-op in that case, nothing to ack).
void z_port_send_ack(const z_msg_t *data_msg);

// call from your own message loop when msg->subject ==
// Z_PORT_DATA_ACK, with the z_port_t the ack is for. If you're
// juggling more than one connection (e.g. repl.c's conns[]), find the
// right one by msg->tag == port->conn_id first -- same lookup
// Z_PORT_DATA/Z_PORT_CLOSE already need.
//
// Frees the OLDEST still-outstanding z_obj_blob() allocation *port's
// own earlier z_port_send() calls made -- a genuine FIFO queue (see
// z_port_t's own comment on `pending`), not matched against anything
// the ack itself carries. This works because mailboxes are FIFO per
// sender/receiver pair, every current DATA receiver acks exactly
// once, in the order it read each message, AND z_port_send_ack()
// itself guarantees that ack reliably arrives (its own comment
// explains why that last part specifically had to be guaranteed, not
// just assumed) -- so the Nth ack to arrive back always corresponds
// to the Nth still-outstanding send, with no need to identify which
// one by value at all.
//
// Safe to call defensively any time this subject shows up, not just
// when you're sure it's fresh and valid: a no-op if the port isn't
// connected, the tag doesn't match, or nothing is currently pending
// (a stale/duplicate ack).
void z_port_handle_ack(z_port_t *port, const z_msg_t *msg);

// -- provider side --

// call from a provider's own message-handling loop when a
// Z_PORT_CONNECT arrives. conn_id is the provider's own choice of how
// to identify this connection internally (e.g. an index into its own
// connection table) -- it becomes the tag on every DATA/CLOSE for
// this connection from here on, in both directions.
void z_port_accept(z_port_t *out_port, const z_msg_t *connect_msg, uint32_t conn_id);

// call instead of z_port_accept() to decline a connection (e.g. a
// provider that only supports one client at a time, already in use).
void z_port_refuse(const z_msg_t *connect_msg, const char *reason);

#endif
