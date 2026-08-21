/*
 * repl -- Zeitlos's command interpreter app.
 *
 * This is NOT sw/os/sh.c (the kernel shell) -- sh.c stays exactly
 * what it already is, a method of last resort always available on
 * the serial console, independent of whether anything in here is
 * even running. `repl` is a separate, ordinary app: a port provider
 * (sw/common/zport.h, docs/ports.md) that `term` connects to, running
 * Machdyne Scheme (sw/ext/ms, a git submodule -- see docs/scheme.md)
 * as its primary language, with system APIs (filesystem, graphics,
 * messaging) eventually reachable from Scheme so first-class Scheme
 * apps become possible -- see this project's own planning notes for
 * the full multi-phase roadmap.
 *
 * Phase 1: proved out the two pieces of plumbing every later phase
 * depends on -- multiple concurrent port connections (docs/ports.md's
 * own "Open questions" flagged this directly), and per-connection
 * line editing living HERE, not in `term` (see sw/common/zline.h's
 * own header comment for why). No Scheme dependency at all yet, a
 * small fixed table of builtin commands stood in for it.
 *
 * Phase 4 (this revision): brings `ms` in for real -- see
 * docs/scheme.md for the memory-layout reasoning (why it's built with
 * -DMS_STATIC_HEAP, not upstream's default malloc'd heap) and the
 * submodule/build setup. `ms_init_lix()` runs once, here, at startup,
 * against ONE shared `ms_global_env` -- every port connection's
 * commands, and every REPL_EVAL request, evaluate against the same
 * global state (a user who wants a truly separate environment can run
 * a second `repl` instance instead, see pidreg -- much simpler, and
 * avoids paying multiple Scheme heaps' worth of memory, a real cost
 * on a 1MB-RAM board, for isolation nothing has asked for yet).
 * Any line that doesn't match a builtin command is evaluated as
 * Scheme by default (dispatch_line()'s final fallback) -- builtins
 * always win on a match first, Scheme gets everything else. One real
 * limitation as of this revision: each line is read and evaluated as
 * exactly one Scheme form on its own, with no multi-line/paren-
 * balance continuation across separate lines yet -- a form split
 * across more than one line of typed input isn't handled (see
 * zline.h's own header comment on why that continuation logic is
 * deliberately NOT this module's job, it layers on top, still to be
 * added).
 *
 * Besides the port protocol, this app also answers Z_REPL_EVAL
 * messages (sw/common/zrepl.h) -- a second, non-interactive way to
 * reach the exact same command dispatcher, meant for a C-based app
 * (a future text editor, most obviously) that already has a chunk of
 * source text in memory and just wants it run, without pretending to
 * be a human typing at a port. See zrepl.h's own header comment for
 * the wire format; handle_eval() below is the provider side of it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>	// sbrk() -- see the "free" command below

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zline.h"
#include "../../common/zrepl.h"
#include "../../common/zterm.h"
#include "../../common/zdns.h"
#include "ms_api.h"

// hostname/IP resolution for the "telnet <ip-or-hostname>" command
// below now goes through sw/common/zdns.h's z_resolve_host() -- see
// its own header comment. Used to be a private parse_ipv4() copy
// here (IP-only, no hostname support), duplicated from sw/os/sh.c's
// own copy purely because there was nowhere shared both build
// contexts (this app vs. the kernel) could reach -- zdns.c's
// dual-build trick (same one sw/common/zstream.c already used)
// finally gave both a real shared home, so both copies were deleted.

// how many simultaneous port connections (i.e. `term` windows) this
// instance will accept -- small on purpose for phase 1, matching
// Z_MAILBOX_DEPTH's own "small on purpose, meant to be drained
// promptly" reasoning (zmsg.h). Raise this once real usage says it
// needs to be higher; a rejected 5th connection ("too many
// connections", see handle_connect() below) is a clean, visible
// failure, not a crash.
#define Z_REPL_MAX_CONNS 4

typedef struct {
	z_port_t	port;
	z_line_t	line;
} repl_conn_t;

static repl_conn_t conns[Z_REPL_MAX_CONNS];

static const char *BANNER =
	"repl -- Zeitlos command interpreter\r\n"
	"type 'help' for a list of commands -- anything else is "
	"evaluated as Scheme\r\n";

static const char *PROMPT = "> ";

// set once in main(), after the one and only ms_init_lix() attempt --
// see there for what happens if it fails (an undersized MS_HEAP_SIZE
// for whatever's actually in ms_stdlib.l is the realistic failure
// mode, see docs/scheme.md). Checked by the "scheme" command below so
// a boot-time failure degrades to a clear, static error message
// forever after, rather than every subsequent "scheme" command
// hitting ms_eval() against an interpreter that was never actually
// initialized.
static bool scheme_ready = false;

// -- Scheme evaluation --
//
// The ENTIRE protected region -- ms_panic_before_try(), the
// setjmp(ms_panic_recovery), and every call that can panic (ms_read/
// ms_eval/ms_to_string) -- has to happen directly in this function's
// own stack frame, per ms_api.h's own header comment on why: a
// longjmp() back into ms_panic_recovery can only land in a stack
// frame that's still live, and this function doesn't return until
// after the protected region is done one way or the other, so that's
// satisfied here. What's NOT allowed is factoring setjmp() itself
// into a further-nested helper that returns before ms_eval() actually
// runs -- this function doesn't do that, everything stays inline.
//
// writes the printed result (or an error message) into `out`,
// NUL-terminated, no trailing newline. `expr` is a single Scheme
// form's worth of source text -- multi-line/paren-balance handling
// isn't implemented yet (phase 5, see this file's own header
// comment), so anything spanning more than one line of typed input
// isn't reachable through this command yet either.
static void eval_scheme(const char *expr, char *out, uint32_t out_cap) {

	if (!scheme_ready) {
		snprintf(out, out_cap,
			"repl: Scheme isn't available (failed to initialize at "
			"startup -- see the boot log on the serial console)");
		return;
	}

	ms_panic_before_try();

	if (setjmp(ms_panic_recovery) == 0) {

		const char *p = expr;
		ms_val *form = ms_read(&p);

		if (!form) {
			snprintf(out, out_cap, "repl: couldn't parse that");
			return;
		}

		ms_val *result = ms_eval(form, ms_global_env);
		char *s = ms_to_string(result, true);
		snprintf(out, out_cap, "%s", s);
		free(s); // ms_to_string() malloc's this -- see ms_api.h

	} else {

		// a panic longjmp'd back here -- ms_log() (inside ms.c) has
		// already printed its own "[panic] ..." line to this
		// process's own stderr (its UART debug console, not the
		// port/connection that asked for this eval), so this is
		// deliberately generic rather than trying to recover and
		// reformat whatever ms_log() said.
		ms_panic_after_recover();
		snprintf(out, out_cap,
			"repl: Scheme error (see the serial console for details)");

	}

}

// -- command dispatch: the ONE thing both the port path (handle_data,
// one line at a time, per connection) and the message path
// (handle_eval, one request at a time, no connection at all) call
// into. This is deliberately the single place later phases change: a
// future version replaces (or wraps) this function's body with real
// ms_eval() against the shared global environment, falling back to
// this exact builtin table only when Scheme itself doesn't recognize
// the input -- see this file's own header comment. Everything above
// this function (connection handling, line assembly, the message
// protocol) shouldn't need to change at all when that happens.

// executes one line of input, writes a human-readable response into
// `out` (NUL-terminated; no trailing prompt or newline -- callers add
// those themselves however fits their own protocol). returns true if
// the input was a request to end the CALLER's session (currently only
// "quit"/"exit") -- meaningless and safely ignored by handle_eval(),
// which has no session to end.
//
// `requester_pid` is the pid of the `term` (or whatever) instance
// this line arrived from -- handle_data() passes the connection's own
// `port.peer_pid`, handle_eval() passes 0 (a bare REPL_EVAL request
// has no connection, and therefore no specific term to redirect --
// see the "port" command below, the only thing that currently cares
// about this parameter at all).
static bool dispatch_line(const char *line, char *out, uint32_t out_cap,
	uint32_t requester_pid) {

	while (*line == ' ') line++; // skip leading spaces

	if (*line == 0) {
		out[0] = 0;
		return false;
	}

	if (!strcmp(line, "help")) {
		snprintf(out, out_cap,
			"commands: help, ping, uptime, echo <text>, free, "
			"port <name>, telnet <ip-or-host>, quit\r\n"
			"anything else is evaluated as Scheme, e.g. (+ 1 2)");
		return false;
	}

	if (!strcmp(line, "ping")) {
		snprintf(out, out_cap, "pong");
		return false;
	}

	if (!strcmp(line, "uptime")) {
		snprintf(out, out_cap, "%lu ticks since boot",
			(unsigned long)z_uptime_ticks());
		return false;
	}

	if (!strncmp(line, "echo ", 5)) {
		snprintf(out, out_cap, "%s", line + 5);
		return false;
	}

	if (!strcmp(line, "echo")) {
		out[0] = 0;
		return false;
	}

	if (!strcmp(line, "free")) {

		// _end/_start: the same linker-provided symbols
		// docs/app_runtime.md describes, and zeitlos.c's own _sbrk()
		// already uses the same `extern char _end;` convention for --
		// _end is the top of this process's static footprint
		// (code+data+.bss, right where k_proc_create()'s own size
		// request, sh.c's fs_size(), came from at boot -- see
		// docs/app_runtime.md); _start is this process's fixed
		// virtual base (0x80000000, same for every app, riscv-app.ld)
		// -- their difference is exactly the static size, the same
		// number kernel.c computes for ITS OWN binary the same way
		// (`(uint32_t)&_end - (uint32_t)&_start` in main(), see
		// k_proc_create()'s very first call site).
		//
		// sbrk(0) (newlib, backed by zeitlos.c's own _sbrk()) returns
		// the current break WITHOUT growing it -- the standard
		// "just tell me where it is" idiom. The gap between that and
		// _end is everything malloc()'d since boot -- for `repl`
		// specifically, that's ms's own T_STR/T_VECTOR cell payloads
		// (ms.c's own type comments -- those two types own
		// malloc'd memory, unlike every other ms_val, which lives
		// entirely inside the fixed cell heap below) plus anything
		// else this file itself ever malloc()s (nothing, currently).
		//
		// what this does NOT show: the kernel's own view of this
		// process's total allocated block (k_mem_alloc() rounds up to
		// at least a 32KB minimum, mem.h's Z_MEM_MIN_BLOCK_SIZE) --
		// there's no syscall yet for a process to ask the kernel that
		// about itself. Left for later ("we can expose the API
		// later").
		extern char _end, _start;
		uint32_t static_footprint = (uint32_t)&_end - (uint32_t)&_start;
		uint32_t heap_grown = (uint32_t)sbrk(0) - (uint32_t)&_end;

		if (scheme_ready) {
			long used = ms_heap_used();
			long total = MS_HEAP_SIZE;
			uint32_t cell_bytes = (uint32_t)ms_cell_size();
			snprintf(out, out_cap,
				"scheme heap:  %ld/%ld cells used (~%lu/%lu KB)\r\n"
				"c heap:       %lu bytes grown via malloc (strings/vectors)\r\n"
				"static:       %lu bytes (code+data+bss, fixed at build)",
				used, total,
				(unsigned long)((uint32_t)used * cell_bytes / 1024),
				(unsigned long)((uint32_t)total * cell_bytes / 1024),
				(unsigned long)heap_grown, (unsigned long)static_footprint);
		} else {
			snprintf(out, out_cap,
				"scheme heap:  unavailable (Scheme failed to initialize)\r\n"
				"c heap:       %lu bytes grown via malloc\r\n"
				"static:       %lu bytes (code+data+bss, fixed at build)",
				(unsigned long)heap_grown, (unsigned long)static_footprint);
		}

		return false;
	}

	if (!strncmp(line, "port ", 5)) {

		const char *target = line + 5;
		while (*target == ' ') target++;

		if (*target == 0) {
			snprintf(out, out_cap,
				"usage: port <name>  (e.g. port portdemo0)");
			return false;
		}

		if (!requester_pid) {
			// a bare REPL_EVAL request, not a real term connection --
			// see this function's own header comment on why there's
			// nothing to redirect in that case.
			snprintf(out, out_cap,
				"repl: 'port' only works from an interactive term "
				"connection, not a REPL_EVAL request");
			return false;
		}

		// see sw/common/zterm.h for the full protocol/reasoning --
		// fire-and-forget, no reply. term.c closes ITS side of the
		// current connection itself (connect_port(), term.c) before
		// attempting the new one -- but this side (repl's own) closes
		// proactively too, right here (the `return true` below --
		// same "end this connection" signal "quit" uses, see
		// handle_data()'s own handling of it), rather than waiting
		// for term's own Z_PORT_CLOSE to arrive: repl is the one
		// telling this peer to leave, so there's no reason to still
		// send a PROMPT down a connection that's already ending on
		// purpose, the way handle_data() otherwise would for any
		// response that doesn't end the session.
		//
		// NOTE: term will very likely never actually SHOW the
		// "disconnecting now" text below on screen -- Z_TERM_SET_PORT
		// is sent first (this line), and once term's own main loop
		// processes it, connect_port() (term.c) blocks inside
		// z_port_connect(), which discards any other queued message
		// (including the Z_PORT_DATA that's about to carry this exact
		// response, plus the Z_PORT_CLOSE right after it) while it
		// waits for the NEW connection's own CONNECTED reply.
		// Harmless -- just don't be surprised the confirmation text
		// doesn't linger on screen; the real evidence the switch
		// worked is the new provider's own banner showing up right
		// after.
		z_msg_new_send(requester_pid, Z_TERM_SET_PORT, 0, z_obj_str(target));
		snprintf(out, out_cap,
			"requested switch to port '%s' -- disconnecting now", target);
		return true;

	}

	if (!strcmp(line, "port")) {
		snprintf(out, out_cap,
			"usage: port <name>  (e.g. port portdemo0, port repl1)");
		return false;
	}

	if (!strncmp(line, "telnet ", 7)) {

		const char *target = line + 7;
		while (*target == ' ') target++;

		if (*target == 0) {
			snprintf(out, out_cap,
				"usage: telnet <ip-or-hostname>  (e.g. telnet 192.168.178.100, "
				"telnet myserver.local)");
			return false;
		}

		if (!requester_pid) {
			// same reasoning as "port" above -- nothing to redirect
			// for a bare REPL_EVAL request, there's no term
			// connection behind it.
			snprintf(out, out_cap,
				"repl: 'telnet' only works from an interactive term "
				"connection, not a REPL_EVAL request");
			return false;
		}

		// z_resolve_host() (sw/common/zdns.h) tries a plain dotted-
		// quad parse first (instant, no messaging) and only falls
		// back to an actual DNS query -- via `net`'s dns.c, see
		// znet.h's Z_NET_DNS_RESOLVE -- if that fails. That fallback
		// blocks this call for up to a few seconds in the worst case
		// (no response/net not running/genuine NXDOMAIN) -- see
		// z_dns_resolve()'s own header comment. Worth knowing here
		// specifically: repl's own main loop (below) services EVERY
		// connected port/REPL_EVAL request from one shared mailbox,
		// so a slow hostname lookup from one `term` window stalls
		// repl's response to every OTHER connected window too, not
		// just this one, for as long as it blocks. Bounded and rare
		// in practice (a real nameserver on a local network answers
		// in single-digit milliseconds, and a literal IP never
		// touches this path at all), but a real cost, not a
		// theoretical one -- worth revisiting with a real
		// non-blocking resolve-then-connect flow if it ever proves
		// to matter with several people using `term` at once.
		uint32_t ip;
		char err[64];
		if (!z_resolve_host(target, &ip, err, sizeof(err))) {
			snprintf(out, out_cap, "telnet: %s", err);
			return false;
		}

		// same SET_PORT mechanism the "port" command above uses --
		// just the Z_MAP form (sw/common/zterm.h) so `net`
		// (sw/apps/net) gets the target IP as part of the CONNECT
		// itself (sw/common/zport.h's z_port_connect_arg()), not a
		// separate message racing against this one. "net0" is net's
		// own pidreg name (net.c registers itself under "net", same
		// convention as repl/term); if net isn't running, term's own
		// name lookup just fails and it stays in local echo, same as
		// any other unreachable "port" target -- no fixed-pid
		// fallback here, matching "port <name>"'s own precedent
		// above (unlike term's OWN startup connection to
		// "repl0"/Z_PID_REPL, which does have one).
		//
		// `arg` (like `port`'s own z_obj_str(target) just above) is
		// intentionally never freed -- a fire-and-forget message with
		// a heap-allocated payload is only safe long-term if nothing
		// ever reuses or frees the memory out from under a receiver
		// that hasn't read it yet (docs/messaging.md); leaving it
		// permanently allocated sidesteps that lifetime question at
		// the cost of a small, deliberate, already-accepted-elsewhere
		// leak (one map + one string per `telnet`/`port` command
		// typed, not a per-byte or per-message cost).
		z_obj_t arg = z_obj_map(2);
		z_map_set(&arg, "name", z_obj_str("net0"));
		z_map_set(&arg, "arg", z_obj_uint32(ip));
		z_msg_new_send(requester_pid, Z_TERM_SET_PORT, 0, arg);

		// shows the resolved address alongside whatever was typed --
		// makes it obvious when `target` was a hostname (as opposed
		// to already being the IP itself, where this is redundant but
		// harmless) which actual address the connection is using.
		snprintf(out, out_cap,
			"connecting to %s (%ld.%ld.%ld.%ld) -- disconnecting now "
			"(F12 returns to repl)",
			target,
			(long)((ip >> 24) & 0xFF), (long)((ip >> 16) & 0xFF),
			(long)((ip >> 8) & 0xFF), (long)(ip & 0xFF));
		return true;

	}

	if (!strcmp(line, "telnet")) {
		snprintf(out, out_cap,
			"usage: telnet <ip-or-hostname>  (e.g. telnet 192.168.178.100, "
			"telnet myserver.local)");
		return false;
	}

	if (!strncmp(line, "scheme ", 7)) {
		eval_scheme(line + 7, out, out_cap);
		return false;
	}

	if (!strcmp(line, "scheme")) {
		snprintf(out, out_cap, "usage: scheme <expr>  (e.g. scheme (+ 1 2))");
		return false;
	}

	if (!strcmp(line, "quit") || !strcmp(line, "exit")) {
		snprintf(out, out_cap, "bye");
		return true;
	}

	// no builtin matched -- default to Scheme. builtins above always
	// win on an exact/prefix match first (so e.g. "help" always shows
	// the builtin help text, never an "unbound symbol: help" Scheme
	// error, even though nothing stops a user from later `define`ing
	// their own Scheme binding named help -- the builtin table simply
	// isn't reachable through Scheme at all, they're two separate
	// namespaces that only happen to share dispatch here) -- but
	// anything else, "the whole line" is handed to eval_scheme()
	// as-is. The explicit "scheme <expr>" command above still works
	// too (mostly redundant now, kept for explicitness/scripting and
	// because it costs nothing to leave in) -- this is just the same
	// path with the "scheme " prefix no longer required.
	eval_scheme(line, out, out_cap);
	return false;

}

// -- port (interactive) side --

static void conn_send_str(repl_conn_t *c, const char *s) {
	uint32_t len = (uint32_t)strlen(s);
	if (!len) return;
	// z_port_send() can legitimately fail (peer's mailbox full, or
	// disconnected -- docs/ports.md's own "explicit, deliberate gap
	// for v1") -- log it rather than silently dropping the bytes with
	// no trace anywhere, same as this file already does for TFTP-style
	// failures elsewhere in the codebase.
	if (z_port_send(&c->port, s, len) != Z_OK)
		printf("repl: conn_send_str failed (%lu bytes) to pid %ld\n",
			(unsigned long)len, (long)c->port.peer_pid);
}

static void handle_connect(const z_msg_t *msg) {

	int slot = -1;
	for (int i = 0; i < Z_REPL_MAX_CONNS; i++) {
		if (!conns[i].port.connected) { slot = i; break; }
	}

	if (slot < 0) {
		z_port_refuse(msg, "repl: too many connections");
		return;
	}

	// conn_id = slot+1, never 0 -- 0 is reserved for the
	// CONNECT/CONNECTED exchange itself (see zport.h)
	z_port_accept(&conns[slot].port, msg, (uint32_t)(slot + 1));
	z_line_reset(&conns[slot].line);

	printf("repl: connection %d accepted (pid %ld)\n", slot, (long)msg->from);

	conn_send_str(&conns[slot], BANNER);
	conn_send_str(&conns[slot], PROMPT);

}

static repl_conn_t *find_conn_by_tag(uint32_t tag) {
	for (int i = 0; i < Z_REPL_MAX_CONNS; i++) {
		if (conns[i].port.connected && conns[i].port.conn_id == tag)
			return &conns[i];
	}
	return NULL;
}

static void handle_data(const z_msg_t *msg) {

	repl_conn_t *c = find_conn_by_tag(msg->tag);
	if (!c) return; // stale DATA against an already-closed connection

	uint32_t len = z_blob_len(&msg->obj);
	uint8_t *data = (uint8_t *)z_blob_data(&msg->obj);
	if (!data || !len) return;

	// a single DATA message is usually one keystroke (see this
	// file's own header comment), but feed every byte through in
	// order regardless -- correct either way, and doesn't assume
	// anything about how many bytes a future, non-term client might
	// bundle into one message (e.g. a pasted block).
	for (uint32_t i = 0; i < len; i++) {

		char echo[Z_LINE_ECHO_MAX];
		uint32_t echo_len;
		int complete =
			z_line_feed(&c->line, data[i], echo, &echo_len, sizeof(echo));

		if (echo_len) {
			// see conn_send_str()'s own comment above -- same
			// reasoning applies here.
			if (z_port_send(&c->port, echo, echo_len) != Z_OK)
				printf("repl: echo z_port_send failed (%lu bytes) to pid %ld\n",
					(unsigned long)echo_len, (long)c->port.peer_pid);
		}

		if (!complete) continue;

		char out[Z_REPL_EVAL_REPLY_MAX];
		bool wants_quit =
			dispatch_line(c->line.buf, out, sizeof(out), c->port.peer_pid);

		if (out[0]) {
			conn_send_str(c, out);
			conn_send_str(c, "\r\n");
		}

		if (wants_quit) {
			z_port_close(&c->port);
			// c->port.connected is now false -- if more bytes from
			// this same peer are still queued behind this DATA
			// message (unlikely, but not impossible), the tag won't
			// match a connected slot anymore and handle_data()'s own
			// early-return above will just drop them, same as any
			// other post-close stray DATA.
			return;
		}

		conn_send_str(c, PROMPT);
		z_line_reset(&c->line);

	}

}

static void handle_close(const z_msg_t *msg) {
	repl_conn_t *c = find_conn_by_tag(msg->tag);
	if (!c) return;
	c->port.connected = false;
	printf("repl: connection closed by peer\n");
}

// -- REPL_EVAL (message-based, non-interactive) side -- see
// sw/common/zrepl.h for the wire format and reasoning.

static void handle_eval(const z_msg_t *msg) {

	char input[Z_REPL_EVAL_REPLY_MAX];

	if (msg->obj.type == Z_STR && msg->obj.val.str) {

		snprintf(input, sizeof(input), "%s", msg->obj.val.str);

	} else if (msg->obj.type == Z_BLOB) {

		uint32_t len = z_blob_len(&msg->obj);
		uint8_t *data = (uint8_t *)z_blob_data(&msg->obj);
		if (len >= sizeof(input)) len = sizeof(input) - 1;
		if (data && len) memcpy(input, data, len);
		input[len] = 0;

	} else {

		z_msg_new_send(msg->from, Z_REPL_ERROR, msg->tag, z_obj_str(
			"repl: REPL_EVAL requires a Z_STR or Z_BLOB payload"));
		return;

	}

	char out[Z_REPL_EVAL_REPLY_MAX];
	dispatch_line(input, out, sizeof(out), 0); // 0 = no requesting term
		// connection -- see dispatch_line()'s own header comment
	// dispatch_line()'s "wants to quit" return is deliberately
	// ignored here -- there's no connection/session for a bare
	// REPL_EVAL request to end.

	z_msg_new_send(msg->from, Z_REPL_RESULT, msg->tag, z_obj_str(out));

}

int main(void) {

	char instance_name[24] = "repl";
	if (z_pid_register("repl", instance_name, sizeof(instance_name)))
		printf("repl: starting as pid %ld, registered as '%s'.\n",
			(long)z_getpid(), instance_name);
	else
		printf("repl: starting as pid %ld (name registration failed, "
			"term won't be able to find this instance by name).\n",
			(long)z_getpid());

	// one-time Scheme init, same setjmp-in-this-frame requirement as
	// eval_scheme() above -- covers a panic during the STDLIB load
	// itself (ms_stdlib.l failing to even define its own functions
	// would be a real, if unlikely, failure mode), not just later
	// per-command evals. scheme_ready stays false on any failure here
	// -- see its own comment above -- rather than retrying, since a
	// failure this early (most likely: MS_HEAP_SIZE too small for
	// ms_stdlib.l to finish loading, see docs/scheme.md) isn't
	// something retrying without a rebuild would fix.
	ms_panic_before_try();
	if (setjmp(ms_panic_recovery) == 0) {
		ms_init_lix(true);
		scheme_ready = true;
		printf("repl: Scheme ready (%d cells, %d protect-stack slots)\n",
			MS_HEAP_SIZE, MS_PROTECT_STACK_SIZE);
		// Logs how much of the shared stack+heap region
		// (Z_PROC_STACK_SIZE, sw/os/kernel.c) stdlib loading alone
		// consumes -- there's no separate heap region at all (see
		// that constant's own comment for why), so this number
		// directly says how much headroom is actually left for
		// everything else this process will ever malloc() (Scheme's
		// own T_STR/T_VECTOR values, and every zport.h z_port_send()
		// call via zobj.c's z_obj_blob()) before hitting real trouble.
		extern char _end;
		uint32_t heap_grown = (uint32_t)sbrk(0) - (uint32_t)&_end;
		printf("repl: heap grown %lu bytes by end of stdlib load\n",
			(unsigned long)heap_grown);
	} else {
		ms_panic_after_recover();
		printf("repl: Scheme failed to initialize (non-fatal -- "
			"builtin commands still work, 'scheme' will report "
			"unavailable)\n");
	}

	for (int i = 0; i < Z_REPL_MAX_CONNS; i++)
		conns[i].port.connected = false;

	while (1) {

		z_msg_t msg;
		while (z_msg_read(&msg) == Z_OK) {

			if (msg.subject == Z_PORT_CONNECT) {
				handle_connect(&msg);
			} else if (msg.subject == Z_PORT_DATA) {
				handle_data(&msg);
			} else if (msg.subject == Z_PORT_CLOSE) {
				handle_close(&msg);
			} else if (msg.subject == Z_REPL_EVAL) {
				handle_eval(&msg);
			}

		}

	}

	return 0;

}
