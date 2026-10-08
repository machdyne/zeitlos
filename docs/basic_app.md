# basic -- the BASIC computer

A small computer inside Zeitlos, the way home computers worked: one
320x240 screen where you type, `PRINT` and draw, in a window or full
screen. Full screen works on a TV through the composite output too.

The language is Machdyne BASIC 1 (`sw/ext/basic`, a submodule; the language
reference is upstream's `docs/basic1.md`), with graphics and a few other
statements added for this machine. It is a learning language, and the
app keeps it to itself: its files are in `/data/basic` and nowhere else, there
is no `FORMAT`, and it cannot touch the rest of the system.

```
MACHDYNE BASIC 1

READY.
10 FOR R = 10 TO 100 STEP 10
20 CIRCLE 160, 120, R
30 NEXT
RUN
```

## Using it

| Key | Does |
|---|---|
| Enter | carries out the line: a command, or a program line to store |
| Backspace | erases the last character |
| Up | recalls the last line entered |
| Escape | erases the line being typed; stops a running program |
| Ctrl+C | stops a running program |
| F6 | full screen, or back to the window |

Typing goes through the keyboard layout, so `PRINT "Grüße"` shows what was
typed: the screen and BASIC's strings are Latin-9 (ISO 8859-15), as in
the 8x8 font (`z_font_8x8`, `sw/data/font/font8x8.mem`: Daniel Hepper's
public-domain font8x8, completed to Latin-9).

`/data/basic/BOOT.BAS`, if there is one, runs when BASIC starts.

## From a terminal

BASIC is also the port `basic0`: in `term`, F11 and `port basic0`. The
terminal types into the same BASIC as the window, with the same keys, and
shows everything it prints -- text only: graphics are drawn in the BASIC
window as always, and `term` shows none of them. Characters travel as
UTF-8 both ways, as everywhere in Zeitlos, so `Grüße` and `€` work.

One terminal at a time; a second is refused. Closing BASIC tells the
terminal so. `F6` (full screen) is the window's key only.

## The screen

320x240 pixels and 40x30 characters of 8x8. Text and graphics share the
screen: text is white on black (or `INK` on `PAPER`, see Colour) and
replaces what is in its cell; a line printed at the bottom scrolls
everything up, drawings included.

## Beyond BASIC 1

| Statement or function | Does |
|---|---|
| `CLS` | clears the screen; the cursor goes home |
| `COLOR c` | how later drawing is done: 0 black, 1 white, 2 inverted (drawing the same shape twice with 2 erases it) |
| `INK c` | draw, and print, in colour c (0-15) |
| `PAPER c` | the colour text is printed on, and `CLS` clears to (0-15) |
| `PALETTE i, r, g, b` | colour i (0-15) is red r, green g, blue b, each 0-15 |
| `PLOT x, y` | one point |
| `LINE x1, y1, x2, y2` | a line |
| `BOX x1, y1, x2, y2` | a rectangle; `BOX x1, y1, x2, y2, 1` fills it |
| `CIRCLE x, y, r` | a circle |
| `POINT(x, y)` | the point's colour: 1 white, 0 black (also off the screen), 2-15 other colours |
| `LOCATE row, col` | where the next text goes: rows 0-29, columns 0-39 |
| `SCREEN 1`, `SCREEN 0` | full screen, or the window |
| `KEY` | the next key pressed while the program runs, or 0 |
| `SYNC` | shows the screen and waits for the next video frame |

Coordinates are pixels: 0-319 across, 0-239 down. Drawing beyond the
screen is not an error; what is on the screen is drawn. `COLOR` other
than 0-2, a negative radius, or `LOCATE` off the screen is `OUT OF
RANGE`. `SCREEN 1` on a bitstream without game mode is `NOT SUPPORTED`.

`KEY` gives a key's Latin-9 code (`A` is 65, Enter 13, Backspace 8), and
128-131 for Up, Down, Left and Right. Keys pressed while a program runs go
to `KEY`, oldest first, up to 32 of them; Escape and Ctrl+C stop the
program instead.

For smooth animation, draw a frame, then `SYNC`:

```
10 X = 0
20 CLS: CIRCLE X, 120, 10: SYNC
30 X = X + 2: IF X < 320 THEN GOTO 20
```

## Colour

Sixteen colours, with `INK`, `PAPER` and `PALETTE`:

```
10 PAPER 6: INK 1: CLS
20 FOR C = 2 TO 15
30 INK C: BOX C * 20, 40, C * 20 + 15, 200, 1
40 NEXT
50 INK 7: LOCATE 27, 1: PRINT "SIXTEEN COLOURS"
```

The colours to start with are the Commodore 64's:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 black | 1 white | 2 red | 3 cyan | 4 purple | 5 green | 6 blue | 7 yellow |
| 8 orange | 9 brown | 10 light red | 11 dark grey | 12 grey | 13 light green | 14 light blue | 15 light grey |

0 is black and 1 white, so `COLOR` means what it always meant: `COLOR
0` draws in colour 0, `COLOR 1` is `INK 1`, and `COLOR 2` inverts (it
turns white into black and back; on other colours it flips them to a
partner colour, and drawing twice still erases). Text is printed in the
ink colour on the paper colour and replaces its cell, as before.

**Where the colours show.** Full screen (`SCREEN 1`, F6) on a machine
with game-mode colour ([color.md](color.md)) shows them as colours. The
window cannot -- the desktop is black and white -- so there the paper
colour is black and every other colour white: text stays readable
against its paper, and a drawing stands out against the screen. Full
screen on a machine without colour shows the same.

That is a choice, not a limitation of the hardware. Showing each colour
as a dithered grey of its brightness was tried first, and a program
that set a coloured `PAPER` came back from full screen as a window
full of dots with its text lost in them. Nothing is reset when the
screen changes: the program's colours, ink and paper are all kept, and
F6 shows them again.

One thing follows from "the paper is black": an area cleared with an
earlier `PAPER` shows white after the paper changes, until the next
`CLS`.

**Full screen in colour is single-buffered.** Sixteen colours need the
whole framebuffer, so there is no second page to draw into: the changed
rows are copied just after a frame boundary, and a big change can show
half drawn for one frame. `SYNC` still paces a program to the display.

**A program that never asks for colour is not changed by it.** Until
something other than `INK 1` or `PAPER 0`, or any `PALETTE`, the screen
is the one plane it always was, drawn by the same code, and full screen
is the double-buffered monochrome it always was.

## Files

`SAVE`, `LOAD`, `DIR`, `DEL`, `TYPE` and the data files of BASIC 1 work on
`/data/basic` (created when BASIC first starts). Names are BASIC's: up to 8
letters and digits and a 3-letter extension, `.BAS` if none is given. A
`SAVE` replaces the old file only once the new one is complete
(`/data/basic/_SAVING.TMP` in between).

## I2C on the bench

A netlist can put the BASIC computer's pins 3 and 4 on a bench bus
([bench.md](bench.md)): `basic main`. Then, with bench running, `PINS -,
-, I2C, I2C` succeeds and `I2C` and `I2CR` reach the parts on that bus,
exactly as on a module's pins C and D. `/data/bench/examples/basicpanel.net` puts it
on a TCA9535 with LEDs and buttons: `LOAD PANEL`, `RUN`.

`NET` on pins 1 and 2 means "left to the system", and costs nothing
here, so a module's program (`PINS NET, NET, I2C, I2C`, as
`/data/basic/PANEL.BAS`) runs unchanged on the BASIC computer -- develop it
here, with the screen, then send it to a module with `sechs send`.

