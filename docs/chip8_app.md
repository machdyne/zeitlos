# chip8

A CHIP-8, SUPER-CHIP and XO-CHIP emulator for Zeitlos.

One interpreter covers all three. They are not three machines but one
machine with about a dozen documented behavioural differences, so the
differences live in a struct (`c8_quirks_t`) rather than in three code
paths.

**Status: phases 0-7 complete.** The emulator runs in a window at 1x,
2x or 4x, renders XO-CHIP's second plane as ordered-dither greys,
drives the buzzer and XO-CHIP audio through the hardware mixer, goes
full screen through game mode with gamepad support, reads per-ROM
configuration, persists SUPER-CHIP flags, and has a built-in
disassembler and single-step debugger. Phase 8 is optional and gated on
measurement.

## Why this is software and not a CPU in the fabric

The obvious FPGA answer is to build a CHIP-8 core in RTL. It would be
small and it would work. It is still the wrong trade here, for four
reasons worth recording so the question does not get reopened by
accident.

**There is no performance case.** The SOC is RV32IM at 48MHz. A
switch-dispatch interpreter costs roughly 40-80 host instructions per
guest instruction. Real CHIP-8 titles execute 500-1000 guest
instructions a second and SUPER-CHIP maybe 2000; even a pathological
XO-CHIP program at 100k/s needs single-digit percent of the CPU.

`DXYN` is the one instruction that might have argued otherwise, and it
does not. With the display stored as 128-bit rows, an 8- or 16-wide
sprite row is a shift and two XORs -- see `row_xor()` in `core.c`. A
full 16x16 SUPER-CHIP sprite, collision detection included, is under
500 host instructions.

**The quirk table is the actual work, and it is hostile to hardware.**
Everything in `c8_quirks_t` is a divergence that real ROMs depend on,
and different ROMs need different combinations selected at load time.
In C that is a struct literal that can be changed and re-tested in a
second. In RTL it is a mode register threaded through the datapath,
and every wrong guess costs a synthesis, place-and-route and flash
cycle. The most volatile part of the design would be sitting in the
least changeable medium.

**A software interpreter can be debugged; a hardware one needs a debug
port built from scratch.** Single-step, breakpoints, register and
memory inspection, disassembly -- on a machine that already ships
`hex` and `logic`, those are most of the point.

**It would serve one app.** The hardware already added for this -- the
blitter's `Z_ROP_XOR`, the ordered-dither fill, the game-mode camera --
is the right kind: general primitives with many consumers. A CHIP-8
CPU has exactly one.

The one hardware idea still worth considering is a **2x/4x expanding
blit mode** in `gpu_blit.v`. The source shifter is already there, and
it would serve this app, `view`'s image zoom and `draw`'s zoom
together. That is a phase 8 item, gated on phase 2/3 measurement
showing the software expansion actually costs something.

## Files

Everything lives in `sw/apps/chip8/`. Nothing is added to
`sw/common/`; if something here turns out to be generally useful it
can be promoted later, but nothing has yet.

```
core.h  core.c       the guest machine. No I/O of any kind.
quirks.c             the three compatibility profiles, as data
render.h render.c    planes -> a 1bpp bitmap. Also no I/O.
config.h config.c    the CHIP8.CFG parser. Also no I/O.
disasm.h disasm.c    Octo-syntax disassembler. Also no I/O.
sound.h  sound.c     buzzer and XO-CHIP audio, on the mixer
debug.h  debug.c     the debugger pane
chip8.c              window, input, pacing, blit -- the only file
                     that owns the machine's state
chip8.cfg.example    a CHIP8.CFG to copy to a ROM directory
tests/test_core.c    guest machine tests
tests/test_render.c  scaling, bit order and dither tests
tests/test_config.c  config parsing and disassembly tests
tests/run_rom.c      headless ROM runner
tests/suite.sh       test-suite driver
tests/goldens.txt    expected display hashes
```

`core.c`, `render.c`, `config.c` and `disasm.c` include nothing from
`sw/common` -- no `zeitlos.h`, no `zgfx.h`, no framebuffer, no
filesystem. That is the single most useful property the emulator has:
the guest machine, the display expansion and the config parser compile
and test on a host in about a second, so a compatibility bug, a
half-pixel dither error or a section that never matches is a failing
assert on a build machine rather than a game that looks subtly wrong on
a TV.

One file outside the app directory changed: `sw/common/ztype.c` gains
three rows registering `CH8`, `SC8` and `XO8` with the file browser.
That table is the sanctioned extension point -- its own header says
adding a type is one line there and nothing else -- so this is a table
row, not new common code.

## Running it

```
> run wm
> run chip8            opens a file picker
```

or select a `.ch8` in `files`, which arrives through wm's launch
argument slot (`Z_WM_SET_ARG`, `zwm.h`).

