/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Line editing for byte-at-a-time input. See zline.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zline.h"
#include "zutf8.h"

void z_line_reset(z_line_t *line) {

	line->buf[0] = 0;
	line->len = 0;
	line->pos = 0;
	line->had_cr = false;
	line->esc = Z_LINE_ESC_NONE;
	line->esc_p = 0;
	line->utf8_len = 0;
	line->utf8_need = 0;
	line->hist_at = Z_LINE_HIST_NONE;
	line->stash[0] = 0;
	line->stash_len = 0;
	line->drawn_rows = 1;
	line->screen_row = 0;

	// hist is deliberately NOT cleared -- it outlives the line being
	// typed, which is the entire point of it.

}

void z_line_set_multiline(z_line_t *line, z_line_complete_fn fn,
	void *user, uint32_t prompt_w, const char *cont_prompt) {

	line->complete = fn;
	line->complete_user = user;
	line->prompt_w = prompt_w;
	line->cont_prompt = cont_prompt;
	line->drawn_rows = 1;
	line->screen_row = 0;

}

void z_line_set_history(z_line_t *line, z_line_hist_t *hist) {
	line->hist = hist;
	line->hist_at = Z_LINE_HIST_NONE;
}

static uint32_t str_len(const char *s) {
	uint32_t n = 0;
	while (s[n]) n++;
	return n;
}

static void str_copy(char *dst, const char *src, uint32_t cap) {
	uint32_t i = 0;
	for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = 0;
}

void z_line_history_add(z_line_hist_t *hist, const char *s) {

	if (!hist || !s || !s[0]) return;

	// Drop consecutive duplicates, as every shell does -- pressing
	// Enter twice on the same command should not fill the history
	// with it.
	if (hist->count) {
		uint8_t last = (uint8_t)((hist->next + Z_LINE_HIST - 1) % Z_LINE_HIST);
		const char *p = hist->entries[last];
		uint32_t i = 0;
		while (p[i] && s[i] && p[i] == s[i]) i++;
		if (!p[i] && !s[i]) return;
	}

	str_copy(hist->entries[hist->next], s, Z_LINE_MAX + 1);

	hist->next = (uint8_t)((hist->next + 1) % Z_LINE_HIST);
	if (hist->count < Z_LINE_HIST) hist->count++;

}

// `n` entries back from the most recent, or NULL if there aren't that
// many.
static const char *hist_get(const z_line_hist_t *hist, int n) {

	if (!hist || n < 0 || n >= (int)hist->count) return NULL;

	int idx = ((int)hist->next - 1 - n + Z_LINE_HIST * 2) % Z_LINE_HIST;

	return hist->entries[idx];

}

// -- row geometry --
//
// A row is a run of the buffer between newlines. These are the only
// place that knows the buffer may contain them; everything below
// works in terms of "the current row", which keeps the single-line
// paths unchanged.

static uint32_t row_start_of(const z_line_t *line, uint32_t at) {

	uint32_t s = 0;

	for (uint32_t i = 0; i < at; i++)
		if (line->buf[i] == '\n') s = i + 1;

	return s;

}

static uint32_t row_end_of(const z_line_t *line, uint32_t at) {

	uint32_t i = at;

	while (i < line->len && line->buf[i] != '\n') i++;

	return i;

}

// Which row `at` falls on, counting from 0.
static uint32_t row_index_of(const z_line_t *line, uint32_t at) {

	uint32_t n = 0;

	for (uint32_t i = 0; i < at; i++)
		if (line->buf[i] == '\n') n++;

	return n;

}

static uint32_t row_count(const z_line_t *line) {
	return row_index_of(line, line->len) + 1;
}

static bool multiline(const z_line_t *line) {
	return line->complete != NULL;
}

// Columns, not bytes.
//
// `pos` is a byte offset into the buffer. The terminal's cursor moves
// in columns, and those two numbers are equal only while every
// character is one byte wide. Every distance this file emits, and
// every place a row change lands the cursor, goes through here so the
// buffer and the screen cannot drift apart.

