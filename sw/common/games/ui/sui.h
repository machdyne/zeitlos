#ifndef SUI_H
#define SUI_H

/*
 * Zeitlos -- the screen toolkit: boxes for screen and full-screen apps,
 * drawn on their 320x240 screen. docs/screen_ui.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- why a second set of boxes --
 *
 * A desktop app's boxes (zdialog.h) are windows at the desktop's
 * resolution, and wm shows no windows over game mode. A screen app
 * (docs/app_kinds.md) draws one 320x240 screen, in a window or full
 * screen; its boxes belong on that screen, at its resolution, in the
 * window and full screen alike. They share no widgets with the
 * desktop's: an element designed for one resolution is not stretched
 * or shrunk to fit the other.
 *
 * -- what they look like --
 *
 * One bit first. A box is black with a one-pixel white frame and a
 * one-pixel black ring outside it, so it stands clear of any picture;
 * white 5x8 text in nine-pixel rows; buttons framed, the focused one
 * inverted. No title bars, no shadows, no animation. In colour a few
 * accents -- the focused button, a warning's title, the scrollbar --
 * and the picture behind the box dimmed through the palette, never a
 * pass over its pixels.
 *
 * -- how they run --
 *
 * Each call is MODAL: it returns when the box is answered. Meanwhile
 * the arcade runtime's frames keep running (ar_frame()), the picture
 * behind is the program's own, repainted by its paint() where needed
 * and the box drawn over it (ar_top(), zarcade.h). The program's
 * overlays are not drawn while a box is up. F11 still goes between the
 * window and full screen; the box stays where it is. A mode change or
 * the window being closed is handed back to the program once the box
 * is gone (ar_push()); a closed window answers the box as Escape would.
 *
 * The answers are zdialog's, so a call site reads the same: Cancel is
 * 0, Yes (and OK) 1, No 2.
 */

#include <stdint.h>
#include <stdbool.h>

/* sui_confirm() button sets, and its answers -- zdialog.h's values */
#define SUI_OK_CANCEL       0
#define SUI_YES_NO          1
#define SUI_YES_NO_CANCEL   2

#define SUI_CANCEL   0
#define SUI_YES      1     /* also OK */
#define SUI_NO       2

/* -- colours --
 *
 * Eight roles. In monochrome each has a fixed ink (black, white, a
 * grey pattern for the scrollbar's trough), whatever the program's own
 * inks are. In colour each is a palette entry: by default the top
 * eight of what the program draws in (ar_colours()), which the toolkit
 * sets to sui_rgb[] while a box is up -- a program whose art leaves
 * them free needs to do nothing. A program can name entries of its own
 * instead with sui_palette(): the boxes then draw in those as they are,
 * and nothing is written to them. Either way the rest of the palette
 * is dimmed behind a box (unless the program asks not to), and all of
 * it is put back when the box goes. */
enum {
	SUI_C_FACE = 0,    /* the box: black */
	SUI_C_TEXT,        /* text: white */
	SUI_C_FRAME,       /* frames and rules: white */
	SUI_C_SEL,         /* the focused button: white (an accent in colour) */
	SUI_C_SEL_TEXT,    /* its label: black */
	SUI_C_WARN,        /* a warning's title: white (red in colour) */
	SUI_C_TROUGH,      /* the scrollbar's trough: a grey pattern */
	SUI_C_THUMB,       /* its thumb: white */
	SUI_NC
};

extern const uint16_t sui_rgb[SUI_NC];

/* In colour, the entries the boxes draw in, one per SUI_C_* (SUI_NC
 * of them), used as the program has set them; `dim`: every other entry
 * dimmed behind a box (false where the program's controls share
 * entries with its art, say). NULL: the default, the top eight entries
 * set to sui_rgb[], the rest dimmed. Kept by value; takes effect at the
 * next box. */
void sui_palette(const uint8_t *idx, bool dim);

/* -- the boxes --
 *
 * `text` is word-wrapped to the box (at most 280 pixels wide); '\n'
 * ends a line. Text longer than ten rows scrolls: the scrollbar, the
 * wheel, the arrows, PgUp and PgDn. */

/* A message and OK. */
void sui_message(const char *title, const char *text);

/* The same with the title as a warning (red in colour). */
void sui_warn(const char *title, const char *text);

/* Two or three buttons (SUI_OK_CANCEL, SUI_YES_NO, SUI_YES_NO_CANCEL).
 * Escape gives the least destructive answer: Cancel where there is
 * one, No otherwise. Left and Right (and Tab) move between the
 * buttons, Enter or Space takes the focused one; Y, N, O and C take
 * Yes, No, OK and Cancel. */
int sui_confirm(const char *title, const char *text, int buttons);

/* A line to type into, OK and Cancel. True, and the text in `out`, on
 * OK with something typed; false on Cancel or an empty line. `initial`
 * may be NULL. At most 127 characters. */
bool sui_prompt(const char *title, const char *text, const char *initial,
	char *out, int outlen);

/* The same where an empty line is an answer. */
bool sui_prompt_empty_ok(const char *title, const char *text,
	const char *initial, char *out, int outlen);

/* For a password: shown as '*', printable ASCII only, and the box's own
 * copy wiped before this returns. */
bool sui_prompt_secret(const char *title, const char *text, char *out,
	int outlen);

/* -- files --
 *
 * A folder's list -- folders first, then files, by name; ".." to go up
 * -- with a scrollbar and the wheel, and Open (or Save) and Cancel.
 * Up, Down, PgUp, PgDn, Home, End move in the list; Enter (or a second
 * click) opens a folder or takes a file; Backspace goes up a folder; a
 * letter jumps to the next name starting with it (open). No new
 * folder, no filters. `start_dir` NULL is /home. On a file, the full
 * path in `out` and true; false on Cancel. */
bool sui_open(const char *start_dir, char *out, int outlen);

/* The same with a name to type, filled with `suggested` (may be NULL);
 * a file picked in the list puts its name there. Typing on the list
 * goes to the name. Does not ask about overwriting: the caller does,
 * with sui_confirm(). */
bool sui_save(const char *start_dir, const char *suggested, char *out,
	int outlen);

/* -- help --
 *
 * The program's help on its own screen: `text` as for z_dialog_help()
 * (zdialog.h) -- '\n' ends a line, an empty line between paragraphs,
 * and '\t' moves to a fixed column, a wrapped line continuing there, so
 * "Ctrl+S\tSave" lines up as a table of keys. Scrolls (the scrollbar,
 * the wheel, the arrows, PgUp, PgDn); F1, Esc or Enter closes it.
 *
 * The runtime opens it for F1, in the window and full screen alike,
 * with the ar_desc_t's help_title and help_text, in every program that
 * links this file (zarcade_sys.c finds it by a weak reference); one
 * that does not keeps the desktop's help box, in the window only. */
void sui_help(const char *title, const char *text);

/* -- for the host tests (tests/test_sui.c) --
 *
 * The last box's layout: false if anything of it was outside the
 * screen or any text was cut off; its rectangle in *x, *y, *w, *h
 * (any may be NULL). */
bool sui_last_layout_ok(int *x, int *y, int *w, int *h);

#endif