```
  1 2 3 4        1 2 3 C
  Q W E R   ->   4 5 6 D     the guest's hex keypad
  A S D F        7 8 9 E
  Z X C V        A 0 B F

  SPACE          the gamepad's A button (hex 5 by default)
  ENTER          the gamepad's START button (hex F by default)

  F1 F2 F3   scale 1x / 2x / 4x
  F4         cycle compatibility profile (resets the machine)
  F5         reset
  F6         full-screen game mode
  F7 F8      slower / faster
  F9         screenshot to CHIP8SS.ZBM beside the ROM
  F10        debugger pane
  F11        single step        F12  run / pause
  Shift+F1   cycle the XO-CHIP grey mapping
  ESC        leave game mode, or quit
```

The titlebar carries an **open icon**: click it to pick another ROM
without going back to the shell. The outgoing ROM's saved flags are
written before anything can repoint at the new one.

**SPACE and ENTER are not on the hex keypad**, and a machine with a
keyboard should still do something sensible when you press the obvious
fire key -- they did nothing at all at first, which is how this got
noticed. They alias the *gamepad's* A and START rather than fixed hex
values, so one `pad a=` line in `CHIP8.CFG` moves the pad button and
the space bar together. That is what somebody editing that line means,
and it avoids a second mapping that could drift out of step with the
first. Default A is 5: the centre of the QWER/ASDF block and the key
most Octo titles use to act.

The alias has to be taught to `reconcile_keys()` as well -- a guest key
held via SPACE is not held via its own usage code, and missing that
would make the alias release itself one frame after being pressed.

The F-keys are deliberately all in that range and none of them is a
letter: `S` for step and `P` for pause would have been more natural and
both are guest keypad keys (hex 8 and, near enough, the rest of the
grid). There is no modifier available to disambiguate, because the
guest reads keys as a level and a chord would register as two keypad
presses.

The profile comes from `CHIP8.CFG` if the ROM's directory has one.
Otherwise `c8_profile_hint()` decides, and it looks at **size before
name**: a ROM larger than 3,584 bytes cannot be CHIP-8 or SUPER-CHIP,
because there is no address space for it to live in, so however it is
named it is XO-CHIP. That is arithmetic rather than a heuristic, and it
is a far stronger signal than a three-character extension that has
survived a trip through a FAT filesystem -- misnaming is common, and a
65,000-byte XO-CHIP ROM called `.CH8` is what prompted the rule.

Only then the extension: `.xo8`/`.xo`, `.sc8`/`.sc`, otherwise CHIP-8,
matched case-insensitively because FAT short names arrive uppercase and
people type lowercase. F4 still cycles it at run time.

## The display

### Storage

Two planes of 64 rows of 128 bits: `px[2][64][4]`, 2KB total. Rows are
MSB-first, so pixel column `c` is bit `31 - (c & 31)` of word `c >> 5`.

MSB-first because sprite data is MSB-first, which makes placing a
sprite row one shift rather than a bit reversal. This is the *opposite*
of the framebuffer's own convention (`zgfx.h`: least significant bit
leftmost) and that costs nothing, because the renderer never copies a
plane row verbatim -- it is always scaling, so it is always going
through a lookup table, and a table can be built either way round for
the same price.

Lores and hires are both stored at their own logical size, not
pre-scaled. A lores `DXYN` wraps at column 64, and storing lores
doubled would put the wrap in the wrong place.

### Scale

Scale is defined as **framebuffer pixels per hires pixel**. Lores
pixels are drawn at twice that. The content area is therefore always
128·S by 64·S and does not change when a ROM switches resolution --
which SUPER-CHIP and XO-CHIP ROMs do at runtime, so a window sized to
the current guest resolution would resize itself mid-game.

| S | content area | lores pixel | hires pixel |
|---|---|---|---|
| 1 | 128x64  | 2x2 | 1x1 |
| 2 | 256x128 | 4x4 | 2x2 |
| 4 | 512x256 | 8x8 | 4x4 |

Windowed mode offers all three, defaulting to 2. Game mode has no
choice to make: S=2 is the largest that fits the 320x240 viewport
(`docs/game_mode.md`), and the hardware doubling then puts it on
screen at 512x256.

### Greys

A 1bpp display and up to four XO-CHIP colours. Each output framebuffer
pixel is thresholded against a 4x4 ordered dither matrix indexed by its
**content-relative** position. Same table-driven code at every scale;
grey quality degrades by area rather than breaking when S is small.

#### A colour index is a palette slot, not a brightness

This was wrong at first and it showed up on hardware as an XO-CHIP
title with unreadable intro text.

Index 1 is Octo's `fillColor`, index 2 is `fillColor2` (plane 2 alone)
and index 3 is `blendColor` (where the planes overlap). Nothing about
that ordering says index 3 is brighter than index 1, and in Octo's own
default theme it is not -- the blend colour is the darkest of the four.