static uint32_t cols_span(const char *s, uint32_t n) {
	if (!n) return 0;
	return (uint32_t)z_utf8_cols(s, n);
}

static uint32_t col_of(const z_line_t *line, uint32_t start, uint32_t at) {
	if (at <= start) return 0;
	return cols_span(line->buf + start, at - start);
}

// Byte offset of display column `col` on the row that begins at
// `start`. Never lands in the middle of a character: a column that
// falls in the right half of a wide one stays on that character.
static uint32_t pos_at_col(const z_line_t *line, uint32_t start, uint32_t col) {

	uint32_t end = row_end_of(line, start);
	uint32_t i = start;
	uint32_t c = 0;

	while (i < end && c < col) {

		uint32_t n = (uint32_t)z_utf8_next_off(line->buf, (int)line->len, (int)i);
		int w;

		if (n <= i) break;

		w = z_utf8_cols(line->buf + i, n - i);

		// A combining mark takes no column. Skipping it keeps the
		// walk moving; counting it as one would disagree with
		// z_utf8_cols() about where the cursor is.
		if (w <= 0) { i = n; continue; }

		if (c + (uint32_t)w > col) return i;

		c += (uint32_t)w;
		i = n;

	}

	return i;

}

// -- echo emitter --
//
// Everything written here is RELATIVE. See zline.h on why: this
// module has no idea how wide the caller's prompt is.

typedef struct {
	char		*buf;
	uint32_t	cap;
	uint32_t	len;
} emit_t;

static void emit_ch(emit_t *e, char c) {
	if (e->len < e->cap) e->buf[e->len++] = c;
}

static void emit_str(emit_t *e, const char *s) {
	for (uint32_t i = 0; s[i]; i++) emit_ch(e, s[i]);
}

static void emit_num(emit_t *e, uint32_t v) {

	char tmp[12];
	int n = 0;

	if (!v) tmp[n++] = '0';
	while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }

	while (n) emit_ch(e, tmp[--n]);

}

// Move the cursor left or right by n columns. Emitted as one
// parameterised sequence rather than n single steps: a line recall
// can move a long way, and n backspaces would be n bytes down the
// port for something that fits in six.
static void emit_left(emit_t *e, uint32_t n) {
	if (!n) return;
	emit_str(e, "\x1b[");
	emit_num(e, n);
	emit_ch(e, 'D');
}

static void emit_right(emit_t *e, uint32_t n) {
	if (!n) return;
	emit_str(e, "\x1b[");
	emit_num(e, n);
	emit_ch(e, 'C');
}

static void emit_erase_eol(emit_t *e) {
	emit_str(e, "\x1b[K");
}

static void emit_up(emit_t *e, uint32_t n) {
	if (!n) return;
	emit_str(e, "\x1b[");
	emit_num(e, n);
	emit_ch(e, 'A');
}

static void emit_down(emit_t *e, uint32_t n) {
	if (!n) return;
	emit_str(e, "\x1b[");
	emit_num(e, n);
	emit_ch(e, 'B');
}

// Erase from the cursor to the end of the screen -- ED mode 0.
static void emit_erase_below(emit_t *e) {
	emit_str(e, "\x1b[J");
}

// Rewrites everything from `from` to the end of the line, erases
// whatever the old line left beyond it, and puts the cursor back
// where it belongs.
//
// This is the one operation every mid-line edit reduces to.
static void redraw_tail(emit_t *e, const z_line_t *line, uint32_t from) {

	// Stops at the end of the current ROW. In multi-line input the
	// buffer continues past it on the next terminal row, and writing
	// that here would print it on this one.
	uint32_t end = row_end_of(line, from);

	for (uint32_t i = from; i < end; i++) emit_ch(e, line->buf[i]);

	emit_erase_eol(e);

	if (end > line->pos) emit_left(e, col_of(line, line->pos, end));

}

