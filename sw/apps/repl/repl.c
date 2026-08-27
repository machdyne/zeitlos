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
 *
 * Phase 5 (this revision): the "future text editor" mentioned just
 * above is no longer future -- `te` (sw/ext/te, a git submodule) is
 * now reachable via the `te <filename>` command. See docs/editor.md
 * for the full writeup (the -DTE_HOST_IO patch te.c needed, the new
 * Z_SYS_FS_* syscalls this required building since no app could read
 * or write a file before this, the memory-budget reasoning behind
 * te_bridge.c's file-size ceiling, and why only one `te` session can
 * be live at a time, process-wide). Unlike Scheme evaluation above,
 * `te` is NOT reachable through Z_REPL_EVAL -- it needs an actual
 * connection to own (screen output, input bytes, the in_editor
 * flag) that a bare eval request has none of; see the "te" command's
 * own handling in dispatch_line() below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>	// sbrk() -- the startup heap-usage log in main()
					// below (the `free` command that also used it is now a
					// Scheme procedure, see zapi.c)

#include "../../common/zeitlos.h"
#include "../../common/zport.h"
#include "../../common/zline.h"
#include "../../common/zrepl.h"
#include "../../common/zterm.h"
#include "../../common/zwm.h"
#include "../../common/zdns.h"
#include "ms_api.h"
#include "te_bridge.h"
#include "page.h"
#include "zapi.h"

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
	// true while THIS connection owns the (single, process-wide) live
	// `te` session -- see te_bridge.h's own header comment for why
	// only one connection can be "in te" at a time. When true,
	// handle_data() routes this connection's bytes straight to
	// te_bridge_feed() instead of the normal z_line_feed()/
	// dispatch_line() path below. Relies on zero-initialized .bss,
	// same as z_port_t's own fields already do (see zport.h's comment
	// on `pending` for why that's a safe assumption for every app
	// binary here).
	bool		in_editor;
	// true while THIS connection owns the (single, process-wide) live
	// `page` session -- exactly the same role in_editor plays for
	// `te`, and mutually exclusive with it in practice since neither
	// command can be typed while the other owns the connection. Kept
	// as a separate flag rather than folded into one "full-screen
	// mode" enum: the two route their bytes to different modules and
	// end on different conditions, so a shared flag would only have to
	// be disambiguated again at every use.
	bool		in_pager;
} repl_conn_t;

static repl_conn_t conns[Z_REPL_MAX_CONNS];

// One history, shared by every connection.
//
// Per-connection would be four copies of the same 1KB for something a
// single user wants to be the same everywhere: recalling a command in
// one terminal window that was typed in another is the behaviour, not
// a compromise. See z_line_hist_t in zline.h.
static z_line_hist_t line_history;

// -- echo batching --
//
// Echo bytes for a whole incoming Z_PORT_DATA are accumulated here
// and sent as ONE message, rather than one message per input byte.
//
// Per-byte was fine while input arrived at typing speed, and broke
// the moment paste existed. z_port_send() refuses once
// Z_PORT_MAX_PENDING_SENDS (8) messages are unacked (zport.h), and
// this loop feeds every byte of the paste without ever returning to
// read the acks -- so a paste of more than eight characters had its
// TAIL silently dropped from the display while landing correctly in
// the buffer. Nine characters pasted, exactly one missing.
//
// Batching also makes a paste one round trip instead of dozens.
#define ECHO_BATCH 512

static char echo_batch[ECHO_BATCH];
static uint32_t echo_batch_len;

static void echo_flush(repl_conn_t *c) {

	if (!echo_batch_len) return;

	if (z_port_send(&c->port, echo_batch, echo_batch_len) != Z_OK)
		printf("repl: echo z_port_send failed (%lu bytes) to pid %ld\n",
			(unsigned long)echo_batch_len, (long)c->port.peer_pid);

	echo_batch_len = 0;

}

static void echo_put(repl_conn_t *c, const char *b, uint32_t n) {

	// Flush before it could overflow. One line's worth of redraw is
	// the largest single contribution, so leaving that much headroom
	// means a batch is never split mid-sequence -- half an escape
	// sequence arriving on its own would be interpreted as text.
	if (echo_batch_len + n > ECHO_BATCH) echo_flush(c);

	if (n > ECHO_BATCH) return;		// cannot happen; refuse rather than smash

	for (uint32_t i = 0; i < n; i++) echo_batch[echo_batch_len++] = b[i];

}