Mapping index order straight onto brightness -- 0, 5, 11, 16 -- puts
`fillColor`, the **primary** fill and what most XO-CHIP text and
sprites are drawn in, on the dimmest non-black grey. Worse, it breaks
continuity with the one-plane case: a colour 1 pixel rendered solid
white right up until the moment a ROM first touched plane 1, then
dropped to a 5/16 dither. Same pixel, same colour index, two
appearances. A title that draws body text and then a drop shadow
behind it has its text go unreadable at exactly that instant.

So the mapping is selectable, and the default is not the index order:

| `c8_palette_t` | 0 | 1 | 2 | 3 | for |
|---|---|---|---|---|---|
| `C8_PAL_FILL` (default) | 0 | 16 | 11 | 5 | most ROMs -- plane 2 as shadow or detail |
| `C8_PAL_INDEX` | 0 | 5 | 11 | 16 | ROMs that really do treat the four as a ramp |
| `C8_PAL_SOLID` | 0 | 16 | 16 | 16 | text-heavy ROMs; colour discarded for legibility |

Background stays black in every mapping. A dithered background is noise
across the whole screen, and 0 is the only level that is exactly and
seamlessly off.

Shift+F1 cycles it at run time; `palette fill|index|solid` sets it per
ROM in `CHIP8.CFG`.

Content-relative rather than screen-relative, which is the opposite of
`z_fb_hw_fill_shade()`'s choice (`zgfx.h`) and for a stated reason: a
shaded *fill* is a background wash that should tile seamlessly with its
neighbours, so it aligns to the screen. This image is one coherent
object, and aligning it to the screen would make its greys crawl every
time the window moved.

The four colours map to 0, 5, 11 and 16 sixteenths -- within a third of
a subpixel of evenly spaced, which is as close as 17 available levels
get.

### Two-plane detection

**A one-plane ROM's colour 1 is foreground white. A two-plane ROM's
colour 1 is the first of three greys.** Same pixel value, two different
correct answers.

`c8_t.two_plane` is latched the first time a program selects plane 1
(`FN01` with N >= 2) and never clears. `c8_levels()` reports 2 or 4.

Sticky rather than "does plane 1 currently have bits set", because the
latter makes the palette flicker: an XO-CHIP game that momentarily
clears plane 1 would have every remaining pixel jump from grey to white
for that frame. It is also one assignment instead of a 1KB scan per
frame.

Deliberately not keyed off the profile either. Plenty of ROMs run under
the XO-CHIP profile for its memory or its wrapping without ever using
the second plane, and they should look like what they are.

This was found the hard way: the first renderer picked the two-plane
palette unconditionally and every CHIP-8 game came out as a dim wash.
The IBM logo test rendering blank is what surfaced it.

### Rendering strategy

The plane is expanded into a staging buffer and blitted once per frame
with `z_fb_hw_blit_mem()`, dirty-row limited (`c8_t.dirty`, one bit per
display row). The renderer reports the band of output rows it touched
and only that band is blitted -- as one blit covering both, even when
the changed rows are far apart, because the blitter has no queue and
each operation pays a full setup and a wait for idle. A few extra rows
in one blit beat exactly the right rows in several.

Windowed blits go through the visible-region loop
(`z_gfx_blit_scissor()`), so a partially covered window paints only the
part of itself that is actually on screen rather than over whatever is
in front of it.

`DXYN` is deliberately **not** routed straight to a hardware XOR blit,
even though `Z_ROP_XOR` makes that possible. The software plane is
needed regardless -- for VF collision, and because XO-CHIP is two
planes -- and a second copy on screen is a divergence waiting to
happen. Games routinely draw 20-40 sprites a frame and most
erase-then-redraw, so per-sprite blitting is 40-80 blitter operations
where one deferred full-plane blit is one.

## Quirks

`c8_quirks_t` in `core.h`. Field names follow the vocabulary of the
CHIP-8 test suite, so a failing test names the field it fails on.

| field | CHIP-8 | SUPER-CHIP 1.1 | XO-CHIP |
|---|---|---|---|
| `vf_reset` | on | off | off |
| `mem_inc` | I+X+1 | unchanged | I+X+1 |
| `display_wait` | on | off | off |
| `clip_sprites` | on | on | off (wraps) |
| `shift_vx` | off (shifts VY) | on | off |
| `jump_vx` | off (BNNN) | on (BXNN) | off |
| `wide_sprite_lores` | n/a | on | on |
| `collision_rows` | off | on | off |
| `scroll_half_lores` | off | on | off |
| `clear_on_mode` | n/a | off | on |
| `addr_mask` | 0x0FFF | 0x0FFF | 0xFFFF |
| `flag_count` | 8 | 8 | 16 |

Two entries are worth calling out.