// Repaints the WHOLE input, every row of it, and leaves the cursor at
// `pos`.
//
// Needed whenever the row structure changes -- a newline inserted, or
// a backspace that joins two rows -- because everything below the
// edit moves. Single-row edits keep using redraw_tail(), which is far
// cheaper; this is the fallback for the cases that cannot be
// expressed as "rewrite the rest of this row".
//
// This is the only code here that needs to know the prompt width. See
// z_line_set_multiline().
static void redraw_all(emit_t *e, z_line_t *line) {

	uint32_t cur_row = row_index_of(line, line->pos);

	// Up to the first row, then to column 0, then past the prompt.
	//
	// Navigating by screen_row, NOT by cur_row: the buffer may have
	// just lost a newline, which moves the cursor's LOGICAL row
	// without the terminal's cursor having moved at all. See
	// screen_row in zline.h.
	if (line->screen_row) emit_up(e, line->screen_row);
	emit_ch(e, '\r');

	// Erase everything from here down before rewriting: the input may
	// have got shorter, and rows the previous render left behind
	// would otherwise stay on screen.
	emit_erase_below(e);

	emit_right(e, line->prompt_w);

	uint32_t rows = row_count(line);
	uint32_t i = 0;

	for (uint32_t r = 0; r < rows; r++) {

		uint32_t end = row_end_of(line, i);

		for (uint32_t k = i; k < end; k++) emit_ch(e, line->buf[k]);

		if (r + 1 < rows) {
			emit_str(e, "\r\n");
			if (line->cont_prompt) emit_str(e, line->cont_prompt);
		}

		i = end + 1;

	}

	line->drawn_rows = rows;

	// Now put the cursor back. It is currently at the end of the last
	// row; walk up to its row and along to its column.
	uint32_t last_row = rows - 1;

	if (last_row > cur_row) emit_up(e, last_row - cur_row);

	emit_ch(e, '\r');
	emit_right(e, line->prompt_w + col_of(line, row_start_of(line, line->pos), line->pos));

	line->screen_row = cur_row;

}

// Replaces the whole input with `s` -- used by history recall.
static void set_line(z_line_t *line, emit_t *e, const char *s) {

	// Back to the start of the input, then rewrite. Relative, so the
	// prompt in front of it is untouched. Columns, not bytes: a
	// multi-byte character is still one step of the cursor.
	emit_left(e, col_of(line, 0, line->pos));

	str_copy(line->buf, s, Z_LINE_MAX + 1);
	line->len = str_len(line->buf);
	line->pos = line->len;

	for (uint32_t i = 0; i < line->len; i++) emit_ch(e, line->buf[i]);

	emit_erase_eol(e);

}

static void hist_show(z_line_t *line, emit_t *e, int at) {

	const char *s = hist_get(line->hist, at);
	if (!s) return;

	// Leaving a fresh line for the first time: keep what was typed,
	// so coming back down returns it.
	if (line->hist_at == Z_LINE_HIST_NONE) {
		str_copy(line->stash, line->buf, Z_LINE_MAX + 1);
		line->stash_len = line->len;
	}

	line->hist_at = at;
	set_line(line, e, s);

}

// -- editing operations --

// Drops `n` bytes at `at`. `n` is always a whole character: callers
// step with z_utf8_prev_off / z_utf8_next_off, so a sequence is never
// cut in half.
static void buf_delete(z_line_t *line, uint32_t at, uint32_t n) {

	for (uint32_t i = at + n; i < line->len; i++)
		line->buf[i - n] = line->buf[i];

	line->len -= n;
	line->buf[line->len] = 0;

}

static void do_left(z_line_t *line, emit_t *e) {

	if (!line->pos) return;

	uint32_t prev = (uint32_t)z_utf8_prev_off(line->buf, (int)line->len, (int)line->pos);
	uint32_t n = line->pos - prev;
	int cols;

	if (!n) return;

	cols = z_utf8_cols(line->buf + prev, n);
	line->pos = prev;
	if (cols > 0) emit_left(e, (uint32_t)cols);

}

