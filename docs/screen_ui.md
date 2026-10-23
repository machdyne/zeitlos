# The screen toolkit

`sw/common/games/ui/sui.[ch]`: boxes for **screen and full-screen
apps** ([app_kinds.md](app_kinds.md)) -- a message, a question, a line
to type into, a file to open or save, the app's help -- drawn on the
app's own 320x240 screen, in the window and full screen alike, on the
[arcade runtime](arcade.md).

A desktop app's boxes (`zdialog.h`) are windows at the desktop's
resolution, and wm shows no windows over game mode; a box opened from
full screen used to drop the app back into its window first
(`ar_dialog_begin()`). These boxes stay on the screen the app is
showing. They share no widgets with the desktop's: an element designed
for one resolution is not stretched or shrunk to fit the other. A
desktop app's code and speed do not change at all.

Full screen has no titlebar, so an app shows the open file's name on
its own screen (pix on its status line); its window's title can be
just its name.

## The calls

The answers are zdialog's, so a call site reads the same: Cancel is 0,
Yes (and OK) 1, No 2.

| Call | Box |
|---|---|
| `sui_message(title, text)` | the text and OK |
| `sui_warn(title, text)` | the same, the title a warning (red in colour) |
| `sui_confirm(title, text, buttons)` | `SUI_OK_CANCEL`, `SUI_YES_NO` or `SUI_YES_NO_CANCEL`; returns `SUI_YES`, `SUI_NO` or `SUI_CANCEL` |
| `sui_prompt(title, text, initial, out, outlen)` | a line to type into, OK and Cancel; true with something typed |
| `sui_prompt_empty_ok(...)` | the same, an empty line an answer |
| `sui_prompt_secret(title, text, out, outlen)` | a password: shown as `*`, printable ASCII, the box's copy wiped |
| `sui_open(start_dir, out, outlen)` | a folder's list: a file's full path in `out` |
| `sui_save(start_dir, suggested, out, outlen)` | the same with a name to type |
| `sui_help(title, text)` | the app's help; the runtime opens it for F1 |
| `sui_palette(idx, own)` | which palette entries the boxes draw in, in colour (below) |

```c
if (sui_confirm("Save changes?", "Save the picture first?",
                SUI_YES_NO_CANCEL) == SUI_YES)
    save();
```

Each call is **modal**: it returns when the box is answered, and the
runtime's frames keep running meanwhile (`ar_frame()`).

## What they look like

One bit first; every box is designed in black and white.

- A black box, a one-pixel white frame and a one-pixel black ring
  outside it, so it stands clear of any picture. Centred on the
  screen; no bigger than it needs (at least 120 wide, at most 300).
- The title on the first row, a one-pixel rule under it; the 5x8 font
  in nine-pixel rows. No title bars, no shadows, no animation.
- Text word-wrapped to at most 280 pixels; `'\n'` ends a line. More
  than ten rows scroll: a seven-pixel scrollbar (a grey trough, a white
  thumb), the wheel, Up and Down, PgUp and PgDn, Home and End.
- Buttons, right-aligned on the last row, framed; the focused one
  inverted. While the line has focus the button Enter takes (OK) has a
  two-pixel frame.
- The line: framed, its text scrolling sideways, a one-pixel caret.

In colour the same picture with a few **accents** -- the focused button
yellow, a warning's title red, the scrollbar's trough and thumb in
blue-greys -- and the picture behind the box **dimmed** to half,
through the palette: one write per entry, never a pass over pixels
(where there are entries for it: below). Everything is put back when
the box goes.

| Role | Monochrome | Colour (`sui_rgb[]`) |
|---|---|---|
| `SUI_C_FACE` | black | `000` |
| `SUI_C_TEXT` | white | `fff` |
| `SUI_C_FRAME` | white | `bbc` |
| `SUI_C_SEL` (focused button) | white | `fd2` |
| `SUI_C_SEL_TEXT` | black | `000` |
| `SUI_C_WARN` | white | `f54` |
| `SUI_C_TROUGH` | a 25% grey pattern | `334` |
| `SUI_C_THUMB` | white | `99a` |