**SUPER-CHIP `mem_inc` is "unchanged".** Every other interpreter here
advances I after `FX55`/`FX65`. SUPER-CHIP 1.0 advanced by X+1; 1.1
does not, and 1.1 is what the surviving catalogue was written against.
A title predating 1.1 needs this overridden, and it presents as arrays
read back from the wrong offset -- garbled graphics rather than a
crash.

**XO-CHIP is not "SUPER-CHIP plus planes".** It reverts shifting,
jumping and sprite clipping to their CHIP-8 forms. It is a separate
branch from CHIP-8 that happens to keep SUPER-CHIP's screen modes.

A profile is a **starting point, not a verdict**. Real ROMs disagree
with their own family, so a ROM directory can override individual
fields per file -- see the next section. Getting the defaults right
just means most ROMs need no entry.

## Per-ROM configuration

`CHIP8.CFG`, in the same directory as the ROM. Uppercase and 8.3
because FAT short names are all this filesystem has.

```
# comments run to end of line; ; also works

[*]                        applies to every ROM in this directory
speed 20

[BLINKY.CH8]
name Blinky
profile schip
speed 30
set mem_inc=x1 shift=off
pad up=2 down=8 a=5 start=F
```

`set` names any field in the quirk table above -- `vf_reset`,
`mem_inc`, `display_wait`, `clip`, `shift`, `jump`, `wide_lores`,
`collision_rows`, `scroll_half`, `clear_on_mode`, `memory`. Booleans
take `on`/`off`/`true`/`false`/`yes`/`no`/`1`/`0`; `mem_inc` takes
`x1`/`x`/`none`; `memory` takes `4k`/`64k`.

`palette` takes `fill`, `index` or `solid` -- see "Greys" above.

`pad` names any of `up down left right a b x y start select` and gives
each a single hex digit. `a` and `start` also decide what SPACE and
ENTER do. Buttons the line does not name keep the app's
default rather than becoming unmapped, so a line naming two buttons
changes two buttons.

Three decisions worth recording:

**One file per directory, not a sidecar per ROM.** A ROM pack is
distributed as a directory, and one file that travels with it is one
thing to write, review and ship. Two hundred `.CFG` files beside two
hundred `.CH8` files also doubles the length of every listing in the
file browser.

**Ordering does not matter.** `profile` may appear after `set`.
Overrides are recorded with a mask of which fields were named and
applied on top of the profile at the end. A parser that applied each
line as it read it would silently discard every `set` above the
`profile` line -- exactly the kind of thing nobody thinks to test, and
there is a test for it.

**Unparsable lines are counted and reported**, not ignored. A typo'd
quirk name would otherwise present as "this ROM still misbehaves" with
a config file that looks perfectly correct.

`chip8.cfg.example` in the app directory is a copyable starting point.

## Exit, and what a ROM can actually do to the outside world

A CHIP-8 program has exactly two ways to reach past its own display
and keypad, and both turn up in menus as instructions to the player.

**`00FD` exits.** SUPER-CHIP added it; on an HP48 it returned to the
calculator. A ROM offering "press X to exit" is telling you which
keypad key it has wired to that instruction -- X is hex 0 on the
standard layout, so the key reaches the guest and the guest halts.

Halting is the ROM ending, so the app ends with it. This originally did
nothing at all: the guest halted, `c8_run()` stopped being called, and
the app went on drawing the same frame forever. Indistinguishable from
the keypress being ignored, which is exactly how it was reported.

The exception is the debugger pane. Having it open is a statement that
you want to watch the machine rather than play it, and the instruction
that stopped it is the one you most want to look at, so a halt with the
pane up freezes in place and says so. F5 resets, ESC quits.

**`FX75`/`FX85` save and load flags** -- eight registers on SUPER-CHIP,
sixteen under XO-CHIP. That is the whole of a CHIP-8 program's
persistent storage; there is no file access and no other state that
survives. So "press C to erase save file" means the game writing zeros
over its flags, and a high score table means the game reading them
back. C is hex B, and reaches the guest the same way.

## Saved flags

`FX75`/`FX85` are persistent storage from the guest's point of view --
on a real HP48 they survived the calculator being switched off, and
SUPER-CHIP games use them for high scores. They are written to the ROM's
own path with the extension replaced by `.FLG`, so a save travels with
the game and a ROM directory can be copied without losing anything.

Sixteen bytes are compared once a frame and written only on a change,
which against a bit-banged SD card is not a close call.

**The write waits half a second for the flags to settle.** `FX75`
writes at most sixteen registers, so a full save is up to sixteen
separate instructions and, at a low tickrate, several frames. Writing
on each of them would be several complete file writes to a card for one
logical save, during gameplay. Thirty frames of quiet is far longer
than any burst and far shorter than a player notices; a shutdown or a
ROM change forces the write immediately rather than waiting.

**The result is announced on the console** -- `saved /GAME.FLG` or
`cleared /GAME.FLG`. Without it there is no way to tell whether "press
C to erase" did anything: an erase is the guest writing zeros, which
from the outside looks exactly like a game that ignored the keypress.