The bus is looked up at every `PINS`, so a netlist edited and reloaded
in bench (F5) is used by the next `RUN`. Without bench, or without a
`basic` line, `PINS ... I2C, I2C` is `NOT SUPPORTED`.

## What it does not have

- **Other pins.** `IN`, `OUT` and `ADC`, and modes other than the above,
  are `NOT SUPPORTED` (`PINS -, -, -, -` works, as everywhere). Giving
  BASIC a PMOD would be a configuration setting, later.
- **`LED`** does nothing.
- **`FORMAT`** is `NOT SUPPORTED`.

## Limits

Programs of up to 32KB (BASIC 1 promises 1KB; about 50 to 70 typical
lines fit in each KB), integers from -32768 to 32767, 26 variables `A`-`Z`,
as BASIC 1 everywhere.

## How it works

| File | |
|---|---|
| `bscreen.c` | the screen: an image in the app's memory, in the framebuffer's own format, four planes once a program uses colour; text, scrolling, the cursor, drawing (each pixel of a shape drawn once, so inverting works), and the colours in black and white for a monochrome display |
| `bedit.c` | typing a line, Unicode to Latin-9 |
| `bext.c` | the statements above, through the interpreter's extension interface (`BASIC_EXT`) |
| `bterm.c` | a terminal's bytes (`basic0`): UTF-8 and VT100 arrows in, as wm's keysyms; Latin-9 out, as UTF-8 |
| `basic_app.c` | the window, full screen, the `basic0` port, keys, showing the screen, and what the interpreter asks of the system (files, time, stopping) |