static const char *BANNER =
	"repl -- Zeitlos command interpreter\r\n"
	"type 'help' for a list of commands -- anything else is "
	"evaluated as Scheme\r\n"
	// F12 is intercepted by `term` itself, before anything reaches a
	// port (see handle_key_event() in term.c) -- so it works even when
	// term is relaying raw bytes to a remote that has no quit command
	// of its own, which is exactly the situation someone needs it in
	// and exactly the situation where they can't be told about it.
	// Worth a line here, on the one screen every session starts at.
	"F12 returns to repl from any port (telnet, portdemo, ...)\r\n";

static const char *PROMPT = "> ";

// Continuation prompt, shown on every row of a form after the first.
//
// The SAME WIDTH as PROMPT, which zline requires: every row then
// starts at the same column, and the editor can repaint without
// asking the terminal where anything is. See z_line_set_multiline().
static const char *CONT_PROMPT = ". ";

// Is this input a complete Scheme form?
//
// Balanced parentheses, ignoring anything inside a string or after a
// line comment -- otherwise a `(` in "a (b" or in a ; comment would
// keep the reader waiting forever for a paren the user never intends
// to type.
//
// An excess of CLOSING parens counts as complete rather than as an
// error: the form is broken either way, and letting it through means
// the reader reports the problem, which is far more useful than the
// terminal silently refusing to accept the line.
static bool form_complete(const char *s, void *user) {

	(void)user;

	int depth = 0;
	bool in_str = false;

	for (uint32_t i = 0; s[i]; i++) {

		char c = s[i];

		if (in_str) {
			if (c == '\\' && s[i + 1]) i++;		// escaped char
			else if (c == '"') in_str = false;
			continue;
		}

		if (c == '"') { in_str = true; continue; }

		// A comment runs to the end of its row, not the end of the
		// whole buffer -- this input may be several rows.
		if (c == ';') {
			while (s[i] && s[i] != '\n') i++;
			if (!s[i]) break;
			continue;
		}

		if (c == '(' || c == '[') depth++;
		else if (c == ')' || c == ']') {
			depth--;
			if (depth < 0) return true;
		}

	}

	// An unterminated string keeps the form open too.
	return depth <= 0 && !in_str;

}

// Only the commands that genuinely CAN'T be Scheme procedures are
// listed as builtins -- each needs the requesting connection itself
// (`te`/`page` take it over for a full-screen session; `port`/`telnet`
// redirect it elsewhere; `quit` ends it), which a Scheme procedure has
// no access to. Everything else moved to the Scheme API and is listed
// under `scheme:`, so each command lives in exactly one place -- see
// docs/scheme_api.md.
//
// DELIBERATELY TERSE, and the guard below is why: dispatch_line()
// writes its response into a Z_REPL_EVAL_REPLY_MAX (256 byte) buffer
// (zrepl.h) via snprintf(), which truncates SILENTLY. The first
// version of this listing after `page`/`ps`/`free`/... were added ran
// to 357 bytes and lost its last third with no runtime symptom at all
// -- caught only by -Wformat-truncation. Rather than trust the next
// person to notice, the typedef below turns an over-long help string
// into a compile ERROR (a negative array size), naming this comment.
//
// If this genuinely needs to grow past the cap, the options are: raise
// Z_REPL_EVAL_REPLY_MAX (it's a wire constant -- every REPL_EVAL peer
// must be rebuilt together, see zrepl.h), or stop routing help through
// `out` and send it straight down the connection in chunks (which a
// bare REPL_EVAL request, having no connection, then couldn't get).
// Shortening the text is the cheap answer and has been the right one
// so far.
static const char HELP_TEXT[] =
	"builtins: help ping echo te <f> page <f> port <n> telnet <h> scheme <e> quit\r\n"
	"scheme:   ls ps free uptime run kill load mkdir touch-file delay-ms ...\r\n"
	"F12 returns here from any port; bare word = call, so `ps` is (ps)\r\n"
	"anything else is evaluated as Scheme";