## Memory

| | bytes |
|---|---|
| guest RAM | 65,536 |
| ROM image, kept for reset | 65,536 |
| display planes | 2,048 |
| staging buffer (4x) | 16,388 |
| dither table | 4,096 |
| everything else | < 1,024 |

About 150KB of `.bss` measured from the built objects, against a 1MB
process pool (`docs/app_runtime.md`). The ROM image is a second copy so
that F5 and F4 can reset without re-reading the file, which matters
because the picker may have been the only thing that knew the path.

### .bss is free; malloc() is not

This is the trap, and it cost a bug found on hardware.

A process's `malloc()` heap comes out of its **stack+heap allowance** --
16KB at `Z_PROC_STACK_SIZE_DEFAULT` (`sw/os/kernel.h`), shared with the
C stack. `.bss` is part of the binary image and is sized separately, so
the 64KB `rom_image` array above costs the allowance nothing.

So the obvious way to load a ROM -- `fs_mallocfile()` -- cannot work
for a large one. A 65,000-byte XO-CHIP ROM needs a 65,001-byte
allocation from a 16KB allowance, and fails every time regardless of
how much RAM the board has. It also could not say why: that function's
NULL return covers "missing", "allocation failed" and "read failed"
alike, by its own documentation.

ROMs are read in chunks straight into `rom_image` with
`fs_open_read()`/`fs_read_chunk()`. That removes the second copy --
there was never a reason to buffer 64KB in order to `memcpy` it into
another 64KB -- and lets each failure name the step that failed. No
kernel tier change is needed, which is a better outcome than moving
`chip8` to a larger allowance would have been.

The two remaining `fs_mallocfile()` calls read the saved flags (16
bytes) and `CHIP8.CFG`, which is size-capped at 8KB for the same
reason.

### ROM size limits

Programs load at `0x200`, so the ceiling is the address space minus
512 bytes:

| | address space | max ROM |
|---|---|---|
| CHIP-8 (COSMAC VIP) | 4KB | 3,584 in theory, 3,232 in practice |
| SUPER-CHIP 1.1 (HP48) | 4KB | 3,584 |
| XO-CHIP | 64KB | 65,024 |

The VIP figure is smaller because the interpreter kept its variables,
stack and display refresh buffer at `0xEA0`-`0xFFF`, leaving a program
`0x200`-`0xE9F`. SUPER-CHIP does not put the display in guest memory
and does get the full 3,584. XO-CHIP's `F000 NNNN` long load is what
buys the rest: a 16-bit address literal where `ANNN` has twelve.

`c8_load()` enforces the per-profile figure, and `c8_profile_hint()`
uses it as a classification rule -- see below. A `c8_t`
contains no pointers, so it can be memcpy'd -- which is what save
states will be built on.

One allocation of the full 64KB regardless of profile, with
`addr_mask` deciding what the guest can reach. A CHIP-8 ROM that runs
off the end therefore wraps at 0x1000 exactly as it did on a VIP,
rather than reading XO-CHIP memory a real CHIP-8 program could never
have seen.

## Input

CHIP-8's key model is "is hex key N down right now", which is a level,
not an event. The two modes read it differently and for a reason.

**Windowed:** maintain a 16-bit key bitmap from `Z_WM_KEY` press and
release messages (`zwm.h` delivers both, to the focused window's owner
only). Focus-respecting by construction -- a game does not keep
running while you type in another window.

**Game mode:** read the raw USB HID key registers as a level, the way
`gamedemo.c` does. There is no focus in game mode and nothing to
respect.

Gamepad via `zpad.h` with a per-ROM key map, because mapping a d-pad
onto a hex keypad is only sensible if the ROM says which four keys it
wants.

Default keyboard layout is the conventional one:

```
    1 2 3 4          1 2 3 C
    Q W E R    ->    4 5 6 D
    A S D F          7 8 9 E
    Z X C V          A 0 B F
```

`FX0A` completes on **release**, not on press. That is what the VIP
did and it is load-bearing: with press semantics one held key satisfies
every `FX0A` executed while it is down, so a menu that waits for a key
twice takes both answers from a single press.

### The lost-release problem

wm delivers key events to the **focused** window only. If focus moves
while a key is held, the release goes to somebody else and this app
believes the key is still down -- forever. On a machine whose guest
reads keys as a level, that is not a cosmetic glitch: the player's
character runs into a wall and stays there.

`reconcile_keys()` closes it. Once per frame, for each key this app
believes is down, it checks the raw USB report and clears the key if
the hardware disagrees. It only ever **clears** -- keys are still set
from wm's events alone -- so it cannot make an unfocused window respond
to typing, which is the thing the focus rule exists to prevent.

### Game mode input

