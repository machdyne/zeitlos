/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zline.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zline.h"

void z_line_reset(z_line_t *line) {
	line->len = 0;
	line->buf[0] = 0;
	line->had_cr = false;
}

int z_line_feed(z_line_t *line, uint8_t byte,
	char *echo, uint32_t *echo_len, uint32_t echo_cap) {

	*echo_len = 0;

	// CR -- always ends a line, and sets had_cr so a following LF (a
	// real CRLF pair, e.g. from a real telnet peer someday) doesn't
	// start a second, empty line.
	if (byte == '\r') {
		line->buf[line->len] = 0;
		line->had_cr = true;
		if (echo_cap >= 2) {
			echo[0] = '\r'; echo[1] = '\n';
			*echo_len = 2;
		}
		return 1;
	}

	// LF -- ends a line UNLESS it's the second half of a CRLF pair
	// this function already completed on the CR half of (had_cr is
	// only ever true immediately after that CR, and gets cleared by
	// every other byte below, so it can't leak across an intervening
	// character).
	if (byte == '\n') {
		if (line->had_cr) {
			line->had_cr = false;
			return 0;
		}
		line->buf[line->len] = 0;
		if (echo_cap >= 2) {
			echo[0] = '\r'; echo[1] = '\n';
			*echo_len = 2;
		}
		return 1;
	}

	line->had_cr = false;

	// backspace -- either raw DEL (0x7f, what a real terminal
	// actually sends, see term.c's key_to_bytes()/its own comment on
	// why) or classic BS (0x08) -- accept both.
	if (byte == 0x7f || byte == 0x08) {
		if (line->len > 0) {
			line->len--;
			if (echo_cap >= 3) {
				echo[0] = 0x08; echo[1] = ' '; echo[2] = 0x08;
				*echo_len = 3;
			}
		}
		return 0;
	}

	// anything else: only accept printable ASCII, and only if there's
	// still room (leaving space for the NUL) -- silently drop (no
	// echo) otherwise. other control bytes (tab, ESC-led sequences,
	// etc.) aren't handled at all yet -- a caller that needs them
	// (e.g. arrow-key recall of previous lines) will need its own
	// layer on top, same as multi-line continuation (see zline.h's
	// own header comment).
	if (byte < 0x20 || byte >= 0x7f) return 0;
	if (line->len >= Z_LINE_MAX) return 0;

	line->buf[line->len++] = (char)byte;
	if (echo_cap >= 1) {
		echo[0] = (char)byte;
		*echo_len = 1;
	}

	return 0;

}