static void do_right(z_line_t *line, emit_t *e) {

	if (line->pos >= line->len) return;

	uint32_t next = (uint32_t)z_utf8_next_off(line->buf, (int)line->len, (int)line->pos);
	uint32_t n = next - line->pos;
	int cols;

	if (!n) return;

	cols = z_utf8_cols(line->buf + line->pos, n);
	line->pos = next;
	if (cols > 0) emit_right(e, (uint32_t)cols);

}

// Home and End work within the ROW, which is what they mean on a
// screen -- a multi-row input has a start of line per row.
static void do_home(z_line_t *line, emit_t *e) {
	uint32_t s = row_start_of(line, line->pos);
	if (line->pos == s) return;
	emit_left(e, col_of(line, s, line->pos));
	line->pos = s;
}

static void do_end(z_line_t *line, emit_t *e) {
	uint32_t end = row_end_of(line, line->pos);
	if (line->pos >= end) return;
	emit_right(e, col_of(line, line->pos, end));
	line->pos = end;
}

static void do_backspace(z_line_t *line, emit_t *e) {

	uint32_t prev, n;
	int cols;
	bool joined;

	if (!line->pos) return;

	prev = (uint32_t)z_utf8_prev_off(line->buf, (int)line->len, (int)line->pos);
	n = line->pos - prev;
	if (!n) return;

	joined = (n == 1 && line->buf[prev] == '\n');
	cols = z_utf8_cols(line->buf + prev, n);

	buf_delete(line, prev, n);
	line->pos = prev;

	// Deleting the newline merges two rows, so everything below moves
	// -- there is no way to express that as a rewrite of one row.
	if (joined) { redraw_all(e, line); return; }

	if (cols > 0) emit_left(e, (uint32_t)cols);
	redraw_tail(e, line, line->pos);

}

static void do_delete(z_line_t *line, emit_t *e) {

	uint32_t next, n;
	bool joined;

	if (line->pos >= line->len) return;

	next = (uint32_t)z_utf8_next_off(line->buf, (int)line->len, (int)line->pos);
	n = next - line->pos;
	if (!n) return;

	joined = (n == 1 && line->buf[line->pos] == '\n');

	buf_delete(line, line->pos, n);

	if (joined) { redraw_all(e, line); return; }

	redraw_tail(e, line, line->pos);

}

static void do_kill_eol(z_line_t *line, emit_t *e) {

	uint32_t end = row_end_of(line, line->pos);

	if (line->pos >= end) return;

	// Only to the end of this ROW. Removing everything below as well
	// would be a much larger action than the key promises.
	uint32_t n = end - line->pos;

	for (uint32_t i = end; i < line->len; i++)
		line->buf[i - n] = line->buf[i];

	line->len -= n;
	line->buf[line->len] = 0;

	if (row_count(line) > 1) redraw_all(e, line);
	else emit_erase_eol(e);

}

static void do_kill_line(z_line_t *line, emit_t *e) {

	// The whole input, every row of it. Ctrl+U means "start over",
	// and starting over halfway through a multi-line form would be
	// stranger than either alternative.
	bool multi = row_count(line) > 1;

	if (multi) {
		line->len = 0;
		line->pos = 0;
		line->buf[0] = 0;
		redraw_all(e, line);
		return;
	}

	emit_left(e, col_of(line, 0, line->pos));

	line->len = 0;
	line->pos = 0;
	line->buf[0] = 0;

	emit_erase_eol(e);

}

// Inserts `n` bytes at the cursor. `n` is one character -- a single
// ASCII byte, or a whole UTF-8 sequence -- and it is all or nothing:
// a sequence that does not fit is not cut down to the bytes that do.
static void do_insert_bytes(z_line_t *line, emit_t *e, const char *s, uint32_t n) {

	if (!n || line->len + n > Z_LINE_MAX) return;

	for (uint32_t i = line->len; i > line->pos; i--)
		line->buf[i + n - 1] = line->buf[i - 1];

	for (uint32_t k = 0; k < n; k++)
		line->buf[line->pos + k] = s[k];

	line->len += n;
	line->pos += n;
	line->buf[line->len] = 0;

	for (uint32_t k = 0; k < n; k++) emit_ch(e, s[k]);

	// Appending at the end is the common case and costs the
	// character; only an insert in the middle needs the tail
	// rewritten.
	if (line->pos != line->len) redraw_tail(e, line, line->pos);

}