No focus, no message traffic. The hex keypad is read straight from the
USB report as a level, the way `gamedemo.c` does, and turned into edges
here by diffing against the previous mask -- the edges still matter
because `FX0A` completes on one.

The gamepad d-pad maps to **both** movement conventions CHIP-8 games
actually use, 2/4/6/8 and Q/E, because a pad wired to only one of them
is useless for half the catalogue. Phase 6 makes this per-ROM.

## Timing

Both the 60Hz timer decrement and frame pacing come from the video
frame counter (`FRAME`, `0x7000_0214`), which is readable outside game
mode. Not the scheduler tick -- the guest's DT and ST are specified in
display frames, and reading the display's own counter makes them exact
under scheduler jitter rather than approximately right.

`display_wait` under the CHIP-8 profile is what caps VIP-era games at
one sprite per frame, and is why those games do not flicker. `c8_step()`
returns `C8_WAIT_FRAME` and the app stops running instructions until
`c8_frame()`.

## Game mode

Two pages of 320 columns side by side, viewport at the top of whichever
is not on screen. Nothing scrolls, so this needs none of `zgame.h`'s
camera machinery -- just the flip, whose order is the part worth
getting right: point the viewport at the page just drawn, then wait for
the boundary at which the hardware adopts it. A flip cannot tear
because the origin is only ever adopted between frames.

Scale is forced to 2x, and that is not a preference: the viewport is
320x240 framebuffer pixels and 4x would be 512x256. The hardware then
doubles it, so 2x reaches the screen at 512x256 physical -- twice the
size of the desktop presentation, with the 4x4 dither cell intact.

The whole image is blitted every frame rather than the dirty band. The
two pages are drawn on alternate frames, so a band that is current for
one is a frame stale for the other, and tracking that costs more than
4KB of blit.

The surround is cleared once on entry, not per frame. The image rect is
fixed for as long as game mode lasts -- scale is forced to 2 and the
output is 128*scale by 64*scale in *both* guest resolutions, so a ROM
switching to hires does not move or resize it.

Messages are drained in game mode too. The window still exists and wm
still talks to it; ignoring the queue would leave wm waiting on redraw
acks until its timeout fired, which presents as the whole desktop
freezing for a second rather than as this app being busy. Redraws are
acked without drawing.

### The exit key has to be released first

Game mode is entered *by* a keypress and its input is read as a **level**
from the USB report once inside. So on the first poll after entering,
the key that got us here is still physically down -- and game mode ends
in the same frame it began. From the outside that looks like F6
clearing the screen and doing nothing else, which is exactly how it
presented on hardware.

`exit_armed` fixes it: ESC and F6 must both be seen *up* once before
either counts. Not a timeout, which would be a guess about how quickly
somebody lets go of a key and would still fail for anyone who held it a
moment longer.

On exit, `Z_WM_REPAINT` to wm. Every window is still alive and still
where it was, but this app drew over all of their pixels and none of
them know.

## Audio

SUPER-CHIP is one square wave gated on ST. XO-CHIP replaces it with a
16-byte pattern buffer played at a programmable rate. Both are the same
thing here -- 128 samples in a looping buffer on one mixer channel --
which is why there is no separate square-wave generator.

That mapping is unusually clean and worth stating: the mixer walks a
buffer forever given `LOOPST`/`LOOPLEN`, and `CH_STEP` sets the rate.
So XO-CHIP's pattern register *is* a mixer buffer, its `FX3A` pitch
*is* `CH_STEP`, and a program rewriting its pattern mid-note is a
`memcpy` with the channel still running. There is no per-sample work
for the CPU at any point.

`c8_t.audio_gen` is bumped whenever the pattern or pitch changes, so
the sound code notices without diffing 16 bytes every frame. A rewrite
mid-note does **not** retrigger the channel: XO-CHIP programs modulate
the buffer while it plays, and retriggering on every write turns a
sustained tone into a clicking one. Only the ST gate ever triggers.

The pitch formula is 4000 * 2^((pitch - 64) / 48) Hz, done as a
48-entry Q14 table and a shift. A table because this core's floating
point is libgcc, and 48 constants are smaller and exact where a `pow()`
call is neither. The exponent is negative for every pitch below 64 --
most of the useful range -- and C's `/` and `%` truncate toward zero,
so the octave split floors explicitly rather than relying on them.

### The close-icon trap

The window is created with `Z_WIN_FLAG_CLOSE_ICON` but **not**
`Z_WIN_FLAG_CLOSE_KILLS_OWNER`, which is the usual choice for a
single-window app and is wrong here.

This process owns a hardware mixer channel. The mixer is a bus master
reading a buffer in this process's own address space, and the killing
form of the close icon skips `c8_sound_shutdown()` -- so the channel
keeps fetching from memory that has just been freed with the process.
Handling `Z_WM_CLOSE` in the message loop costs one case and makes
shutdown ordered.

