# doom

The shareware episode of Doom, full screen in 256 colours, on boards
with chunky game mode ([color.md](color.md), "Chunky"): the 8-plane
45F/85F boards with `GAME_CHUNKY`. A one-off port, not part of the
release: it is not in `sw/apps/Makefile`'s APPS and not on any card.

## Building

```
make -C sw/apps/doom
```

The first build fetches what is not in the tree:

- **the engine**: [doomgeneric](https://github.com/ozkl/doomgeneric),
  cloned at commit `e257a09` into `sw/ext/doomgeneric`. GPL-2: id's 1997
  source release by way of Chocolate Doom. It is compiled from there
  unmodified, at `-O2`.
- **the WAD**: id's shareware `doom1.wad` 1.9 (4,196,020 bytes, MD5
  `f0cefca49926d00903cf57551d901abe`), from the Ubuntu archive's
  `doom-wad-shareware` package; `dpkg-deb`, or `ar` and `zstd`, unpacks
  it. The shareware licence allows passing it on unmodified.

Both are `.gitignore`'d, so **nothing GPL is in this tree**. What
`sw/apps/doom` adds is its own: `doom_zeitlos.c` (the platform layer,
`main()`, the WAD reader) and the Makefile. `doom.bin` links the engine,
so `doom.bin` is distributed under the GPL-2, with the engine's source.

## Running

Copy `sw/apps/doom/doom.bin` to `/apps/doom` and `doom1.wad` to
`/data/doom/doom1.wad` (any route in [build.md](build.md): the card,
`tget`, `zlink put`), then `run doom`. Doom keeps `default.cfg` and its
saves in `/data/doom/` too.

The engine's own options work: `run doom -timedemo demo1` plays the
first demo as fast as it can and prints `timed 5026 gametics in N
realtics` (frames a second = 35 x 5026 / N); `-warp 1 3`, `-skill 4`,
`-nomonsters` and the rest. `-stats` prints instructions and cycles a
frame, frames a second and late frames when Doom ends. Started with no
options, doom reads them from `/data/doom/args` if there is one (words
separated by spaces or new lines).

`run doom -fps`: options are passed as the launch argument. `-fps` shows frames a second in the black band above the picture,
updated once a second (doomgeneric's own `-devparm` ticker is not
drawn: its `I_DisplayFPSDots()` is empty). `-notvsync` copies each frame to the screen at once instead of at the
next frame boundary: faster, with a tear.

`-membench` measures what memory costs (loads and stores over buffers
much larger than the data cache, in the patterns the renderer makes),
prints it and ends without starting the game.

On a bitstream without chunky mode it says so and runs **without a
picture** (useful only with `-timedemo`); `-headless` asks for that
anywhere.

## Keys

| Key | Doom |
|---|---|
| Arrows | move and turn |
| Ctrl | fire |
| Space | open, use |
| Shift (held) | run |
| Alt (held) + arrows | strafe |
| `,` `.` | strafe left, right |
| 1-7 | weapons |
| Tab | map |
| Esc | Doom's menu (and quit, from there) |
| Enter, `y`, `n` | menu answers |
| `-` `=` | smaller, larger view |
| F1-F8 | Doom's: help, save, load, sound, detail, quick save, end game, messages |
| Super+Esc | leave (wm takes the screen back; Doom saves its settings and ends) |

F9-F12 are Zeitlos's ([keys.md](keys.md)), so Doom's quick load (F9),
quit (F10) and gamma (F11) are only in its menu.

Ctrl, Shift and Alt as buttons need wm's `Z_WM_GAME_GRAB_MODIFIERS`
([window_manager.md](window_manager.md)): a modifier pressed on its own
normally reaches no app.

A pad works as well: the d-pad moves, A fires, B uses, X runs, Y
strafes, Start is the menu and Select the map.

## How it fits

- **The picture.** The engine draws 320x200 at a byte a pixel; chunky
  game mode is 320x240 at a byte a pixel, so the frame goes into VRAM as
  it is, 20 rows down (black above and below), copied at the frame
  boundary: VRAM has room for one picture only, and a copy started at
  the boundary stays ahead of the beam. The engine's palette (gamma
  applied) goes into the 256-entry palette, as RGB444, at the same
  moment. Palette flashes (damage, pickups) are whole-palette changes,
  as in the original.
- **Memory.** `APP_STACK = 8M`, the most a program can have: the
  engine's zone is 6 MB of it. Lumps are read from the WAD as needed,
  not preloaded.
- **The WAD** is read through the fs syscalls (`zfsapp.h`), a whole lump
  a request, in place of the engine's stdio reader; `default.cfg` and
  saves go through stdio (`POSIX_IO`).
- **Wrapped, not edited** (`-Wl,--wrap`): `M_SetConfigDir` (to
  `/data/doom/`), `M_FileExists` (through `fs_size()` first, which
  the simulator has too) and `I_FinishUpdate` (to VRAM straight from the
  engine's own screen, not through a second 64,000-byte copy in main
  memory first).
- **No sound** yet.

## Speed

The simulator (`sim/`, `zsim-headless -r <root> -m 33554432 doom.bin`
with `/data/doom/args` holding `-timedemo demo1`) plays all of demo1:
5,026 tics, **about 426,000 instructions a frame** (a frame and a tic
each, in a timedemo), the copy to VRAM not included. Where they go:
`R_DrawColumn` 43%, `R_DrawSpan` 23%, the wall loop 6%, `memcpy` 4%.
(The simulator's clock is the host's, so its time-based waits -- the
screen melt between levels above all -- spin for a long time there; on
hardware they take what they should.)

On hardware the number that counts is `-timedemo demo1`'s.