static void do_insert(z_line_t *line, emit_t *e, char c) {
	do_insert_bytes(line, e, &c, 1);
}

// How many bytes a lead byte opens, or 0 if it cannot start UTF-8.
// Overlongs and out-of-range values look like leads here and are
// rejected by z_utf8_valid() once the sequence is complete, which is
// the one place that decides.
static int utf8_need_of(uint8_t b) {
	if ((b & 0xE0) == 0xC0) return 2;
	if ((b & 0xF0) == 0xE0) return 3;
	if ((b & 0xF8) == 0xF0) return 4;
	return 0;
}

static void commit_utf8(z_line_t *line, emit_t *e) {
	if (!z_utf8_valid((const char *)line->utf8, line->utf8_len)) return;
	do_insert_bytes(line, e, (const char *)line->utf8, line->utf8_len);
}

// Inserts a newline and continues editing on a fresh row.
static void do_newline(z_line_t *line, emit_t *e) {

	if (line->len >= Z_LINE_MAX) return;

	for (uint32_t i = line->len; i > line->pos; i--)
		line->buf[i] = line->buf[i - 1];

	line->buf[line->pos] = '\n';
	line->len++;
	line->pos++;
	line->buf[line->len] = 0;

	// Appending at the very end is the common case -- Enter at the
	// end of what you are typing -- and just needs a new row started.
	if (line->pos == line->len) {
		emit_str(e, "\r\n");
		if (line->cont_prompt) emit_str(e, line->cont_prompt);
		line->drawn_rows = row_count(line);
		line->screen_row++;
		return;
	}

	redraw_all(e, line);

}

// Up and down move between ROWS when there is more than one, and only
// reach history at the edges.
//
// That ordering is the whole reason multi-line is usable: pressing up
// to fix a typo two rows above must not replace the entire form with
// the previous command.
static void do_up(z_line_t *line, emit_t *e);
static void do_down(z_line_t *line, emit_t *e);

static void do_hist_prev(z_line_t *line, emit_t *e) {
	if (!line->hist) return;
	hist_show(line, e, line->hist_at + 1);
}

static void do_hist_next(z_line_t *line, emit_t *e) {

	if (!line->hist || line->hist_at == Z_LINE_HIST_NONE) return;

	if (line->hist_at == 0) {
		// Back to the line that was being typed before browsing
		// started.
		line->hist_at = Z_LINE_HIST_NONE;
		set_line(line, e, line->stash);
		return;
	}

	hist_show(line, e, line->hist_at - 1);

}

// Moves the cursor to `col` on the row `delta` away, keeping the
// column where possible -- shorter rows clamp to their end, as in any
// editor.
static void row_move(z_line_t *line, emit_t *e, int delta) {

	uint32_t start = row_start_of(line, line->pos);
	uint32_t col = col_of(line, start, line->pos);

	uint32_t target_start, new_pos, new_col;

	if (delta < 0) {
		if (!start) return;
		target_start = row_start_of(line, start - 1);
	} else {
		uint32_t end = row_end_of(line, line->pos);
		if (end >= line->len) return;
		target_start = end + 1;
	}

	// Keep the column, clamped to the target row, and never in the
	// middle of a character. The cursor then moves to THAT column,
	// which is not always the one asked for: a wide character can
	// refuse the right half.
	new_pos = pos_at_col(line, target_start, col);
	new_col = col_of(line, target_start, new_pos);
	line->pos = new_pos;

	if (delta < 0) { emit_up(e, 1); line->screen_row--; }
	else { emit_down(e, 1); line->screen_row++; }

	emit_ch(e, '\r');
	emit_right(e, line->prompt_w + new_col);

}