## Testing

```
make -C sw/apps/chip8            build and run the unit tests
make -C sw/apps/chip8 roms       download the test-suite ROMs
make -C sw/apps/chip8 suite      run them against tests/goldens.txt
make -C sw/apps/chip8 suite-pgm  ... and write PGMs to /tmp/chip8-pgm
```

**Unit tests**: 186 checks in `tests/test_core.c`, 60 in
`tests/test_render.c` and 60 in `tests/test_config.c`.

The core tests are written against quirk fields by name. Nearly every
CHIP-8 compatibility problem is a one-line behavioural difference that
produces a game which *runs* and looks *wrong* -- no crash, no error,
nothing to catch. The only way to know is to state the expected
behaviour somewhere a machine can check it.

The renderer tests check exact positions and exact counts, not "some
pixels got set". A reversed bit order and a dither one pixel out of
phase both produce output that looks like a picture, so the tests
assert that guest (0,0) lands on the lowest bit of the first word, that
one lores pixel at 4x is exactly 64 lit pixels and nothing else, and
that a 16x16 block of grey level 1 is exactly 5/16 lit across the whole
run -- a per-block dither would get the per-pixel count right and the
joins wrong.

The config tests are the same idea applied to a parser whose bugs are
silent by construction: a section that never matches, a `set` line
above its `profile` line, a prefix matching where it should not, and a
typo'd field name all present as "that ROM still misbehaves" with a
file that reads correctly.

**Suite** (`tests/suite.sh`): runs Timendus' `chip8-test-suite` ROMs
headlessly and compares an FNV-1a hash of the resulting display against
committed values.

The ROMs are not vendored. They are someone else's MIT-licensed project
with its own release cadence, and a copy in this tree goes stale
silently; nothing in the normal build needs them. `suite.sh` exits 77
("skipped", the convention `sw/apps/hex/tests` uses) when they are
absent, so a machine with no network still passes.

The golden is a text file of hashes rather than a directory of images
on purpose: a behaviour change then shows up as one modified line
naming the ROM it happened in. `--pgm` is for when that line changes
and you need to see what it looks like now.

Every suite entry is tuned to end on a **settled** screen. Several of
these ROMs animate their menus -- the selection bullet blinks -- so a
hash taken while a menu is up depends on which frame the run stopped
at. Each entry ends on a result screen, most of them inside `FX0A` with
execution stopped entirely. Verified stable across frame counts; if you
retune an entry, re-check it at N and N+7.

### Current results

| entry | result |
|---|---|
| `1-chip8-logo` | renders |
| `2-ibm-logo` | renders |
| `3-corax+` | every opcode group ticks |
| `4-flags` | all ticks |
| `5-quirks` under CHIP-8 | all six checks tick |
| `5-quirks` under SUPER-CHIP 1.1 | all six checks tick |
| `5-quirks` under XO-CHIP | all six checks tick |
| `8-scrolling` lores, both profiles | coherent, profile-appropriate, hashed |

The quirks test reports back exactly what each profile claims: CHIP-8
as vF reset ON / memory ON / display wait ON / clipping ON / shifting
OFF / jumping OFF, SUPER-CHIP as OFF/OFF/NONE/BOTH/ON/ON, XO-CHIP as
OFF/ON/NONE/NONE/OFF/OFF.

`8-scrolling` is recorded honestly as a **regression golden, not a
verification**. Both profiles produce a coherent image and the two
differ in the way the half-pixel lores scroll quirk predicts, but there
is no authoritative reference here to diff against pixel for pixel. The
definitive check is running real SUPER-CHIP scrolling titles on the
board, in phase 2.

`6-keypad` and `7-beep` are downloaded but not in the golden set. Both
are inherently interactive -- one wants keys held and released while
you watch, the other wants ears. `EX9E`, `EXA1` and `FX0A` are covered
by unit tests; the beep is phase 4.

## Phases

| phase | deliverable | status |
|---|---|---|
| 0 | this document | done |
| 1 | interpreter core + host tests | done |
| 2 | windowed app, selectable 1x/2x/4x | done |
| 3 | XO-CHIP second plane + grey render | done |
| 4 | sound | done |
| 5 | game mode + gamepad | done |
| 6 | ROM library, launcher integration, per-ROM config | done |
| 7 | debugger, disassembler, screenshots | done |
| 8 | *optional* expanding blit mode in `gpu_blit.v` | gated on measurement |

`chip8` is in `sw/apps/Makefile`'s `APPS` list, and `CH8`/`SC8`/`XO8`
are registered in `sw/common/ztype.c` so the file browser opens them.

## The debugger

`F10` grows the window and draws a pane below the guest image:
registers, the run/pause state, and eight instructions of live
disassembly from PC. `F11` steps one instruction, `F12` runs and
pauses.