A program runs inside the interpreter's `basic_yield()`. The interpreter
asks `hw_break()` at every program line and `hw_delay_ms()` while it
waits; both read wm's messages (redraws, keys) and show the screen at
most once per video frame, so the window stays alive while a program
runs. `WAIT` and `SLEEP` give the CPU back (`z_proc_wait()`).

Keys from wm and from a terminal wait in one inbox and are acted on after
the messages are read: the message handler never prints. Output to the
terminal is batched, and making room in a full port means reading
messages for their acknowledgements, so a handler that printed would be
called from inside its own printing.

Full screen is game mode with the game grab (`Z_WM_GAME_GRAB`): wm sends
the keys here, through the keyboard layout as in the window, and stops
managing windows until BASIC gives the screen back or wm takes it
(`Z_WM_GAME_REVOKED`, Alt+Esc). BASIC does not read the raw keyboard
queue, which wm drains too (see `sw/apps/gamedemo/gamedemo.c`).

## Machdyne BASIC in Zeitlos

`sw/ext/basic` is a git submodule: Machdyne BASIC
(https://github.com/machdyne/basic), as `sw/ext/ms` and `sw/ext/te` are,
with no local changes. Run `git submodule update --init --recursive`
after cloning (the README says so too). Three apps use parts of it:

| App | Uses |
|---|---|
| `basic` | the interpreter, `basic.c` and `basic.h` (and `fs/fs.h` for its file codes), with its extension interface (`BASIC_EXT`) |
| `ls99` | the same interpreter built `BASIC_PROFILE` to play an LS10 or LS11 exactly, and the Sechs module core, `sechs/sechs.c` |
| `sechs` | the Sechs master library, `tools/sechs/sechsm.c`: the code Linux's `sechsctl` runs |

Zeitlos needs a BASIC with `BASIC_PROFILE` and `basic_input` (commit
`778b2d1` or later). To move to a newer one: `git -C sw/ext/basic pull`,
then `make test` in `sw/apps/basic`, `sw/apps/ls99` and `sw/apps/sechs`,
and commit the new submodule pointer. BASIC's own `make test`, run in
`sw/ext/basic`, compares `BASIC_PROFILE` with the real LS10 and LS11
builds.

## Tests

`make test` in `sw/apps/basic`, on a host, with no toolchain or board:
the screen's exact pixels (text, scrolling, the cursor, every shape drawn
twice inverted leaves nothing; colour: drawing, text in ink on paper,
scrolling in paper, inverting, black and white for the window, and that a program without
colour never touches the other planes), and BASIC programs typed into the real
interpreter with the extensions and a host stand-in for `basic_app.c`
(`tests/host.c`): printing, Latin-9, `INPUT`, every statement above,
files, and a 20KB program; and a terminal's bytes, UTF-8 both ways for
every Latin-9 character, arrows, and Escape alone.