static void do_up(z_line_t *line, emit_t *e) {

	// Inside a multi-row input, up moves a row -- unless already on
	// the first, where it falls through to history the way it would
	// on a single-row line.
	if (multiline(line) && row_start_of(line, line->pos)) {
		row_move(line, e, -1);
		return;
	}

	// History would replace the whole input, so it is refused while
	// there is a multi-row form in progress: recalling over it would
	// silently discard work that took several lines to type.
	if (multiline(line) && row_count(line) > 1) return;

	do_hist_prev(line, e);

}

static void do_down(z_line_t *line, emit_t *e) {

	if (multiline(line) && row_end_of(line, line->pos) < line->len) {
		row_move(line, e, 1);
		return;
	}

	if (multiline(line) && row_count(line) > 1) return;

	do_hist_next(line, e);

}

// Dispatches a completed CSI sequence by its final byte.
static void csi_final(z_line_t *line, emit_t *e, char f, uint32_t p) {

	switch (f) {

		case 'A': do_up(line, e); break;
		case 'B': do_down(line, e); break;
		case 'C': do_right(line, e); break;
		case 'D': do_left(line, e); break;
		case 'H': do_home(line, e); break;
		case 'F': do_end(line, e); break;

		case '~':
			// ESC[1~ home, ESC[3~ delete, ESC[4~ end -- the other
			// common encodings of the same keys. Which one arrives
			// depends on the terminal; sw/apps/term sends ESC[3~ for
			// Delete and ESC[H / ESC[F for Home/End.
			if (p == 1 || p == 7) do_home(line, e);
			else if (p == 3) do_delete(line, e);
			else if (p == 4 || p == 8) do_end(line, e);
			break;

		default:
			// Unrecognised: swallowed. Doing nothing is right --
			// letting the parameter bytes fall through into the line
			// is what the minimal reader used to do, and it silently
			// corrupted commands.
			break;

	}

}

