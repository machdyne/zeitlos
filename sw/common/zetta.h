#ifndef ZETTA_H
#define ZETTA_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zetta -- the Zeitlos Editor for TexT Applications. docs/zetta.md.
 *
 * A full-screen editor for prose -- messages, notes, a letter -- in the
 * manner of nano: word-wrapped paragraphs that reflow as you type, a help
 * bar that says which keys do what. (Named for the SI prefix 10^21: an
 * editor smaller than pico and nano, with the largest name. Ironic.)
 *
 * EMBEDDED, not run: the BBS has it inside each caller's session, posix
 * and the REPL wrap it in a loop. So it never waits and never reads a
 * terminal itself --
 *
 *   zetta_init()      the caller's buffer, and its configuration
 *   zetta_key()       bytes typed (raw: zetta decodes the escape
 *                     sequences), and the time
 *   zetta_tick()      the time, when nothing is typed: a lone Esc is
 *                     told from an arrow key's first byte by a pause
 *   cfg.write()       where the screen updates go
 *
 * -- and what the text is FOR is the app's business: zetta_key() returns
 * an event -- one of the app's own actions ("Post", "Save"), Exit, or the
 * answer to a dialog the app opened -- and the app does the work.
 *
 * No heap: zetta_t lives in the caller's state; the text, the header
 * fields and the cut buffer are the caller's memory.
 */
#include <stdint.h>
#include <stdbool.h>

#define ZETTA_FIELDS_MAX   4
#define ZETTA_ACTIONS_MAX  8
#define ZETTA_ROWS_MAX     60
#define ZETTA_COLS_MAX     200
#define ZETTA_HOLD         256			// input held after an event, for the next call

// Keys: a character (Unicode), a control key (1..31: Ctrl-A is 1), or
// one of these.
enum {
	ZK_UP = 0x110000, ZK_DOWN, ZK_LEFT, ZK_RIGHT, ZK_HOME, ZK_END,
	ZK_PGUP, ZK_PGDN, ZK_DEL, ZK_INS, ZK_ESC, ZK_ENTER, ZK_BS, ZK_TAB,
};
#define ZK_CTRL(c) ((c) - '@')			// ZK_CTRL('S') is 19

// What zetta_key() tells the app.
enum {
	ZE_NONE = 0,
	ZE_ACTION,			// one of the app's actions: its id in zetta_t.action
	ZE_EXIT,			// Ctrl-X: the app decides -- confirm, save, discard
	ZE_ANSWER,			// a dialog the app opened was answered: zetta_t.answer
};

// A header field above the text: "To", "Subject".
typedef struct {
	const char *label;
	char *buf;						// the caller's, NUL-terminated
	uint32_t cap;
	// An app's check when the field is left: NULL if fine, else what to
	// show. May be NULL.
	const char *(*check)(const char *value, void *ctx);
} zetta_field_t;

// An action the app offers: shown in the help bar and the Esc menu.
typedef struct {
	uint32_t key;					// e.g. ZK_CTRL('S'); 0: in the menu only
	const char *label;				// "Post"
	int id;							// returned in zetta_t.action
} zetta_action_t;

typedef struct {
	int rows, cols;					// the screen: 25 x 80 for the BBS
	const char *title;
	zetta_field_t fields[ZETTA_FIELDS_MAX];
	int nfields;
	zetta_action_t actions[ZETTA_ACTIONS_MAX];
	int nactions;
	int main_action;				// the id Ctrl-Z runs (Mystic's habit); -1 none
	char *clip;						// the cut buffer (Ctrl-K, Ctrl-U); may be NULL
	uint32_t clip_cap;
	void (*write)(void *ctx, const char *bytes, uint32_t n);
	void *ctx;
} zetta_cfg_t;

// A pick-lines dialog's source (the quote window): line i, or NULL past
// the last.
typedef const char *(*zetta_lines_fn)(void *ctx, int i);

typedef struct {
	zetta_cfg_t cfg;
	char *buf;						// the text: UTF-8 paragraphs, '\n' where Enter was pressed
	uint32_t cap, len;
	uint32_t cur;					// the cursor, a byte offset
	uint32_t top;					// the first row shown: a row's first byte
	int want_col;					// kept across up/down
	int focus;						// -1 the text, else a header field
	uint32_t fcur;					// the cursor in the focused field
	bool modified;
	uint32_t clip_len;
	bool clip_append;				// Ctrl-K right after Ctrl-K adds to the cut

	// dialogs
	int mode;
	char status[160];
	bool status_error;
	char prompt_q[80];
	char *prompt_buf;
	uint32_t prompt_cap, prompt_cur;
	int prompt_tag;
	zetta_lines_fn pick_fn;
	void *pick_ctx;
	char pick_prefix[16];
	int pick_n, pick_at, pick_top;
	uint8_t pick_on[256 / 8];		// which lines are chosen (the first 256)
	int menu_at;
	char search[64];

	// what zetta_key() returned
	int action;						// ZE_ACTION: the app's id
	int answer;						// ZE_ANSWER: 'y', 'n', or 0 (cancelled)
	int answer_tag;					// ... to which question

	// input decoding
	uint8_t esc[8];
	int esc_len;
	uint32_t esc_ms;
	uint8_t utf[4];
	int utf_len, utf_need;
	bool last_cr;
	uint8_t hold[ZETTA_HOLD];
	uint32_t hold_len;

	// the screen as last drawn: a fingerprint a row
	uint32_t shown[ZETTA_ROWS_MAX];
	bool drawn;
} zetta_t;

// Starts editing `buf` (cap bytes; its first NUL-terminated contents the
// text). Draws the whole screen.
void zetta_init(zetta_t *ed, const zetta_cfg_t *cfg, char *buf, uint32_t cap);

// Bytes typed, and the time in ms (any clock that counts up). An event.
int zetta_key(zetta_t *ed, const uint8_t *bytes, uint32_t n, uint32_t now_ms);
// One key already decoded (a GUI, a test): the same, without the bytes.
int zetta_key_code(zetta_t *ed, uint32_t key);
// The time, with nothing typed: a pending Esc becomes the Esc key.
int zetta_tick(zetta_t *ed, uint32_t now_ms);

// Draws everything again (after something else wrote to the screen).
void zetta_redraw(zetta_t *ed);
// Draws what changed -- after the app changed the config (a title) while
// handling an event. (zetta_status() and zetta_error() do it themselves.)
void zetta_refresh(zetta_t *ed);

// A line on the status bar.
void zetta_status(zetta_t *ed, const char *msg);
void zetta_error(zetta_t *ed, const char *msg);

// Dialogs the app opens; each answered by a ZE_ANSWER with its tag.
void zetta_confirm(zetta_t *ed, const char *question, int tag);			// 'y' or 'n', 0 Esc
void zetta_prompt(zetta_t *ed, const char *question, char *buf, uint32_t cap, int tag);	// 'y' Enter, 0 Esc
// Pick lines from `fn` (Space chooses, Enter inserts them, each after
// `prefix`, at the cursor): the quote window. Answered 'y' or 0.
void zetta_pick(zetta_t *ed, const char *title, zetta_lines_fn fn, void *ctx, const char *prefix, int tag);

// The text as lines of at most `cols` characters (0: as typed, with
// paragraphs whole), into out[cap], NUL-terminated. Its length.
uint32_t zetta_text(const zetta_t *ed, char *out, uint32_t cap, int cols);

#endif