// build-time check -- see HELP_TEXT's own comment. A negative array
// size rather than _Static_assert(): this tree builds with --std=gnu99
// and this idiom needs no C11.
typedef char repl_help_text_fits_in_reply_buffer[
	(sizeof(HELP_TEXT) <= Z_REPL_EVAL_REPLY_MAX) ? 1 : -1];

// set once in main(), after the one and only ms_init_lix() attempt --
// see there for what happens if it fails (an undersized MS_HEAP_SIZE
// for whatever's actually in ms_stdlib.l is the realistic failure
// mode, see docs/scheme.md). Checked by the "scheme" command below so
// a boot-time failure degrades to a clear, static error message
// forever after, rather than every subsequent "scheme" command
// hitting ms_eval() against an interpreter that was never actually
// initialized.
static bool scheme_ready = false;

// copies `s` into `out` (capacity `out_cap`), and when it doesn't fit
// makes that VISIBLE instead of silently dropping the tail.
//
// This exists because silent truncation has now bitten this file
// twice: once in the `help` text (357 bytes into a 256 byte buffer,
// losing its last third with no symptom), and once in Scheme results
// -- `(free)` prints to ~293 bytes and `(ps)` on a full process table
// to ~770, both of which used to just stop mid-token, looking for all
// the world like the output was corrupt rather than merely cut off.
// Z_REPL_REPLY_MAX (zrepl.h) is now large enough for both, but a
// Scheme value has no upper bound in general -- (ls) on a large
// directory, or any user expression -- so the honest thing is to say
// so when it happens rather than pretend it didn't.
static void copy_or_mark_truncated(char *out, uint32_t out_cap, const char *s) {

	static const char MARK[] = " ...[truncated]";

	if (!out || !out_cap) return;

	uint32_t len = (uint32_t)strlen(s);

	if (len < out_cap) {
		memcpy(out, s, len + 1);
		return;
	}

	// pathologically small buffer -- nothing useful fits alongside the
	// marker, so just say what happened.
	if (out_cap <= sizeof(MARK)) {
		snprintf(out, out_cap, "[truncated]");
		return;
	}

	// sizeof(MARK) counts its NUL, so keep + sizeof(MARK) == out_cap
	// exactly: every byte used, still NUL-terminated.
	uint32_t keep = out_cap - sizeof(MARK);
	memcpy(out, s, keep);
	memcpy(out + keep, MARK, sizeof(MARK));

}

// -- Scheme stdout capture (z_stdout_hook, sw/common/zeitlos.h) --
//
// `ms`'s printing procedures -- display, write, print, newline, gc and
// dump -- all write to stdout, which on this OS means the serial
// console. For someone running them from a `term` window that is
// simply the wrong screen: (dump) printed ~200 symbol names onto the
// UART and showed the user nothing.
//
// While an interactive command is being dispatched, stdout is
// redirected here, ACCUMULATED into a buffer, and sent to the
// requesting connection once dispatch has finished. Buffering rather
// than sending per write() matters for two reasons: ms issues many
// small fputs() calls (dump emits one per symbol), which would be one
// z_port_send() each and blow past Z_PORT_MAX_PENDING_SENDS
// (zport.h's own flow-control note) -- the same batching te_bridge.c
// already does for te; and sending from inside the hook risks
// re-entering it, since a failed z_port_send() logs with printf().
//
// The normal path therefore never sends while the hook is installed.
// Only an overflowing buffer does, and cap_flushing guards that case
// by routing any nested write back to the UART instead of recursing.
// Sized so the largest realistic output goes out in ONE z_port_send().
// That's not just efficiency: z_port_send() refuses once
// Z_PORT_MAX_PENDING_SENDS (8, zport.h) sends are unacked, and no ack
// can arrive mid-command -- repl only processes Z_PORT_DATA_ACK back
// in its main loop, which it doesn't reach until this whole dispatch
// finishes. So the real budget is 8 sends per command, shared with the
// reply and the prompt.
//
// (dump) is the worst case by a wide margin: ~210 bound symbols after
// the stdlib loads, ~1.5KB of names and wrapping. At 512 bytes that
// was 3 sends of a ~6 send budget, and it would have grown quietly
// tighter with every builtin added. 2KB makes it one.
static repl_conn_t	*cap_conn = NULL;	// where captured output goes
static char			cap_buf[2048];
static uint32_t		cap_len = 0;
static bool			cap_flushing = false;	// re-entrancy guard
static bool			cap_last_was_cr = false;

