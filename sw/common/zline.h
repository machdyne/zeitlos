#ifndef ZLINE_H
#define ZLINE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Line assembly for byte-at-a-time input over a duplex byte stream --
 * pulled out of sw/apps/repl/repl.c (the first thing that needed it)
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
 * -- multi-line --
 *
 * A line may span several terminal rows. Pressing Enter asks the
 * CALLER whether the input is finished (z_line_complete_fn); if it
 * says no, a newline is inserted and editing continues on a fresh
 * row, with the cursor free to move back up into what was already
 * typed.
 *
 * The decision stays with the caller because "finished" depends
 * entirely on what the text means -- balanced parentheses for a
 * Scheme form, a closing quote, a trailing backslash. This module has
 * no opinion on any of that and should not acquire one.
 *
 * -- editing --
 *
 * This grew from "backspace and Enter" into a real line editor:
 * cursor movement, insert in the middle, delete, kill, and history
 * recall. That belongs HERE rather than in the terminal, because
 * `term` is a dumb terminal -- it sends VT100 sequences and draws
 * what comes back (see docs/terminal.md). Whatever reads those bytes
 * decides what they mean.
 *
 * It deliberately did NOT go into readline() in sw/common/zeitlos.c,
 * which stays minimal: that is the serial console's reader, and the
 * console is the recovery path of last resort. Complexity there is
 * complexity in the one thing that has to work when nothing else
 * does.
 *
 * -- everything is relative --
 *
 * No absolute cursor positioning is ever emitted, and no assumption
 * is made about the prompt. This module has no idea how wide the
 * caller's prompt is or which column the input starts in, so redraws
 * are expressed as "write these characters, erase to end of line,
 * move back N". That keeps it correct behind a prompt of any width,
 * including one that changes (a continuation prompt, say).
 */

#define Z_LINE_MAX 128 // max chars per line, excluding the terminating NUL

// Recallable previous lines.
//
// Supplied by the CALLER rather than embedded in z_line_t, because
// one is wanted per user, not per connection: `repl` serves up to
// four terminals and sharing one history between them is both what a
// single-user machine wants and 3KB cheaper. A caller that wants
// separate histories just passes separate instances, and one that
// wants none passes NULL.
#define Z_LINE_HIST 8

typedef struct {
	char	entries[Z_LINE_HIST][Z_LINE_MAX + 1];
	uint8_t	count;		// how many slots are filled, up to Z_LINE_HIST
	uint8_t	next;		// ring write position
} z_line_hist_t;

// Escape-sequence parser state. Arrow keys arrive as ESC [ A and the
// like (term.c's key_to_bytes()), spread across three separate
// z_line_feed() calls, so the parse has to be resumable.
typedef enum {
	Z_LINE_ESC_NONE = 0,
	Z_LINE_ESC_ESC,			// saw ESC
	Z_LINE_ESC_CSI,			// saw ESC [ or ESC O, reading parameters
} z_line_esc_t;

typedef bool (*z_line_complete_fn)(const char *s, void *user);

typedef struct {
	char		buf[Z_LINE_MAX + 1];
	uint32_t	len;

	// Cursor position within buf, 0..len. Separate from len, which
	// is the whole point of an editor: they were one variable when
	// this could only append.
	uint32_t	pos;

	bool		had_cr;	// true right after a '\r' -- lets a following
						// '\n' (a real CRLF pair) be swallowed instead
						// of starting a second, empty line

	uint8_t		esc;	// z_line_esc_t
	uint32_t	esc_p;	// numeric parameter accumulated so far

	// History, optional. `at` is Z_LINE_HIST_NONE while editing a
	// fresh line, otherwise how many entries back the display
	// currently shows.
	z_line_hist_t *hist;
	int			hist_at;

	// The line being typed, stashed while browsing backwards through
	// history, so that coming forward again returns it rather than
	// losing it -- which is what every shell does and what fingers
	// expect.
	char		stash[Z_LINE_MAX + 1];
	uint32_t	stash_len;

	// -- multi-line --
	//
	// See z_line_set_multiline(). `drawn_rows` is how many rows the
	// last repaint put on screen, so the next one knows how much to
	// erase -- a buffer that just got shorter leaves rows behind
	// otherwise.
	z_line_complete_fn	complete;
	void				*complete_user;
	uint32_t			prompt_w;
	const char			*cont_prompt;
	uint32_t			drawn_rows;

	// Which screen row the cursor is PHYSICALLY on, counted from the
	// first row of the input.
	//
	// Not the same as the row `pos` falls on, and that difference is
	// the whole reason this exists: an edit that removes a newline
	// changes the logical row of the cursor before anything has been
	// redrawn, while the terminal's cursor has not moved at all. A
	// repaint that navigated by the logical row would start writing
	// one row too low and leave the original behind.
	uint32_t			screen_row;

} z_line_t;

#define Z_LINE_HIST_NONE (-1)

// "Is this input finished?" -- asked when Enter is pressed.
//
// Returning false inserts a newline and keeps editing instead of
// submitting. `s` is the whole buffer, newlines included.

// Enables multi-line input.
//
// `prompt_w` is the printed width of the caller's prompt, and the
// continuation prompt MUST be the same width -- every row then starts
// at the same column, which is what keeps the geometry uniform enough
// to repaint without querying the terminal.
//
// This is the one thing zline needs to know about the caller's
// prompt, and it is only needed in multi-line mode: single-line
// editing stays entirely relative (see the header comment above).
//
// `fn` NULL disables multi-line, and Enter always submits.
void z_line_set_multiline(z_line_t *line, z_line_complete_fn fn,
	void *user, uint32_t prompt_w, const char *cont_prompt);

// Attaches (or detaches, with NULL) a history buffer. Call after
// z_line_reset(), which does not touch it.
void z_line_set_history(z_line_t *line, z_line_hist_t *hist);

// Records a completed line in `hist`.
//
// Called by the CALLER, not by z_line_feed(), because only the caller
// knows whether a line was worth remembering -- a blank line, or one
// consumed by a pager or an editor session rather than executed, is
// not. Consecutive duplicates are dropped, as in every shell.
void z_line_history_add(z_line_hist_t *hist, const char *s);

// (re)initializes/clears a line buffer -- call once before the first
// byte ever fed to it, and again after z_line_feed() reports a
// completed line, before feeding the byte after that.
void z_line_reset(z_line_t *line);

// max bytes z_line_feed() can ever want to echo back for a single
// input byte -- the backspace erase sequence ("\x08 \x08") is the
// longest single-byte case, matching zeitlos.c's own readline()
// convention for the same visual trick. size an `echo` buffer passed
// to z_line_feed() to at least this.
// Redrawing after an edit in the middle of a line means echoing the
// rest of the line back, so this has to be able to hold one -- plus
// the erase-to-end and cursor-move sequences around it.
#define Z_LINE_ECHO_MAX (Z_LINE_MAX + 32)

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
