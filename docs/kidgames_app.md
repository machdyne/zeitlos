# kidgames

A compilation of educational games for kids (roughly ages 5-10),
ported from [kidgames](https://github.com/machdyne/kidgames), which
targets ncurses on Kakao Linux.

    > run kidgames

Ten games behind one menu, in a single process. The original's
structure survives intact -- a game is a label and a `run()` function,
`games.c` is a table of them, and the menu calls `run()` directly.
What changed is everything underneath: a terminal became a
framebuffer, eleven colour pairs became one bit, and a keyboard-only
app became one that is fully playable with a mouse alone.

## Status

| phase | what | state |
|---|---|---|
| 0 | skeleton, build, dock registration | done |
| 1 | `kgui` runtime, on-screen keyboard, big font | done |
| 2 | save file, word lists, level-up screen | done |
| 3 | eight of the ten games | done |
| 4 | Memory Match | done |
| 5 | art pipeline, Name That Animal | done (placeholder art) |
| 6 | audio feedback | done |

**All ten games play.** Sixteen animals, tiered 6 / 6 / 4 by name
length. The art pipeline resizes and dithers whatever PNGs are in
`art/`, so replacing or extending the roster is a directory of images,
three lines in `animals.c` and one command.

## One screen size, always

The playfield is **320x240 in both display modes**, and this is the
decision everything else follows from.

Game mode (`docs/game_mode.md`) is a 320x240 camera over the 640x480
framebuffer, pixel-doubled on scanout. A window can be any size the wm
gives it. Supporting both properly would normally mean a layout that
reflows, and for a screen of hand-placed big letters, a keyboard and a
row of stars that is a great deal of arithmetic to get wrong --
`sw/apps/logic` is the standing reminder of how that ends.

So the window is created at exactly the size that yields a 320x240
content area and is **not resizable**. `KG_WIN_W`/`KG_WIN_H` are
derived from `Z_WM_TITLEBAR_H` and zwin.c's 2px inset rather than
written down as 324/255, because the inset is zwin.c's to change and a
hardcoded number would not follow it. `tests/test_layout.c` asserts
the result is 320x240, which is the only thing keeping the two copies
of that formula in step -- exactly the duplication
`z_win_content_rect()` warns about in its own comment.

Both modes then draw identical pixels to a different origin. There is
no reflow path because there is nothing to reflow, and a bug visible
in a window is the same bug in game mode.

### Game mode is the default, and does not page-flip

At 1:1 a 5x8 glyph is unreadable across a room by someone who is still
learning to read at all. Pixel-doubled it is comfortable. So the app
enters game mode at startup where the bitstream has it, and **F2**
drops to a window.

It does **not** page-flip, unlike every other full-screen app here.
`z_game_flip()` exists for things that redraw the world every frame; a
quiz screen redraws one field when a letter is typed. Flipping would
put that incremental draw on the page nobody is looking at, so every
partial update would have to become a full repaint -- slower, and a
rewrite of ten games' worth of drawing, to avoid a tearing risk a
static screen does not have.

### The camera points at the window

Game mode draws to **the window's content rect**, exactly as windowed
mode does, and the viewport is aimed at that rectangle. There is no
second coordinate system and no branch in any drawing primitive.

This is the second design, and the first one broke the mouse. Game
mode originally drew at framebuffer (0,0) and cleared the whole
640x480 on entry -- which looks reasonable until you notice that
**game mode is a camera, not a mode change**. Every window is still
alive and still exactly where it was, and `dispatch_mouse()` in wm.c
delivers `Z_WM_MOUSE` to the focused window *only while the cursor is
over it*, hit-testing in framebuffer coordinates. A playfield drawn at
(0,0) while the window sat elsewhere on the desktop therefore received
almost no clicks at all.

Aiming the camera at the window fixes it at the root rather than
patching the arithmetic: drawing, hit testing and wm's own hit test
all share one coordinate system, in both modes.

The range works out without luck. The window is 324x255 and wm keeps
it on a 640x480 desktop, so its content origin is at most (318, 238);
the viewport's clamp with wrap off tops out at (320, 240), exactly
where a 320x240 camera would start running off the framebuffer. A
fixed-size, unresizable window is what makes that hold -- a resizable
one would need the clamp checked.

Two consequences worth knowing. The viewport is re-aimed on
`Z_WM_WINDOW_MOVED`, or the camera would keep looking at where the
window used to be. And wm's own `Ctrl+Alt+Arrow` pan can move the
camera off the playfield; that is left alone rather than fought, and
costs only visibility -- input stays correct throughout, because none
of it depends on where the camera is pointing.

Nothing outside the window's clip is written in either mode, so
leaving game mode needs no `Z_WM_REPAINT` to wm. The desktop
underneath is exactly as it was.

Pages 1-3 of the framebuffer stay free. That is where staged sprite
art would go if the blitter turned out to have no one-pass masked
sprite.

## The pump

`kg_getkey()` replaces the original's `ui_getkey()` and is the reason
the ten games port with their control flow intact: it blocks until a
key arrives, exactly as `ui_getkey()` did.

What it does while blocking is the Zeitlos part. It drains the
mailbox, applies `Z_WM_SET_CLIP`, answers `Z_WM_REDRAW` through a
registered repaint callback and acks it, turns pointer presses into
keys where the on-screen keyboard caught them, and sleeps with
`z_proc_wait()` rather than spinning.

**The callback is not optional**, for the same reason `zdialog.c`'s is
not. A window that misses a `Z_WM_SET_CLIP` keeps drawing against a
region that is no longer true, and a window that has never had one
draws nothing at all, with nothing on the console to say why. Every
screen that can be on display while `kg_getkey()` blocks registers how
to redraw itself, and anything that takes over the screen
(`kg_message()`, `levelup_celebrate()`) saves and restores the
caller's.

The wait is indefinite when there is no deadline -- every input path
wakes us, so there is nothing to poll for. It shortens to
`Z_TICK_HZ / 10` only while a key is physically held down, to drive
the keyboard's pulse.

## Colour, on a display that has none

The original carries meaning in eleven ncurses colour pairs. Here
there are two colours and seventeen dither levels.

**Rule one: a shade never carries meaning on its own.**
`z_fb_hw_fill_shade()` degrades to a plain black or white fill on a
bitstream built without dither support, so a distinction drawn only in
shade silently disappears there. Every distinction is also a
difference in shape, position or inversion.

**Rule two, learned from a render: never put text on a shade.** This
was not anticipated. The menu's selected row was a shade wash plus
inverted text, on the theory that the wash carried the selection and
the inversion was the no-dither fallback. It is illegible: an ordered
dither has already lit half the pixels the letters need, so stroke and
background differ by almost nothing. A surface that carries text is
filled solid and the text inverted; a surface that carries no text may
be shaded.

The substitutions:

| original | carried | here |
|---|---|---|
| `CP_MENU_SEL` | selection | solid bar, inverted text |
| `CP_BIGFONT` / `CP_HINT_BLOCK` | your word vs the answer | solid vs dithered glyph |
| `CP_GOOD` / `CP_BAD` | right vs wrong | face sprite plus frame style (phase 5) |
| `CP_FRAME` | borders | 1px hardware fills |

## The on-screen keyboard

Six of the ten games ask for a typed answer, so "playable with the
mouse alone" is not achievable by adding hit boxes to what is already
drawn -- there has to be somewhere to click a letter.

It earns its space for two better reasons.

**It shows where the key is.** A five-year-old knows the letter and
cannot find it. The layout is therefore **QWERTY, not alphabetical**.
Alphabetical would be easier to scan and completely useless for the
actual task, which is "the screen says K, now find K on the thing in
front of me". Digits are drawn as a row in physical keyboard order for
the same reason, not as a calculator keypad. `tests/test_pad.c`
asserts the QWERTY order and the row stagger so a well-meaning tidy-up
cannot quietly sort it.

**It shows the key is still down.** Kids hold keys. Zeitlos does not
auto-repeat -- `sw/os/hid.c` diffs each USB report against the last and
emits press/release *edges* only -- so a key held for three seconds
produces exactly one character and nothing goes wrong. But the kid
does not know that, and it is a habit worth unlearning before it meets
a machine that does repeat. A key that is down draws inverted; held
past `KG_HOLD_NUDGE_TICKS` (about two thirds of a second) it pulses an
inner frame. Not a shade change, because a bitstream without dither
would lose it; not a blink-out, because a key that vanishes under a
finger reads as "broken" rather than "let go".

A second press of the same key inside `KG_DEBOUNCE_TICKS` is dropped.
Contact bounce and a kid drumming on one key look identical from up
here and neither should produce two letters.

**Clicks become keys.** A pointer press inside the pad is turned into
an ordinary key event by the pump and never surfaces as
`KG_KEY_CLICK`. Games that only take typed answers therefore need no
mouse code at all. Only games with their own clickable board -- Memory
Match, Letter Hunt, the menu -- ever see a click.

The keyboard-to-pad and click-to-pad paths are separate code over the
same table, so `tests/test_pad.c` asserts they land on the same index.
If they disagree, pressing C lights some other key, and the pad exists
to show a kid where C is.

### Pointer coordinates in game mode

In game mode the pointer is over the **desktop**, not over the
pixel-doubled viewport, and wm's coordinates are desktop coordinates.
The viewport shows framebuffer (0,0)-(319,239) doubled, so a screen
pixel maps to playfield `(sx/2, sy/2)` and the window's own position is
irrelevant. Using the windowed translation there put every click at an
offset that depended on where the window had been left before the mode
switch, which looks exactly like a miscalibrated pointer.

## Big letters

The 5x7 dot-matrix font is the original's, unchanged. Five columns is
wide enough to give M, N, W, K, X, Y and R real diagonal strokes,
which a 3-column font collapses into look-alikes.

It is **not** a `z_font_t` and cannot be. Hardware glyph memory is
written by one process board-wide -- `wm`, at its own startup -- and an
app that loads a font corrupts every other process's text. There is
also no room in the table, and the blitter's glyph mode tops out at 8
pixels wide regardless.

So big letters are drawn as rectangles, which is what they are.
Naively that is one fill per lit pixel: up to 35 blitter operations
per letter. `kg_big_putc()` instead merges vertically adjacent rows
with identical bit patterns, then emits one fill per run of lit cells
within each group. 'L' is two fills rather than thirteen; 'A' is seven
rather than nineteen. Over the whole font that is 6.4 fills per glyph
against 19.4 lit cells.

`tests/test_layout.c` draws every glyph at one pixel per cell and
compares the framebuffer to the bitmap cell for cell, in both
directions. A run splitter that is subtly wrong produces letters that
are subtly wrong, and nobody reads a 'B' with a stray fill as a bug in
a run splitter.

## Frames are fills, not lines

Nearly every frame in this app has text inside it, and the blitter and
the line rasterizer do not order against each other: where rasterizer
chrome and blitter glyphs share a 32-bit VRAM word, the chrome comes
up intermittently missing, position-dependently, which reads as a
geometry bug. `docs/widgets.md` has the worked example -- the save
dialog's filename field, where the outline filled in as characters
were typed and completed at six.

`kg_frame()` is therefore four `z_fb_hw_fill_rect()` calls. A
one-pixel-wide fill is a perfectly good line.

## No printf, no floats

One conversion specifier links picolibc's formatter: on the order of
100KB, on an app that crashes on start if it outgrows the space the
loader has for it. `fputs` is no escape either -- it needs a `FILE`,
which drags in about 40KB of stdio on its own.

Numbers go through `kg_utoa()`/`kg_itoa()`/`kg_append_num()`
(`kgfmt.c`) and out via `puts()`. `make noprintf` fails the build if a
formatter comes back. It strips comments first with
`-fpreprocessed -dD -E`, because the first version of the check failed
on this very paragraph's counterpart in the source.

Floating point is banned for the rv32i boards, where one `double`
pulls in the whole soft-float runtime. `make nofloat` is the lexical
check and `make nmcheck` is the definitive one, scoped to this app's
own objects -- `sw/common/zobj.c` has a float member and is linked by
every app in the tree, which is a separate question.

## Persistence

`/user/kidgames.sav`, one line per game:

    spelling 120 3
    counting 40 2

The original's format exactly, because it is hand-editable and
corruption-tolerant, and because a parent who wants to reset a level
can do it in `text` without being told how.

FatFs here is built `FF_USE_LFN 0`, so 8.3 is not advice -- a longer
name cannot be written to the card at all. `kidgames.sav` fits, and so
does the app binary's own name at exactly eight characters.

**Failure is normal.** A Zeitlos machine boots to a desktop with no
sdcard at all, and then there is nowhere to write. Everything degrades
silently to "no saved progress": the games keep an in-memory score and
say nothing. A kid does not need to be told about a filesystem, and a
modal error would be the first thing they ever saw from this app.

Writes happen at natural breakpoints only -- leaving a game, levelling
up -- never per answer. An SD write goes over bit-banged SPI and takes
long enough to be felt, and feeling it after every letter would make
the whole app seem slow.

The parser is hand-rolled because `sscanf` links the scanner the same
way `printf` links the formatter. It is about fifteen lines, which is
less than this explanation. `tests/test_data.c` drives it against
every way a hand edit can go wrong: a line with no numbers, a
non-numeric score, a missing level, comments, blank lines, and no
trailing newline. Each must be **skipped**, leaving other games'
progress intact -- a parser that read a malformed line as "score 0,
level 0" would not report an error, it would silently reset progress
and then write that back.

A level of 0 in the file is clamped to 1 on the way in, once, rather
than at ten games' call sites: it would otherwise index a word list at
-1. And `kg_save_format()` refuses a write that would not fit rather
than truncating, because a half-written last line is skipped by the
parser -- so one game's progress would vanish on every save from then
on, quietly, and only for whichever game sorted last.

## Random numbers

`kg_rand()` (`kgrand.c`) over `z_rng_below()`, seeded from `rtl/trng.v`
where the board has one and from stirred state where it does not. A
board with no TRNG still deals a different word each boot, which is the
failure a kid would notice first.

It is its own file so the host tests can replace it with a
deterministic generator. That is not merely a stub: a test of "does a
level-3 word have five letters" is a test of the word list, not of the
generator, and it has to fail the same way every run to be worth
anything. A test that draws from real entropy and fails one time in
forty is a test people learn to re-run.

## Files

    kg.h          geometry, key and charset enums, the monochrome rules
    kgui.h/.c     window, mode switching, the pump, drawing primitives
    kgwidget.c    menu, message box, text field
    kgpad.h/.c    the on-screen keyboard
    kgfont.h/.c   5x7 big block letters
    kgfmt.c       number formatting, with no formatter
    kgsave.h/.c   /user/kidgames.sav
    kgrand.h/.c   the one random call
    wordlist.h/.c shared level-tiered word lists
    levelup.h/.c  the shared "LEVEL UP!" screen
    kground.h/.c  the shared round: header, result, scoring, face
    games_word.c  Spelling, Unscramble, Missing Letter
    games_number.c Counting, Simple Math
    games_guess.c Number Guessing, Word Guess, Letter Hunt
    games_memory.c Memory Match
    games_animal.c Name That Animal
    animals.h/.c  the roster: 16 animals, name and picture, three tiers
    kgsound.h/.c  three cues, through the hardware mixer
    gen_art.py    art/*.png -> kgart.c/.h  (output committed)
    kgart.h/.c    GENERATED -- do not edit
    game.h        the plug-in interface
    games.c       the registry
    kidgames.c    main

    tests/kg_shim.h        host stubs zrender.h does not supply
    tests/kg_rand_host.c   deterministic generator for the tests
    tests/render.c         every screen, as an image
    tests/test_layout.c    bands, font runs, containment, labels
    tests/test_pad.c       the keyboard
    tests/test_data.c      the save file and the word lists

## The games

Eight games, three files, because the screens fall into three shapes
and the original's one-file-per-game layout duplicated each shape
several times over -- including its bugs. The praise wording and the
pause lengths had already drifted between the eight copies there.

`games_word.c` -- Spelling, Unscramble, Missing Letter. One screen: a
word in big letters, a field under it, an answer checked against the
list. They differ in what is shown and what is wanted, and those
differences are now the only thing in the file that varies.

Missing Letter accepts **any letter that spells a word the list
knows**, not just the one drawn. Blank the first interior letter of
"cat" and "cot" and "cut" are equally correct English; insisting on
the drawn word would tell a five-year-old that a word they know is
wrong. It also blanks interior letters only -- English has huge rhyme
families at word edges (_AT, CA_, _OG) and far fewer in the middle --
so the list check is a safety net rather than the main mechanism.

`games_number.c` -- Counting, Simple Math. The countable object is a
**diamond**, not the five-pointed star the game's wording implies and
not the original's `*`. A five-pointed star at 22 pixels in one bit is
a blob with texture, and the whole task is looking at a group and
knowing how many there are: each shape needs a hard edge and a gap
around it and nothing else. Each row is centred on **its own** width,
because a last row of three left-aligned under a row of five reads as
one group with a hole in it.

`games_guess.c` -- Number Guessing, Word Guess, Letter Hunt. The three
with a board and state across several answers, so they score a round
rather than an answer.

Number Guessing's higher/lower hint is a **triangle**, not the
original's coloured word. It is the one piece of information the round
turns on, and the audience includes pre-readers. Word Guess draws
un-guessed letters as `KG_BIG_GHOST` (an empty outline) rather than
`KG_BIG_BLANK` (a filled block): blank means "a letter goes here and
you know which", which is Missing Letter; ghost means "you know
nothing about this yet". They stay apart by shape, not shade. Letter
Hunt marks the cursor with a box **around** the letter rather than
inverting it, because the task is reading that letterform and
comparing it to another, so it has to stay exactly as legible selected
as unselected.

`games_memory.c` -- Memory Match. The one game with genuine 2D
navigation, which changes three things the others share. The arrow
keys move **literally** here (up and down change row) rather than
going through `kg_key_is_next()`, and they **clamp rather than wrap**:
wrapping in one dimension is a convenience and wrapping in two is a
way to lose the cursor. Space still means "move" -- it advances in
reading order -- because a key that means "move" in nine games and
"flip" in the tenth is an inconsistency a five-year-old absorbs as
"sometimes it does the wrong thing". And a round is a whole grid, so
levelling is by **efficiency** (clearing with at most two wasted
guesses) rather than a streak, with a threshold of two rather than
five.

Four card states have to be told apart at a glance, and none of them
may use shade, because three of the four carry a letter and kg.h's
second rule forbids text on a dither. So: face-down is a **diagonal
hatch** (`kg_pattern`, the blitter's 8-row pattern fill); face-up is
the letter in a plain frame; matched is the letter plus a solid bar
across the card's foot, below the letter where it cannot interfere
with reading it; and the cursor is a double ring **outside** the card,
in the gap. The hatch is diagonal specifically -- a grid or a checker
aligns with both the card edges and the hardware's 8-pixel anchor, so
a field of them reads as one continuous texture and the cards stop
being visible as objects, which is fatal when the task is remembering
which card was where.

Clicking a card also moves the cursor, so the keyboard and the mouse
never disagree about where "here" is.

`games_animal.c` -- Name That Animal. The only game that draws art
rather than letters, which is why it was last: it needed `gen_art.py`
and `kg_sprite()` before it could exist. Otherwise the plainest of the
ten, and it uses the shared round unchanged.

`kground.c` carries what all nine share: the header, the result
screen, the scoring rule, the save. `kg_round_finish()` is one call
because the **order** matters and was subtly different between games in
the original -- the save must happen before the result screen (which
blocks for over two seconds), and the level-up celebration after it, or
the kid sees "LEVEL UP" before being told they were right.

### Scoring a round versus scoring an answer

`kg_round_finish()` scores, saves, shows and celebrates.
`kg_round_show()` only shows. The split exists because two games score
themselves: Number Guessing pays by how few guesses it took, and
Memory Match by how efficiently the grid was cleared.

This is not a hypothetical tidiness. Memory Match was written calling
`kg_round_finish()` after its own scoring and awarded double points
and a double streak on every cleared grid -- the kind of bug that
looks like generous game design until somebody adds up the numbers.

### The face

`kg_face()` draws the happy or sad face from rectangles. That is not a
placeholder for phase 5: a face is two eyes and a mouth, and at 64
pixels in one bit that *is* the drawing. It costs seven fills and no
data.

The mouth took two attempts. The first was a bar with its ends turned
up or down -- three fills, and on paper unmistakable. Rendered, both
variants came out as a **bracket**: a horizontal rule with two stubs,
differing only by which side the stubs were on. Neither read as a
mouth. Since this face is the entire replacement for the green and red
the original used for right and wrong, and has to work for someone who
cannot read the word underneath it, that was fatal.

It is a parabola now, one short vertical fill per column, integer-only:

    dy = t^2 * lift / half^2

A smile hangs down in the middle, a frown peaks up. Same code, one
sign, about thirty fills.

## Art

    art/*.png  ->  gen_art.py  ->  kgart.c / kgart.h

**96x48, 2:1.** Source images may be any size -- they are fitted,
contrast-stretched and dithered down. Anything that is not 2:1 is
letterboxed onto white rather than squashed, because the art direction
here is silhouette proportion and a stretched animal is a different
animal.

The **height** is the pinned dimension and the width is not. The piece
is drawn at 2x, so 96x48 becomes 192x96 on the playfield, and it sits
between the prompt and the answer field -- which cannot move, because
the on-screen keyboard is below it. That leaves 107 screen pixels, so
53 source rows is the hard ceiling and 48 fits with slack. Width has
far more room: 320 pixels of playfield is 160 source columns.

The generated files are **committed**, the same convention
`sw/data/icons/gen_dock_icon_data.py` and chess's `gen_pieces.py`
follow: the build needs no Python and no Pillow, and a change to the
art is a reviewable diff rather than a silent difference between two
people's builds.

### Dithering

`--mode fs` (default) is Floyd-Steinberg, `--mode ordered` is the same
Bayer 4x4 `kg_shade()` uses on the target -- so a piece in that mode
shares its cell structure with every shaded surface in the app -- and
`--mode threshold` does not dither at all.

The part that needed measuring was the **background**. Error diffusion
has nowhere to put its error on a large flat area that is nearly but
not quite white, so it sprays isolated dots across the whole picture.
On a black playfield every one of those dots is lit. Illustrations
almost never have a mathematically pure white background; a JPEG round
trip or a soft shadow is enough.

A fixed cutoff cannot fix it. On the first set of test images 250 did
nothing at all -- autocontrast had already left the paper below it --
while a hand-tuned 235 cut isolated pixels from 68 per piece to 12.
Which of those is right depends entirely on the source, so `--white
auto` (the default) finds the histogram's brightest peak instead: for
a picture of an animal on a background, that peak *is* the background.
It measured 9.7 isolated pixels per piece, beating the hand-tuned
number without anyone having to tune it. `--white N` forces a fixed
cutoff and `--white off` keeps every shade.

Only the light half of the histogram is considered, and a peak holding
less than a twentieth of the pixels is ignored: a mostly-dark
picture's peak is its *subject*, and snapping that to white would
erase the animal.

### Look at the preview

`gen_art.py --preview` writes `art-preview.png`, a contact sheet of
every piece at 4x with its name. It is the only way to find out that a
dither turned a spider into a smudge -- the same argument as `make
render` for the screens, and it is how the background speckle above
was spotted in the first place.

The run also prints an ink percentage per piece, because the two ways
this goes wrong silently both show up in that one number: a
light-on-dark source comes out mostly ink, and a failed threshold
comes out nearly blank. Neither raises an exception and both look like
bad art rather than a bad conversion.

### The packing

The packing is the framebuffer's own bit order -- pixel x at bit
`(x & 7)` of byte `(x >> 3)`, **least significant bit leftmost** --
which is the opposite of what most image tools write, so it is done
explicitly. Each array carries four bytes of padding past the last
row, because the blitter may read one word beyond what it needs when
source and destination are not word-aligned, and our destination x is
whatever centres the sprite, so misalignment is the normal case.

### No mask plane

`z_fb_hw_blit_sprite()` takes data and mask and makes two passes, so a
sprite lands over an arbitrary background. This app emits no mask and
does not use it, for two reasons, the second deciding:

- every piece is drawn onto a rectangle this app has just cleared, so
  there is nothing to mask against; and
- a masked sprite is **two passes** on a blitter without the one-pass
  cookie, which leaves the sprite's footprint momentarily blank.
  Everything else would flip that away on a back page. This app
  deliberately does not page-flip, so a two-pass sprite would punch a
  visible one-frame hole in the screen a kid is looking at.

So `kg_sprite()` clears the rectangle and COPIES the data plane: one
blit, atomic, and it works on any bitstream that has the memory-source
mode at all. If art ever needs a patterned background, add `--mask` to
`gen_art.py` and check `z_fb_hw_rop_available()` first.

### Doubling

Pieces are stored at 96x48 and drawn at 192x96. Storing them at the
full drawn size would be four times the bytes -- 37KB of art rather
than 9KB -- which matters on an app whose binary size is the one thing
never yet measured.

The blitter copies; it does not scale. Doubling in software into a
buffer the blitter then moves in **one** operation costs 3072 bit
tests once per question, on a screen that then sits still while a kid
types. The alternative is one fill per source pixel: three thousand
blitter operations for one picture.

### When the bitstream has no memory-source blit

`z_fb_hw_blit_mem()` is a blitter mode some bitstreams predate, and
zgfx.h is explicit that a binary built here may meet one. Rather than
a blank rectangle, the game draws a framed box saying the picture is
missing and keeps playing -- the round still scores and the result
screen still reveals the name. `kg_sprite_available()` is probed once
at startup, not per draw.

That branch is invisible on any machine likely to be tested on, so it
is exactly the kind of thing that rots. `tests/kg_shim.h` returns true
for the probe; flipping it to false renders the fallback, which is
worth doing whenever that message's layout changes.

## Sound

Three cues, and only three: right, wrong, level up. The original has
no audio; the argument for adding it is the one that drives the faces
and the star rows -- a lot of the audience cannot read, and a sound
says "yes" faster than a picture of a smile does.

**Keystrokes are deliberately silent.** A click per key sounds like a
good idea for the same reason the keyboard's pulse is, and it is not:
a kid hunting for a letter presses a lot of wrong keys on the way to
the right one, and a machine that chirps at each is a machine an adult
turns the volume down on. These cues mark events, not input.

They play through the hardware mixer as **one-shots**: point a channel
at a buffer, set its length, trigger, and the DMA plays to the end and
stops (`LOOPLEN` of 0 is one-shot). No CPU after the trigger, which
matters -- the alternative is pushing samples into the DAC FIFO for
the sound's whole duration, from inside a message pump with other work
to do.

Channel **7**, not 0. Every other app that makes a noise takes channel
0 -- chip8's buzzer, gamedemo's music, audiotest. The mixer is global
state that outlives any one app, so a cue on channel 0 would cut off
whatever a tracker in another window was playing.

Silence is a normal outcome. No audio block, no mixer in the
bitstream, or no DAC wired all end with `kg_sound_available()` false
and every call doing nothing. Nothing in the app checks.

### The bus-master trap

**The mixer does not go through the MTU.** Handing it an app pointer
does not fail quietly: it requests an address nothing decodes, an
undecoded address on this bus never acks, and the mixer holds its
grant on the main arbiter forever. The CPU is starved of the bus and
*the machine* stops -- not the app. `phys_of()` translates, the same
way chip8, gamedemo and track all do. This is now the fourth copy of
that function, which is a better argument for moving it into
`sw/common` than the third was.

### Triangles, not squares

Every other beep in this tree is a square wave, and a square is the
wrong choice here: its odd harmonics make it read as an **alarm**,
which is exactly the wrong thing to say to a six-year-old who has just
got an answer wrong. A triangle is two multiplies and sounds like a
note.

Each note fades to zero at its end. A tone that stops at full
amplitude is a step in the waveform, and a step is a click -- on a
small speaker, louder than the note.

The wrong-answer cue is one low note, **not** a descending pair. The
descending pair is the obvious mirror of the right-answer cue and
sounds like a rebuke; this app never punishes a wrong answer, it shows
the correct one and moves on, and the sound should agree with that.

### The test that earned itself immediately

`tests/test_data.c` checks that no cue overruns the 4096-sample
buffer, and caught the level-up arpeggio on the run it was written:
400ms against a 371ms buffer, so the last note was cut off partway.
Nothing failed -- `note()` appends and stops at the limit -- it just
ended at full amplitude, which is a click. Nobody was ever going to
report "the level-up sound ends slightly abruptly" as a bug.

`kgsound.c` is built with `-DKG_SOUND_HOSTED` for the tests, which
drops the four entry points that touch mixer registers and keeps the
**renderer**. Stubbing the whole file would have made that check test
nothing.

## Adding a game

1. Write `game_yourgame.h/.c`, using `game_spelling.c` as a template.
2. Draw with `kgui.h` only -- never reach for `zwin.h` or `zgfx.h`.
   A game that can see them can draw with the wrong coordinate
   convention, which is the single most common mistake in this area.
3. Pull words from `wordlist_pick()` rather than a private list, so
   the vocabulary stays shared and there is one place to review for
   age-appropriateness.
4. Use `kg_save_load()`/`kg_save_store()` with a unique, stable `id`.
5. Call `levelup_celebrate()` after your own result screen when the
   level goes up.
6. Add one line to `GAMES[]` and the `.c` to the Makefile's `APPOBJS`.

The menu picks it up automatically -- and scrolls, though ten games
currently fit without it, so an eleventh needs no other change.

## The link, checked without a cross toolchain

`fs_read_file()`, `fs_write_file()` and `fs_mkdir()` are declared in
`sw/common/zfsapp.h` and defined in `sw/common/zfsapp.c`, so using them
needs `zfsapp.o` in the link. It was missing, and **nothing on the
host side noticed**: `includecheck` only resolves headers, and every
host test builds `kgsave.c` with `-DKG_SAVE_HOSTED`, which compiles
those three calls out entirely. The first thing to complain was the
real RISC-V link -- the one build step that needs a cross toolchain,
and so the one step that could not be run while this was written.

`make linkcheck` closes that. It compiles every target source and
every `COMMONOBJS` source with the **host** compiler, then checks that
no symbol of ours is left undefined across the lot: the link step in
miniature, needing no cross toolchain. `KG_SAVE_HOSTED` and
`KG_SOUND_HOSTED` are deliberately not defined for it, since those two
files are exactly the ones whose real bodies it has to see.

One hole, named rather than papered over: `zrng.c` has RISC-V inline
asm (`rdcycle`) and cannot be assembled on an x86 machine, so its
exports are allow-listed by the `z_rng_` prefix.

It found a second problem on its first run. `TARGETSRC` had been
written out by hand alongside `APPOBJS` and the two had drifted:
`kgart.c` was in the objects and missing from the source list, so the
generated art was the one target source **no** check had ever looked
at. `TARGETSRC` is now `$(APPOBJS:.o=.c)`, because the real fix for
two lists that disagree is one list.

## Testing

    make test      unattended: headers, the link, no floats, no printf,
                   and the three suites
    make render    draw a screen and LOOK AT IT

`make render WHAT=` takes `menu`, `pad`, `digits`, `field`, `msg`,
`font`, `result`, `counting`, `math`, `numguess`, `wordguess`,
`letterhunt`, `missing`, `memory`, `memory1` and `animal`. Every game screen has
one -- `memory` and `memory1` are the largest and smallest grids,
since a centring bug shows at the extremes and nowhere else. And
`tests/render.c` `#include`s the game sources so it can call their
draw functions without the input loop around them.

The two are not redundant. A geometry assertion can only check a
relationship somebody thought to write down, which is exactly how
`sw/apps/logic` shipped three layout bugs against three successive
passing tests. Three of the first four problems in this app were found
by looking at a render and none of them by a test:

- the selected menu row was illegible (text on a dither);
- the digits keyboard left 34 pixels of blank band between the answer
  field and the keys, which read as a gap in the middle of the
  keyboard rather than as room -- it is bottom-aligned into the band
  now, and `kg_pad_top()` exists so a caller places the field against
  the real keys rather than against `KG_PAD_Y`;
- the text field was one pixel shorter than the glyph scale it then
  chose, so every typed answer had its bottom row clipped. Invisible
  at 1:1 on a desk; pixel-doubled it is a letter with its feet cut
  off. The height and the scale are derived from each other now.

Phase 3 added three more, all found the same way and none catchable by
a geometry assertion:

- the sad face read as neutral (above);
- Letter Hunt's cursor is a double frame drawn five pixels above the
  letter, and it cut a line through the caption on the row above --
  every round, since the cursor is always somewhere. Nothing was
  outside the playfield and no two rectangles a test knew about
  overlapped. A marker's own thickness is part of its row's height and
  has to be budgeted for;
- Simple Math was sized to a flat scale of 7, which is 49 pixels of a
  154-pixel play area: a one-digit sum floating in a screen two thirds
  empty. It starts at the tallest that fits and comes down now.

The fourth suspected problem in phase 1 -- a missing header rule -- was
not real.
Checking the framebuffer numerically rather than squinting at a render
settled it in one command, which is worth doing before changing
anything.

`make render` writes a PBM at 2x, which is pixel for pixel what game
mode puts on the glass.

The containment test is the one that earns its keep: it fills the
framebuffer, draws deliberately oversized things off all four edges,
and requires every pixel outside the playfield to be untouched. On
hardware `z_fb_hw_fill_rect()` clamps to the **screen**, not to a
window, so a rectangle that runs past our own edge lands on whatever
app is next to us -- and in game mode it lands in the off-screen pages
where the sprite art will live. Both are silent.
