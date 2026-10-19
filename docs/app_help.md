# Help in apps: F1

**F1** in an app shows a small help box: what the app is for, its
important keys, and how to do the main thing -- a screen or two at
most. **Close** (Enter, Esc, or F1 again) puts it away. When `read` is
installed and the app's manual is on the card, **Read docs** sits
beside Close and opens the full page from `docs/` in `read`.

```
+-- Sheet help ------------------------------------+
| A spreadsheet: numbers, text and formulas in a   |
| grid.                                            |
|                                                  |
| /           Commands: insert, delete, number   # |
|             format, width, sort, extend, ...   # |
| F2          Edit the cell                      | |
| Ctrl+Z/Y    Undo, redo                         | |
|                          [Read docs] [ Close ]   |
+--------------------------------------------------+
```

Apps with it: `text`, `files`, `read`, `sheet`, `hex`, `term` (start panel), `calc`, `cal`, `clock`, `settings`, `ask`, `bench`, `view`, `draw`, `play`, `mmod`, `track`, `web`, `irc`, `basic`, `logic`, `chess`, `casino`, `kidgames`, `space3d`, `gpu3d`.

Apps whose F1 was already their own help keep it: `midi` and `mesh`
(a key panel in the window), and the casino games `blackjack`, `craps`,
`poker`, `roulette` and `slots` (one help line at a time in the status
bar). `chess` had the status-bar kind too; in its window F1 is now the
box, and in full screen, where a box cannot show, it is the old lines.
`chip8` uses F1 for the display scale (an emulator's keys are the
program's), so it has no help box. In `basic`, `chess` and `space3d`
the box is for the window only, not full screen.

While the box is up the app behind waits: a game or a running BASIC
program pauses, music in `play` and `track` pauses and resumes, the
clock's hands stop. `web` and `irc` keep going underneath -- a page
keeps loading, chat keeps arriving.

In `term`, F1 works on the **start panel** only. Connected, every key
belongs to the program at the other end, and F1 is a key a remote
program may want -- so term sends it on, like any other key.

## Why a box of the app's own, and the manual as well

The first design (0.0.7, round 6) had F1 open the app's manual in
`read`, so that there would be one description of each app, not two.
It had a hole: `read` is not a core app and `/docs` is on the SD card,
so on a system running from flash -- exactly where help is most needed
-- F1 could only say that the help was somewhere else.

So the short version lives in the app. It is a few hundred bytes of
text, written for someone using the app now: not the manual, but the
keys you would otherwise have to go and look up. The manual stays the
long version and the one place for everything else, and the button
opens it where it can.

Keeping the two from drifting: the box carries keys and one or two
sentences, which change rarely and together; a change to an app's keys
changes its help string in the same commit, the same way it changes
the manual.

**F1** rather than Ctrl+H: Ctrl+H is Replace in `text` (as in most
editors), and F1 is help nearly everywhere else.

## The box

One size for every app: **300 pixels wide** (about 58 columns of the
5x8 font), and as tall as the text up to 16 lines; longer text gets a
scrollbar, and Up/Down, PgUp/PgDn, Home/End, Space and the wheel scroll
it. Small on purpose: it sits over the app, which is still there to be
looked at.

| key | does |
|---|---|
| Enter, Esc, F1 | close |
| Tab / Shift+Tab | move between Read docs and Close (Close starts focused) |
| Space | the next page while there is one; then presses the focused button |
| arrows, PgUp/PgDn, Home/End, wheel | scroll |

"Read docs" appears only when both are there: `read` (z_exec_exists())
and `/docs/<doc>.md` (fs_stat()). It opens the page from the top.

## Adding it to an app

```c
#include "../../common/zdialog.h"

if (keysym == Z_KEY_F1) {
	static const char help[] =
		"Browse, open and manage files.\n\n"
		"Enter\tOpen the selection\n"
		"Backspace\tUp a folder\n"
		"F2\tRename\n\n"
		"Copy and Move ask for the destination ...";
	z_dialog_help(&dlg_ctx, "Files help", help, "file_browser");
	return;
}
```

and `zdialog.o zfsapp.o` in the Makefile's OBJS if they are not there.

The text has two rules:

- `\n` ends a line; an empty line separates paragraphs. Everything
  else wraps at word boundaries.
- `\t` moves to column 12, and a line that wraps after one continues
  at column 12 -- so `"Ctrl+S\tSave the file\n"` lines up as a table of
  keys with no table code. Keep the key names to 11 characters.

Write: one line on what the app is for, the keys that matter, a
sentence or two on the main task. Leave out what the app's own screen
already says.

`ctx->on_msg` may be NULL in an app that has no handler factored out of
its loop: the dialog then keeps the app window's region current and
acks its redraws itself, and the app repaints its window after
z_dialog_help() returns.

## What it costs

`sw/common/zdialog.c` reaches each kind of dialog only through a table
of handlers set by its entry point, and the file list (some 7KB of
`.bss`) stands apart from the rest, so section GC links what an app
uses and nothing else. An app that already had dialogs pays about
3KB plus its text; an app that had none pays for the dialog core and,
where it had none of them either, the buttons and speech: 6KB in
`term`, 8-9KB in `calc`, `cal` and `clock`, up to 16-21KB in apps such
as `casino`, `space3d` and `basic` that drew everything themselves.
Those are card apps; the core apps' budgets are below.

`text` is the one core app this moved: 157,672 to 160,580 bytes, so its
RAM budget goes from 154K to 157K (Makefile, CORE_BUDGETS).

## Testing

`sw/common/tests/test_dialog.c` drives the box's real key, wheel and
layout code on the host: wrapping at words, the tab column and the
hanging indent, the scrollbar only when needed, scrolling, Close
focused first. `HELP_PBM=help.pbm` draws one to look at.

Board checks for the final phase: F1 in each app shows its box;
Read docs appears with `read` and the card, not without either, and
opens the right page; the app behind is repainted when the box closes;
F1 in a connected `term` reaches the far end.