int z_line_feed(z_line_t *line, uint8_t byte,
	char *echo, uint32_t *echo_len, uint32_t echo_cap) {

	emit_t e = { echo, echo_cap, 0 };

	*echo_len = 0;

	// A UTF-8 sequence in progress. A continuation byte extends it;
	// anything else abandons it with no echo and is then read for
	// what it is. A control byte has to keep working, and half a
	// character must not appear on screen or in the line.
	if (line->utf8_need) {

		if ((byte & 0xC0) == 0x80 && line->utf8_len < sizeof line->utf8) {

			line->utf8[line->utf8_len++] = byte;

			if (line->utf8_len >= line->utf8_need) {
				commit_utf8(line, &e);
				line->utf8_len = 0;
				line->utf8_need = 0;
			}

			// This byte was text, so a CR waiting for its LF is over.
			line->had_cr = false;
			*echo_len = e.len;
			return 0;

		}

		line->utf8_len = 0;
		line->utf8_need = 0;

	}

	// -- escape sequences --
	//
	// Handled before anything else: mid-sequence, a byte means what
	// the sequence says it means, not what it would mean alone. 'D'
	// in ESC[D is an arrow key, not the letter D.
	if (line->esc == Z_LINE_ESC_ESC) {

		if (byte == '[' || byte == 'O') {
			line->esc = Z_LINE_ESC_CSI;
			line->esc_p = 0;
			return 0;
		}

		// ESC followed by anything else: not a sequence this handles.
		line->esc = Z_LINE_ESC_NONE;
		return 0;

	}

	if (line->esc == Z_LINE_ESC_CSI) {

		if (byte >= '0' && byte <= '9') {
			line->esc_p = line->esc_p * 10 + (uint32_t)(byte - '0');
			return 0;
		}

		if (byte == ';' || byte == '?') return 0;	// further params

		// A final byte ends the sequence.
		if (byte >= 0x40 && byte <= 0x7e) {
			line->esc = Z_LINE_ESC_NONE;
			csi_final(line, &e, (char)byte, line->esc_p);
			*echo_len = e.len;
			return 0;
		}

		line->esc = Z_LINE_ESC_NONE;
		return 0;

	}

	if (byte == 0x1b) {
		line->esc = Z_LINE_ESC_ESC;
		return 0;
	}

	// CR -- always ends a line, and sets had_cr so a following LF (a
	// real CRLF pair, e.g. from a real telnet peer someday) doesn't
	// start a second, empty line.
	if (byte == '\r') {

		line->buf[line->len] = 0;
		line->had_cr = true;

		// Ask the caller whether this is finished. See
		// z_line_complete_fn in zline.h -- what "finished" means
		// depends on what the text is, and this module has no
		// opinion on that.
		if (line->complete && !line->complete(line->buf, line->complete_user)) {
			do_newline(line, &e);
			*echo_len = e.len;
			return 0;
		}

		emit_str(&e, "\r\n");
		*echo_len = e.len;
		return 1;

	}

	// LF -- ends a line UNLESS it's the second half of a CRLF pair
	// this function already completed on the CR half of.
	if (byte == '\n') {
		if (line->had_cr) {
			line->had_cr = false;
			return 0;
		}
		line->buf[line->len] = 0;

		if (line->complete && !line->complete(line->buf, line->complete_user)) {
			do_newline(line, &e);
			*echo_len = e.len;
			return 0;
		}

		emit_str(&e, "\r\n");
		*echo_len = e.len;
		return 1;
	}

	line->had_cr = false;

	// backspace -- either raw DEL (0x7f, what a real terminal
	// actually sends, see term.c's key_to_bytes()) or classic BS
	// (0x08) -- accept both.
	if (byte == 0x7f || byte == 0x08) {
		do_backspace(line, &e);
		*echo_len = e.len;
		return 0;
	}

	// The emacs-style control keys every line editor has. They cost
	// nothing to support and are what hands already know.
	switch (byte) {
		case 0x01: do_home(line, &e); *echo_len = e.len; return 0;	// Ctrl+A
		case 0x05: do_end(line, &e); *echo_len = e.len; return 0;	// Ctrl+E
		case 0x02: do_left(line, &e); *echo_len = e.len; return 0;	// Ctrl+B
		case 0x06: do_right(line, &e); *echo_len = e.len; return 0;	// Ctrl+F
		case 0x0b: do_kill_eol(line, &e); *echo_len = e.len; return 0;	// Ctrl+K
		case 0x15: do_kill_line(line, &e); *echo_len = e.len; return 0;	// Ctrl+U
		case 0x03:													// Ctrl+C
			// Abandon whatever is being typed and hand the caller an
			// empty line, which it answers with a fresh prompt.
			//
			// The escape hatch for multi-line entry, and the reason
			// it is not optional: a stray '(' or an unclosed quote
			// leaves the form permanently incomplete, and without
			// this there is no way to stop the terminal asking for
			// more. Every shell binds this key to exactly that.
			line->len = 0;
			line->pos = 0;
			line->buf[0] = 0;
			line->hist_at = Z_LINE_HIST_NONE;
			emit_str(&e, "\r\n");
			*echo_len = e.len;
			return 1;

		case 0x04:													// Ctrl+D
			// Delete forward, matching every shell. On an EMPTY line
			// a shell would take this as end-of-input; that is a
			// decision about what a line MEANS, so it is left to the
			// caller -- this just does nothing.
			do_delete(line, &e);
			*echo_len = e.len;
			return 0;
		default: break;
	}

	// Printable ASCII: one byte, one column. Same path as before.
	if (byte >= 0x20 && byte < 0x7f) {
		do_insert(line, &e, (char)byte);
		*echo_len = e.len;
		return 0;
	}

	// A lead byte opens a sequence and echoes nothing yet. A
	// continuation with nothing open, or a byte that cannot start
	// UTF-8, is dropped -- the same silence as any other rejected
	// byte. The gap is checked for the whole sequence, when it
	// completes, not for this one byte.
	if (byte >= 0x80) {
		int need = utf8_need_of(byte);
		if (need) {
			line->utf8[0] = byte;
			line->utf8_len = 1;
			line->utf8_need = (uint8_t)need;
			line->had_cr = false;
		}
	}

	return 0;

}