static void conn_send_str(repl_conn_t *c, const char *s);

// sends whatever has accumulated, with the hook temporarily removed so
// a send failure's own printf() can't come back through here.
static void cap_flush(void) {

	if (!cap_len) return;

	z_stdout_hook_t saved = z_stdout_hook;
	z_stdout_hook = NULL;
	cap_flushing = true;

	if (cap_conn && cap_conn->port.connected) {
		if (z_port_send(&cap_conn->port, cap_buf, cap_len) != Z_OK) {
			// out of pending-send slots, or the peer's mailbox is
			// full. Fall back to the console rather than dropping the
			// bytes: output landing in the wrong place is a nuisance,
			// output vanishing with only a one-line warning is the
			// silent-truncation failure mode this file has already
			// been bitten by three times.
			printf("repl: captured-output send failed (%lu bytes) -- "
				"falling back to console:\n", (unsigned long)cap_len);
			fwrite(cap_buf, 1, cap_len, stdout);
		}
	} else {
		// no connection to show it on (it went away mid-command) --
		// fall back to the console rather than dropping the output on
		// the floor entirely.
		fwrite(cap_buf, 1, cap_len, stdout);
	}

	cap_len = 0;
	cap_flushing = false;
	z_stdout_hook = saved;

}

static void cap_putc(char c) {
	if (cap_len >= sizeof(cap_buf)) cap_flush();
	cap_buf[cap_len++] = c;
}

// Expands a BARE '\n' into "\r\n" for the VT100 at the far end, while
// leaving an existing "\r\n" alone -- ms.c mixes the two conventions
// (bi_dump's line wrapping emits "\r\n" explicitly, while `newline`
// emits a plain "\n"), and blindly expanding every '\n' would turn the
// former into "\r\r\n".
static void repl_stdout_hook(const char *data, uint32_t len) {

	if (cap_flushing) {
		// a nested write from inside cap_flush() -- send it to the
		// console rather than recursing back into the buffer we are
		// in the middle of emptying.
		fwrite(data, 1, len, stdout);
		return;
	}

	for (uint32_t i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\n' && !cap_last_was_cr) cap_putc('\r');
		cap_putc(c);
		cap_last_was_cr = (c == '\r');
	}

}

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
		// NOT snprintf(): a printed Scheme value has no bounded size,
		// and quietly losing its tail reads as corruption rather than
		// as truncation -- see copy_or_mark_truncated() above.
		copy_or_mark_truncated(out, out_cap, s);
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

