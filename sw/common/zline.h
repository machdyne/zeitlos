#ifndef ZLINE_H
#define ZLINE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Line assembly for byte-at-a-time input over a duplex byte stream --
 * pulled out of sw/apps/lisp/lisp.c (the first thing that needed it)
 * so any other port provider (sw/common/zport.h, docs/ports.md) that
 * wants the same "type -> see what you typed -> press Enter -> get a
 * line" behavior doesn't have to reimplement it.
 *
 * Why this exists at all: a port CLIENT (like `term`) does NOT
 * locally echo or line-buffer once it's actually connected to a
 * provider -- see term.c's own header comment, that behavior is ONLY
 * term's no-port-connected fallback. Against a real provider, term
 * sends one Z_PORT_DATA per keystroke, raw, and draws exactly what
 * comes back and nothing more (see term.c's handle_key_event()). So a
 * provider that wants to look like an interactive command line (the
 * way a real UNIX pty's line discipline does for a shell) has to do
 * its own character-at-a-time echo, backspace handling, and line
 * buffering, PER CONNECTION. `portdemo` gets away without any of this
 * because it's a raw byte echo test harness, not a line-oriented
 * command interface -- see its own header comment, which admits
 * backspace visibly does nothing against it for exactly this reason.
 *
 * This module only assembles bytes into lines and produces the bytes
 * a provider should echo back for each one -- it knows nothing about
 * z_port_t, messaging, or what a completed line means once you have
 * one. One z_line_t per connection; a caller feeds bytes in one at a
 * time (as they arrive off Z_PORT_DATA -- a single DATA message may
 * bundle more than one byte, e.g. a pasted block, so feed each byte
 * of a payload through separately, in order) and sends whatever comes
 * back in `echo`/`*echo_len` down its own port, then acts on
 * `line->buf` once z_line_feed() reports a completed line.
 *
 * Deliberately NOT handling here: multi-line continuation (e.g.
 * waiting for balanced parens before treating input as "really"
 * complete) -- that depends on what the completed line MEANS, which
 * this module has no opinion on. A caller that needs that (`lisp`,
 * once real Scheme evaluation is wired in) layers it on top, by
 * choosing not to act on a "complete" line yet and instead
 * accumulating it into its own multi-line buffer.
 */

#define Z_LINE_MAX 128 // max chars per line, excluding the terminating NUL

typedef struct {
	char		buf[Z_LINE_MAX + 1];
	uint32_t	len;
	bool		had_cr;	// true right after a '\r' -- lets a following
						// '\n' (a real CRLF pair) be swallowed instead
						// of starting a second, empty line
} z_line_t;

// (re)initializes/clears a line buffer -- call once before the first
// byte ever fed to it, and again after z_line_feed() reports a
// completed line, before feeding the byte after that.
void z_line_reset(z_line_t *line);

// max bytes z_line_feed() can ever want to echo back for a single
// input byte -- the backspace erase sequence ("\x08 \x08") is the
// longest single-byte case, matching zeitlos.c's own readline()
// convention for the same visual trick. size an `echo` buffer passed
// to z_line_feed() to at least this.
#define Z_LINE_ECHO_MAX 4

// feeds one input byte through the line discipline.
//
//   returns 0 -- byte consumed, line not yet complete. `echo` has
//                been filled with 0+ bytes (see *echo_len) the caller
//                should send back down its own port -- may be 0
//                bytes (e.g. backspace against an already-empty
//                line, or a byte silently dropped because the line
//                is already at Z_LINE_MAX and isn't itself a line
//                terminator -- same as a real terminal dropping
//                input once a line's at its limit).
//   returns 1 -- line complete (this byte was '\r', or a standalone
//                '\n' not preceded by '\r'). `line->buf` is now the
//                completed line, NUL-terminated, the delimiter
//                itself not included. `echo` is filled with "\r\n"
//                so the caller's remote actually sees a new line
//                (real VT100 behavior needs both -- see zvt100.c's
//                '\r'/'\n' handling, plain '\n' alone doesn't return
//                the cursor to column 0). Call z_line_reset() before
//                feeding the byte after this one.
//
// `echo_cap` must be at least Z_LINE_ECHO_MAX; `*echo_len` is always
// set on return (0 if there's nothing to send back).
int z_line_feed(z_line_t *line, uint8_t byte,
	char *echo, uint32_t *echo_len, uint32_t echo_cap);

#endif