In monochrome the boxes draw in these inks whatever the program's own
inks are (the runtime's `ar_top()` gives them their own ink table), so
a box looks the same in every program, in the window and full screen.

### Colour: whose entries

By default, in **64 or 256 colours**, the boxes take the **top eight
entries** (56..63 or 248..255), write their colours there while a box is
up, and dim every other entry. In **16 colours** the art has every
entry, so they draw in its **darkest and lightest** entries as they are
-- the face, the focused button's label and the trough dark, the rest
light -- and nothing is written or dimmed: no accents there, but the
picture behind is left exactly as it was.

A program can name entries of its own instead -- its controls' colours,
or its art's darkest and lightest where the art uses every entry --
with `sui_palette(idx, dim)`: the boxes draw in those entries as they
are and write nothing to them; `dim` says whether the rest of the
palette is dimmed behind a box (no, where those entries are also the
art's). pix names its controls' entries ([pix_app.md](pix_app.md)).

```c
static const uint8_t mine[SUI_NC] = { 0, 15, 15, 15, 0, 15, 0, 15 };
sui_palette(mine, false);
```

`sui_palette(NULL, ...)` goes back to the default. Whatever was changed
is put back when the box goes.

## Files

`sui_open()` and `sui_save()` are as wide as the screen allows (300
pixels): the title, the folder (its left end cut with `...` when it is
long), twelve rows of the folder's list in a frame with a scrollbar,
for Save a `Name` line, and Open (or Save) and Cancel.

- The list: `..` first (but at `/`), then folders (shown with a `/`),
  then files, each by name, case not counted. Up to 192 entries and 3 KB
  of names; a bigger folder is cut short.
- Up, Down, PgUp, PgDn, Home, End move the selection; the wheel
  scrolls; the scrollbar pages and drags.
- Enter, Open, or a second click on the same row within 24 frames: into
  a folder (`..` up), or the file taken. Backspace goes up a folder,
  selecting the one you came out of.
- Open: a letter jumps to the next name that starts with it.
- Save: the focus starts in `Name`, filled with `suggested`; a file
  picked in the list puts its name there; typing on the list goes to
  it; `/` is not a character it takes. Save (or Enter in the line)
  answers the folder and the name. Overwriting is the caller's
  question to ask.
- No new folder, no filters.

`start_dir` NULL is `/home`; a folder that cannot be listed starts at
`/` instead. The listing comes from the runtime (`ar_dir_list()`,
`fs_list_into()` underneath; on a build host, a directory named by
`AR_HOST_FS`).

## Help

`sui_help(title, text)` is the same box at the same width, nineteen
rows of text, and Close. The text follows z_dialog_help()'s rules
([app_help.md](app_help.md)): `'\n'` ends a line, an empty line
separates paragraphs, and `'\t'` moves to column 17, a wrapped line
continuing there -- a key longer than that takes a row of its own, its
meaning on the next. It scrolls; F1, Esc or Enter closes it.

The runtime opens it for **F1** -- in the window and full screen alike
-- with the program's `help_title` and `help_text`, in every program
that links `sui.o`: `zarcade_sys.c` refers to `sui_help` weakly, so a
program without the toolkit keeps the desktop's box in the window and
pays nothing. Keys held are released before it opens, and the game gets
`AR_EV_SUSPEND` after it closes, so it comes back paused.

Linked by pix, snake, blocks, bricks and planedemo. While the box is
up the program's frame hook (`ar_frame_hook()`) is not called either,
so a program that draws in the hook (planedemo) draws nothing over it;
a program whose `paint()` draws nothing (planedemo again) starts its
picture over on the `AR_EV_SUSPEND` that follows.

## Keys and the mouse

| Key | Does |
|---|---|
| Enter | the focused button; in the line, OK |
| Space | the focused button (in the line: a space) |
| Esc | the least destructive answer: Cancel where there is one, No otherwise; OK in a message |
| Tab, Shift+Tab | the next, the previous: the line, then the buttons |
| Left, Right | between the buttons (in the line: the caret) |
| Y N O C | Yes, No, OK, Cancel, where the box has them (not while typing) |
| Up Down PgUp PgDn Home End | scroll long text (in the line: Home and End move the caret) |
| typing | in the line; typed on a button, it goes back to the line |
| F11 | still the window or full screen; the box stays |

A click on a button takes it (pressed and released over the same
button). A click in the line puts the caret there. The scrollbar: a
click above or below the thumb pages, the thumb drags. A box takes a
press only after it has seen the buttons up, so a button still held
from the click that opened it presses nothing.

With speech on (`ar_say()`), a box says its title and text when it
opens, and the name of each button as focus moves to it.

## How it works

The runtime keeps a **box on top** (`ar_top()`, [arcade.md](arcade.md),
"A box on top"): every dirty rectangle the program's `paint()` repaints
is followed, where it meets the box, by the box's own paint, clipped to
it; a dirty rectangle wholly inside the box is not painted by the
program at all. So the picture behind is always the program's own,
kept by its dirty tiles, and a box costs nothing while it waits. A
change in the box (focus, a character, a scroll) dirties the box's
rectangle and nothing else.

While a box is up the program is not running its frames: its overlays
are not drawn (what it last submitted is repainted away), its
`before_paint()` is not called, and Esc, F1, C and M are the box's keys
rather than the runtime's. A mode change (F11, C) or the window being
closed is handed back to the program after the box, through
`ar_push()`, as `AR_EV_MODE` and `AR_EV_QUIT`; a closed window answers
the box as Esc would.

Size: see [Using it](#using-it).

## Using it

An app on the runtime adds the directory and the object:

```make
SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui
OBJS = ... zarcade.o zarcade_sys.o sui.o ...
```

That brings F1 help on the screen with it (above). Size (riscv-none-elf-gcc
-Os): 12.6 KB of code and 6.5 KB of `.bss` (the folder's list is 4.4 KB
of it) -- against 18.9 KB and 10.3 KB for the desktop's `zdialog`,
`zflist` and `zedit` together.

## Testing

`make -C sw/common/games/ui test` runs a stand-in program -- a busy
sixteen-colour picture with sixteen different monochrome inks --
through every box in every mode (the window, mono full screen, 16, 64
and 256 colours, and 256 asked for on a six-plane machine), on
`tests/ar_host.c`'s software framebuffer, with ASan and UBSan:

- every answer: OK, Yes, No, Cancel, Esc, a closed window, the letters,
  Tab, Shift+Tab and the arrows, a click, typing and editing a line,
  a password, an empty line with and without `empty_ok`;
- every box's layout: on the screen, nothing cut off
  (`sui_last_layout_ok()`);
- the palette given back exactly after the boxes, with the default
  entries and with the program's own;
- a mode change inside a box handed back to the program afterwards;
- every frame against a full repaint (`AR_HOST_VERIFY`).

`make -C sw/common/games/ui render` writes the pictures as PNG,
`tests/out/<mode>/*.png` and a `sheet.png` per mode.