// -- bare-word command syntax (docs/scheme_api.md \S1) --
//
// translates e.g. "tget 1.2.3.4 firmware.bin" into
// `(tget "1.2.3.4" "firmware.bin")` -- but only actually attempted by
// dispatch_line() below when the first word is bound to something
// callable (ms_is_callable()); this function itself doesn't check
// that (its only caller already does, right before using its result).
//
// Argument classification, one whitespace-separated token at a time:
//   - already starts with '"' or '('  -> passed through unquoted (the
//     user already wrote real Scheme for this one argument)
//   - "#t"/"#f"/"#true"/"#false"       -> passed through unquoted
//   - parses ENTIRELY as a number (strtod consumes the whole token,
//     not just a prefix -- so e.g. "192.168.1.1", which strtod reads
//     as 192.168 and stops at the second '.', correctly falls through
//     to the quoted-string case below instead of becoming a bare
//     192.168) -> passed through unquoted, becomes a Scheme number
//   - anything else                    -> wrapped in "..." as a
//     string literal, with '"' and '\' escaped defensively
//
// There's no way to pass a bare symbol/variable reference through
// this syntax at all -- `line mywin 10 10` sends the STRING "mywin",
// never the value of a variable named mywin. Write real Scheme
// (`(line mywin 10 10 ...)`) for that; `scheme <expr>` is also always
// available as an explicit bypass of this whole function.
//
// Returns false (out untouched) if `line` doesn't tokenize (no first
// word at all) or if the translated form would overflow `out_cap` --
// dispatch_line() falls back to evaluating `line` completely
// unchanged in either case, same as if this function didn't exist.
static bool translate_command_line(const char *line, char *out, uint32_t out_cap) {

	const char *p = line;
	while (*p == ' ') p++;
	const char *cmd_start = p;
	while (*p && *p != ' ') p++;
	uint32_t cmd_len = (uint32_t)(p - cmd_start);

	if (cmd_len == 0 || cmd_len >= Z_LINE_MAX) return false;

	char cmd[Z_LINE_MAX + 1];
	memcpy(cmd, cmd_start, cmd_len);
	cmd[cmd_len] = 0;

	if (!ms_is_callable(cmd)) return false;

	uint32_t o = 0;
#define Z_EMIT(c) do { if (o + 1 >= out_cap) return false; out[o++] = (char)(c); } while (0)
#define Z_EMIT_STR(s) do { for (const char *_q = (s); *_q; _q++) Z_EMIT(*_q); } while (0)

	Z_EMIT('(');
	Z_EMIT_STR(cmd);

	while (*p) {

		while (*p == ' ') p++;
		if (!*p) break;

		const char *tok_start = p;
		while (*p && *p != ' ') p++;
		uint32_t tok_len = (uint32_t)(p - tok_start);

		char tok[Z_LINE_MAX + 1];
		if (tok_len >= sizeof(tok)) return false;
		memcpy(tok, tok_start, tok_len);
		tok[tok_len] = 0;

		Z_EMIT(' ');

		if (tok[0] == '"' || tok[0] == '(') {
			Z_EMIT_STR(tok);
			continue;
		}

		if (!strcmp(tok, "#t") || !strcmp(tok, "#f") ||
			!strcmp(tok, "#true") || !strcmp(tok, "#false")) {
			Z_EMIT_STR(tok);
			continue;
		}

		char *endptr = NULL;
		strtod(tok, &endptr);
		if (endptr && *endptr == 0 && endptr != tok) {
			Z_EMIT_STR(tok);
			continue;
		}

		Z_EMIT('"');
		for (const char *q = tok; *q; q++) {
			if (*q == '"' || *q == '\\') Z_EMIT('\\');
			Z_EMIT(*q);
		}
		Z_EMIT('"');

	}

	Z_EMIT(')');
	out[o] = 0;

#undef Z_EMIT
#undef Z_EMIT_STR

	return true;

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
// `conn` is the connection this line arrived from -- handle_data()
// passes the connection's own repl_conn_t*, handle_eval() passes NULL
// (a bare REPL_EVAL request has no connection, and therefore no
// specific term to redirect, and nothing to hand editor-mode
// ownership to -- see the "port"/"telnet"/"te" commands below, the
// only things that currently care about this parameter at all).
// `requester_pid` (derived below) is what those commands actually
// need most of the time -- kept as a local rather than changing every
// existing call site, since `conn ? conn->port.peer_pid : 0` reads
// worse repeated inline than named once.
static bool dispatch_line(const char *line, char *out, uint32_t out_cap,
	repl_conn_t *conn) {

	uint32_t requester_pid = conn ? conn->port.peer_pid : 0;

	while (*line == ' ') line++; // skip leading spaces

	if (*line == 0) {
		out[0] = 0;
		return false;
	}

	if (!strcmp(line, "help")) {
		snprintf(out, out_cap, "%s", HELP_TEXT);
		return false;
	}

	if (!strcmp(line, "ping")) {
		snprintf(out, out_cap, "pong");
		return false;
	}

	// NOTE: `uptime` and `free` used to be builtins right here. Both
	// are Scheme procedures now (zapi.c) -- they return real values
	// instead of pre-formatted text, and the bare-word command syntax
	// below means typing `uptime` or `free` still works exactly as it
	// did. `free` also reports considerably more than it used to (the
	// kernel pool as well as this process's own heaps).

	if (!strncmp(line, "echo ", 5)) {
		snprintf(out, out_cap, "%s", line + 5);
		return false;
	}

	if (!strcmp(line, "echo")) {
		out[0] = 0;
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

	if (!strncmp(line, "te ", 3)) {

		const char *target = line + 3;
		while (*target == ' ') target++;

		if (*target == 0) {
			snprintf(out, out_cap, "usage: te <filename>");
			return false;
		}

		if (!conn) {
			// same reasoning as "port"/"telnet" above -- a bare
			// REPL_EVAL request has no connection to hand editor-mode
			// ownership to, and nowhere for te's own VT100 screen
			// output to go.
			snprintf(out, out_cap,
				"repl: 'te' only works from an interactive term "
				"connection, not a REPL_EVAL request");
			return false;
		}

		char reason[128];
		if (te_bridge_start(&conn->port, target, reason, sizeof(reason))) {
			// te_bridge_start() has already sent the editor's own
			// first screen down this connection -- nothing more to
			// say here, and no PROMPT either (handle_data() checks
			// conn->in_editor itself to skip that, see there).
			conn->in_editor = true;
			out[0] = 0;
		} else {
			snprintf(out, out_cap, "te: %s", reason);
		}

		return false;

	}

	if (!strcmp(line, "te")) {
		snprintf(out, out_cap, "usage: te <filename>");
		return false;
	}

	// `page` -- the read-only counterpart to `te` above, for files far
	// too large for te's TE_MAX_FILE_SIZE ceiling (see page.h and
	// docs/editor.md). Structurally identical to the `te` case: it
	// takes over this connection for a full-screen VT100 session, so
	// it needs a real connection and can't be a Scheme procedure.
	if (!strncmp(line, "page ", 5)) {

		const char *target = line + 5;
		while (*target == ' ') target++;

		if (*target == 0) {
			snprintf(out, out_cap, "usage: page <filename>");
			return false;
		}

		if (!conn) {
			// same reasoning as "te"/"port"/"telnet" above -- a bare
			// REPL_EVAL request has no connection to hand session
			// ownership to, and nowhere for the pager's own screen
			// output to go.
			snprintf(out, out_cap,
				"repl: 'page' only works from an interactive term "
				"connection, not a REPL_EVAL request");
			return false;
		}

		char reason[128];
		if (page_start(&conn->port, target, reason, sizeof(reason))) {
			// page_start() has already drawn the first screen --
			// nothing to say here, and no PROMPT either (handle_data()
			// checks conn->in_pager itself to skip it, see there).
			conn->in_pager = true;
			out[0] = 0;
		} else {
			snprintf(out, out_cap, "page: %s", reason);
		}

		return false;

	}

	if (!strcmp(line, "page")) {
		snprintf(out, out_cap, "usage: page <filename>");
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

	// no builtin matched -- try bare-word command syntax
	// (docs/scheme_api.md \S1) before falling back to evaluating the
	// line exactly as typed: e.g. "ls" -> "(ls)",
	// `tget 1.2.3.4 f` -> `(tget "1.2.3.4" "f")` -- but ONLY if the
	// first word is CURRENTLY bound to something callable
	// (ms_is_callable(), sw/ext/ms/ms.c) -- deliberately no separate
	// curated keyword list to maintain (docs/scheme_api.md \S1c: "any
	// bound procedure", not an allow-list). An unbound first word, or
	// one bound to something non-callable (a plain variable), falls
	// through completely unchanged -- eval_scheme() sees exactly
	// `line` in that case, same as always (this preserves e.g.
	// `myvar` alone still just printing myvar's value, not attempting
	// to call it).
	char translated[Z_LINE_MAX * 2 + 8];
	if (scheme_ready && translate_command_line(line, translated, sizeof(translated))) {
		eval_scheme(translated, out, out_cap);
	} else {
		eval_scheme(line, out, out_cap);
	}
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
	z_line_set_history(&conns[slot].line, &line_history);

	// Multi-line entry: Enter on an unbalanced form starts a new row
	// instead of submitting. See form_complete() above and
	// z_line_set_multiline() in zline.h.
	z_line_set_multiline(&conns[slot].line, form_complete, NULL,
		(uint32_t)strlen(PROMPT), CONT_PROMPT);

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

	if (c) {

		uint32_t len = z_blob_len(&msg->obj);
		uint8_t *data = (uint8_t *)z_blob_data(&msg->obj);

		if (data && len) {

			// a single DATA message is usually one keystroke (see this
			// file's own header comment), but feed every byte through
			// in order regardless -- correct either way, and doesn't
			// assume anything about how many bytes a future, non-term
			// client might bundle into one message (e.g. a pasted
			// block, or -- the common case for this specific check --
			// a multi-byte VT100 escape sequence like an arrow key
			// while this connection is in the `te` editor below).
			for (uint32_t i = 0; i < len; i++) {

				// while this connection owns the live `te` session
				// (te_bridge.h), every byte goes straight to it,
				// bypassing the normal line-editing/dispatch path
				// entirely -- bytes from any OTHER connection still
				// go through that path below regardless, so repl
				// stays responsive to its other windows while one is
				// editing. This check is per-BYTE, not per-message,
				// for the same reason z_line_feed() below needs
				// bytes fed one at a time.
				if (c->in_editor) {

					if (!te_bridge_feed(data[i])) {
						// Esc :q just ended the session -- clear the
						// screen (te's own last redraw is still up)
						// and drop this connection back into normal
						// line mode.
						c->in_editor = false;
						conn_send_str(c,
							"\r\n" VT100_ERASE_SCREEN VT100_CURSOR_HOME
							"te: session ended\r\n");
						conn_send_str(c, PROMPT);
						z_line_reset(&c->line);
					}

					continue;

				}

				// same per-byte check, and same reasoning, as
				// in_editor just above -- while this connection is
				// paging, its bytes drive the viewer instead of the
				// line editor, and every OTHER connection keeps
				// working normally throughout.
				if (c->in_pager) {

					if (!page_feed(data[i])) {
						// 'q' just ended the session -- page_feed()
						// has already restored the screen itself.
						c->in_pager = false;
						conn_send_str(c, PROMPT);
						z_line_reset(&c->line);
					}

					continue;

				}

				char echo[Z_LINE_ECHO_MAX];
				uint32_t echo_len;
				int complete =
					z_line_feed(&c->line, data[i], echo, &echo_len, sizeof(echo));

				if (echo_len) echo_put(c, echo, echo_len);

				if (!complete) continue;

				// A line is about to be executed, and its output goes
				// down the same port -- so the echo has to be on the
				// wire first or the two arrive out of order.
				echo_flush(c);

				// Remember it, now that it is a real submitted line.
				// z_line_feed() deliberately does not do this itself
				// -- only the caller knows a line was worth keeping,
				// and blank ones are dropped by
				// z_line_history_add().
				z_line_history_add(&line_history, c->line.buf);

				// Z_REPL_REPLY_MAX, not the smaller REPL_EVAL wire
				// cap -- this is repl's own stack buffer for an
				// interactive reply and has no peer to stay
				// compatible with, so it's sized for what real
				// commands actually print (see zrepl.h).
				char out[Z_REPL_REPLY_MAX];

				// capture anything the command prints to stdout and
				// show it HERE rather than on the serial console --
				// see repl_stdout_hook() above. Installed only for the
				// interactive path: a REPL_EVAL request (handle_eval())
				// has no connection to show output on, so its stdout
				// keeps going to the console exactly as before.
				cap_conn = c;
				cap_len = 0;
				cap_last_was_cr = false;
				z_stdout_hook = repl_stdout_hook;

				bool wants_quit =
					dispatch_line(c->line.buf, out, sizeof(out), c);

				// newlib buffers stdout (_isatty() reports a tty, so
				// it's line-buffered) -- anything ms printed without a
				// trailing newline is still sitting in libc's own
				// buffer and has not reached the hook yet. Flush it
				// through while the hook is STILL installed, then take
				// the hook down before emptying our own buffer.
				fflush(stdout);
				z_stdout_hook = NULL;

				if (c->in_editor || c->in_pager) {
					// dispatch_line() just handed this connection to
					// `te` or `page`, which has already drawn its own
					// full screen -- appending repl's captured
					// diagnostics on top of it would corrupt that
					// display, so send them to the console instead.
					if (cap_len) fwrite(cap_buf, 1, cap_len, stdout);
					cap_len = 0;
				} else {
					cap_flush();
				}

				cap_conn = NULL;

				if (out[0]) {
					conn_send_str(c, out);
					conn_send_str(c, "\r\n");
				}

				if (wants_quit) {
					z_port_close(&c->port);
					// c->port.connected is now false -- if more bytes
					// from this same peer are still queued behind this
					// DATA message (unlikely, but not impossible), the
					// tag won't match a connected slot anymore and
					// find_conn_by_tag() will just drop them next
					// time, same as any other post-close stray DATA.
					break;
				}

				z_line_reset(&c->line);

				if (c->in_editor || c->in_pager) {
					// dispatch_line() just started a `te` or `page`
					// session on this connection -- it already sent
					// that session's own first screen, so don't ALSO
					// print the normal PROMPT on top of it.
					continue;
				}

				conn_send_str(c, PROMPT);

			}

			// Anything still buffered goes out now.
			//
			// This is the paste case: a block of characters that does
			// not complete a line leaves its whole echo sitting in the
			// batch, and without this the text would not appear until
			// the next keystroke.
			echo_flush(c);

		}

	}

	// tells whoever sent this it's now safe to free its own
	// z_obj_blob() allocation -- see z_port_send_ack()'s own comment
	// (zport.h) for why this has to come after every branch above has
	// genuinely finished reading `data` (it's a safe no-op for a
	// stale/unrecognized connection, or an empty/malformed payload --
	// neither of those branches ever touch `data` at all). One call
	// site covering every way this function can finish, on purpose,
	// rather than one per early exit, so it can't accidentally get
	// missed if this function's control flow changes later.
	z_port_send_ack(msg);

}

static void handle_close(const z_msg_t *msg) {
	repl_conn_t *c = find_conn_by_tag(msg->tag);
	if (!c) return;
	if (c->in_editor) {
		// this connection's own `term` disappeared (crashed, closed,
		// whatever) while it owned the live `te` session -- release
		// the process-wide editor lock rather than leaving every
		// future `te` command permanently refused with "already in
		// use". No attempt to save first: there's no connection left
		// to report success/failure back to, and te_bridge_abort()'s
		// own contract is explicit about not trying -- same accepted
		// "unclean disconnect loses unsaved changes" tradeoff any
		// real editor has.
		te_bridge_abort();
		c->in_editor = false;
	}
	if (c->in_pager) {
		// same reasoning as the `te` case just above -- release the
		// process-wide pager lock rather than leaving every future
		// `page` command refused with "already open". This one also
		// closes the still-open file handle, which is a real (if
		// small) resource: handles live in a bounded kernel-side table
		// with no process-exit sweep (sw/common/zfs.h).
		page_abort();
		c->in_pager = false;
	}
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
	dispatch_line(input, out, sizeof(out), NULL); // NULL = no requesting
		// term connection -- see dispatch_line()'s own header comment
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
		// registers every Zeitlos-specific procedure (ls, read-file,
		// ...) into the same shared ms_global_env stdlib just loaded
		// into -- see zapi.h/docs/scheme_api.md. Still inside this
		// function's own protected region (this file's own comment
		// on eval_scheme() explains why that's required for anything
		// that can reach ms_log(MS_PANIC, ...), which
		// ms_def_builtin() can in principle do via its own
		// alloc_cell() calls on a genuinely exhausted heap) --
		// lumped into the same all-or-nothing early-boot check as
		// the stdlib load itself, rather than a separate try/catch,
		// since a heap too small for THIS is already a hard failure
		// either way.
		zapi_register();
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
			} else if (msg.subject == Z_PORT_DATA_ACK) {
				repl_conn_t *c = find_conn_by_tag(msg.tag);
				if (c) z_port_handle_ack(&c->port, &msg);
			} else if (msg.subject == Z_PORT_CLOSE) {
				handle_close(&msg);
			} else if (msg.subject == Z_REPL_EVAL) {
				handle_eval(&msg);
			} else if (msg.subject == Z_WM_CLOSE) {
				// titlebar close icon clicked on one of THIS repl's
				// Scheme-created windows (zapi.c's zapi_win_create(),
				// (win-create ...) in Scheme -- see Z_WM_CLOSE's own
				// comment in zwm.h for why wm sends this instead of
				// just destroying the window itself: repl can own
				// several windows off this one pid, so only the
				// specific window that was clicked should go away,
				// never the others and never repl itself). obj is a
				// Z_UINT32 window id -- see zapi_win_close() (zapi.h)
				// for what actually happens with it.
				if (msg.obj.type == Z_UINT32)
					zapi_win_close((int)msg.obj.val.uint32);
			}

		}
	}

	return 0;

}