**A pane and not a second window.** A second window is the obvious
shape and it does not work: `Z_WM_SET_CLIP` (`zwm.h`) carries no window
id, so an app owning two windows cannot tell which one a visible-region
update is for. Whichever arrived last would win and one window would
draw over the other. `zdialog.c` gets away with it because its windows
are modal and strictly one at a time; a debugger that is useful is
neither. Growing the one window costs nothing extra -- the app already
rebuilds its window when the scale changes.

**Forward disassembly only.** Nothing is shown above PC. CHIP-8
instructions are not all the same length (XO-CHIP's long load is four
bytes) and code and data share one address space, so there is no way to
walk backwards that is right more often than it is wrong. A listing
that is confidently wrong above the cursor is worse than no listing.

**A paused machine does not age.** DT and ST stop with it. Ticking
timers while stopped would expire every one of them the moment you
resumed, which for a game means dying during the pause you took in
order to look at why you were dying.

**Nothing is ever blanked.** The first version cleared the pane with a
fill and redrew every line over the top, sixty times a second. On a
framebuffer that is scanned out directly, with no back buffer anywhere,
that is a clear and a redraw the display can catch mid-way -- the pane
visibly flashed on hardware.

There is no double buffer to reach for in a window, so the fix is to
stop blanking. `z_font_5x8` is resident in glyph memory (wm loads it at
startup), so `z_win_draw_text()` goes through the hardware glyph
blitter, which paints a **solid cell** -- background included. Every
character drawn therefore erases what was under it in the same
operation, with no blank state in between.

That makes the fill unnecessary provided every line is padded to a
fixed width, so a line that gets shorter overwrites its own tail. The
padding is load-bearing: without it, a four-byte `i := long 0x1234`
replaced by `clear` leaves a fragment behind that reads as a garbled
instruction rather than as stale pixels.

Once nothing is being blanked, the rest is not redrawing what has not
changed. The pane keeps a copy of what it drew and diffs it per
character, drawing only runs of changed cells -- in practice the few
digits of a register that moved, not 500 glyph blits a frame. The cache
is invalidated whenever the window is rebuilt, moved, or left behind
for game mode.

The disassembler emits Octo syntax, which is what the modern CHIP-8
toolchain reads and writes. It is not quite round-trippable: Octo
spells a call as a bare label and a ROM image has no labels, so `2NNN`
comes out as `call 0xNNN`. Everything else is real Octo -- including
the conditionals, which are **inverted** relative to the opcodes
(`3XNN` skips when equal, so the body runs when not equal). Getting
that backwards produces a listing that reads as the opposite program
and is entirely plausible, so there are tests for all four.

## Screenshots

`F9` writes `CHIP8SS.ZBM` beside the ROM. ZBM (`zbm.h`) because the
renderer's output is *already* in exactly that pixel format -- the
framebuffer's own, LSB leftmost -- so a screenshot is a 16-byte header
and the buffer with no conversion anywhere, and `draw` and `view` can
open the result.

## Not yet verified

What that leaves open, in rough order of how likely it is to bite:

- **The blit.** Position, clipping against a partially covered window,
  and the source alignment of an odd `dst_x`. The renderer's output is
  checked exhaustively; where it lands on screen is not.

  One window bug is already fixed: the first window of a session was
  asked for at the image's size with no allowance for wm's frame, so it
  came back with a content area smaller than the image and the picture
  was cropped by exactly the border. The frame's size is wm's business
  and is only knowable from a real window, so `open_window()` now
  measures it and asks a second time. One extra creation, once per
  session.

  A second is fixed too: the debugger pane flashed, because it cleared
  itself before every redraw. See "The debugger" above.
- **Pacing.** `frame_now()` prefers the video frame counter and falls
  back to the kernel tick at 5/61. The fallback has never been timed
  against anything.
- **The mixer channel.** `phys_of()` is copied from `music.c` and the
  buffer is 128 signed bytes, but the pitch table has only been checked
  arithmetically. A wrong `CH_STEP` is an audible pitch error rather
  than a failure, so it will be obvious and is worth listening for.
- **Game mode.** The flip order is the same one `zgame.c` documents,
  but the page layout here is this app's own.
- **SUPER-CHIP scrolling.** `8-scrolling` is recorded as a regression
  golden, not a verification -- see above. Real SUPER-CHIP titles are
  the check.
- **Filesystem writes.** The flags save and the screenshot both write
  through `zfsapp.h` and neither has been run against a real card.
- **The debugger pane's width.** 240 pixels is the widest line at 5x8
  measured by counting characters, not by rendering it. If it is one
  character short, the register line clips.

One thing found while cross-compiling that is **not** this app's:
linking against picolibc fails with undefined `__heap_start`,
`__data_size` and friends. `sw/apps/hello_win` fails identically, and
`sw/common/arch.mk` documents it as known and unhandled -- item 3 of
its picolibc list. newlib is the supported libc.
