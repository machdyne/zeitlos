# Colour

Game-mode colour: up to 16 colours in the 320x240 game-mode viewport,
from the unchanged 640x480x1bpp framebuffer, for one block RAM.

The desktop stays monochrome. Nothing about the 640x480 mode, the
framebuffer, the blitter, the rasterizer or any existing program
changes. Colour is off at reset and off whenever game mode is off.

## The idea

Game mode already shows only a quarter of the framebuffer. The other
three 320x240 regions are off-screen. Colour reads up to three of them
as extra **bitplanes** of the same picture:

```
  framebuffer 640x480                     what is shown, 320x240
  +-------------+-------------+
  |  plane 0    |  plane 1    |           index = p3 p2 p1 p0
  | (viewport)  |  (+320, 0)  |                    |
  +-------------+-------------+    ==>    palette[index] -> RGB
  |  plane 2    |  plane 3    |
  |  (0, +240)  | (+320,+240) |
  +-------------+-------------+
```

Each pixel's colour index is one bit from each plane at the same
position, and a 16-entry palette turns the index into a colour.

That framing is what keeps it cheap and compatible:

- **The rasterizer and the blitter are unchanged.** A plane is ordinary
  1bpp framebuffer. Drawing in colour is drawing into each plane — see
  [Drawing](#drawing).
- **No new VRAM.** The planes live in VRAM that already exists.
- **No new resolution, timing or pixel format.** The display still sees
  640x480@60Hz, pixel-doubled, exactly as in monochrome game mode.

## What the framebuffer holds

640x480 is exactly four 320x240 regions, so every colour depth is a
trade against spare pages:

| Colours | Planes | Spare 320x240 pages | What the spare is for |
|---|---|---|---|
| 2 | 1 | 3 | monochrome game mode, as before |
| 4 | 2 | 2 | **either** a back buffer **or** 640-wide scroll room |
| 8 | 3 | 1 | sprite storage (VRAM-to-VRAM blits); single-buffered |
| 16 | 4 | 0 | single-buffered, fixed view |

The 8- and 16-colour layouts have no back buffer. A game using them
draws during vertical blanking or redraws only what changed, or accepts
tearing.

## Plane placement

Plane 0 is the viewport. Plane *k* (1..3) is the viewport moved by its
own offset: **dx** adds 320 columns and **dy** adds 240 rows, both
modulo 640x480. Two bits per plane, and one mechanism covers every
useful layout:

| Layout | Planes | Plane 1 | Plane 2 | Plane 3 | Viewport |
|---|---|---|---|---|---|
| 16 colours | 4 | dx | dy | dx+dy | (0,0) |
| 8 colours | 3 | dx | dy | — | (0,0); spare page at (320,240) |
| 4 colours, double-buffered | 2 | dx | — | — | flip between (0,0) and (0,240) |
| 4 colours, side-scrolling | 2 | dy | — | — | (x, 0), wrap on |

The double-buffered 4-colour layout has page A in the top half (plane 0
on the left, plane 1 on the right) and page B in the bottom half. A flip
is the same one VIEW write it is in monochrome.

The side-scrolling layout has plane 0 in rows 0..239 and plane 1 in
rows 240..479, each a full 640-wide strip. With wrap on, each strip is
a horizontal torus, exactly the model `zgame.h`'s half-pages already
use for monochrome scrolling.

Plane offsets **always wrap**, in clamp mode too. In clamp mode the
viewport stays inside the framebuffer, but a plane at +320 from an
origin of x=100 reads columns 420..639 and then 0..99. That is the
arithmetic, not a special case: a layout whose planes are side by side
has no horizontal scroll room, and the hardware does not pretend
otherwise.

## Registers

In `socctl` (`rtl/socctl.v`), alongside GAME and VIEW
([game_mode.md](game_mode.md), [socctl.md](socctl.md)).

| Address | Name | Contents |
|---|---|---|
| `0x7000_0224` | COLOR | W: bit 0 EN, bits 2:1 planes−1, bits 9:8 / 11:10 / 13:12 plane 1/2/3 `{dy,dx}`. R: `{ 0x5A50, avail, 0, offsets, 0, np, en }` |
| `0x7000_0234` | PALETTE | W only, whole-word store: bits 27:24 entry, bits 11:0 RGB444. Reads 0 |

COLOR is part of the GAME/VIEW payload: a write to any of the three
flips the same load toggle, and the whole payload is adopted at one
frame boundary. "Enter game mode, set the origin, turn colour on"
written as three stores lands on one frame.

**Turning game mode off turns colour off.** A GAME write with bit 0
clear also clears COLOR's EN, in hardware. The next entry into game
mode — including the window manager's Super+Esc over an ordinary
desktop — is monochrome unless whoever enters it asks for colour again.
Without this, a game that exited without tidying up would leave the
desktop's own pixels to be shown as bitplanes the next time anyone
panned.

### Why words 9 and 13

Earlier bitstreams decode only three address bits in socctl, so on them
word 8 is word 0 again, 9 is 1, and so on. Software must be able to
probe for colour without disturbing an old board:

- Word 9 aliases MAGIC there, and word 13 aliases FRAME. Both are
  read-only, so a write does nothing.
- MAGIC's top half is `0x5A43`, not COLOR's `0x5A50`, so the probe
  fails cleanly.
- Word 8 would have aliased CTRL and turned the cursor into a Z. Word
  15 would have cleared DIRTY bits under the screen streamer.

The decode is four bits wide now, so words 8, 10..12, 14 and 15 are
free and read as zero.

### Presence

| Check | Meaning |
|---|---|
| `Z_FEATURE2_COLOR` (FEATURES2 bit 15) | `COLOR` was defined in `rtl/boards.vh` |
| `z_color_present()` | this bitstream's socctl has the COLOR register |
| `z_color_available()` | colour can actually be shown: `COLOR`, `GAME` and `GPU` (on composite too) |

Use `z_color_available()`. `rtl/sysctl.v` ands the conditions before
telling socctl, so its AVAIL bit is the answer that matters, and EN is
forced low (and reads back low) where it is clear.

## Software interface (`sw/common/zsoc.h`)

```c
if (z_color_available()) {
    z_game_view_set_enabled(true, false);
    z_game_set_view(0, 0);
    z_color_set(true, 4, Z_COLOR_OFFSETS(Z_COLOR_DX,
                                         Z_COLOR_DY,
                                         Z_COLOR_DX | Z_COLOR_DY));
    z_game_wait_frame();
    z_color_set_palette(1, 0xf80);   /* orange */
}
```

| Function | Does |
|---|---|
| `z_color_set(on, planes, offsets)` | enable, plane count 1..4, plane 1..3 offsets |
| `z_color_enabled()`, `z_color_planes()` | read back |
| `z_color_set_palette(index, rgb444)` | one palette entry |

This is the register layer only; the drawing layer is
[`zcolor.h`](#the-colour-runtime-swcommonzcolorh).

## The colour runtime (`sw/common/zcolor.h`)

What every colour program would otherwise write for itself: where the
planes are, how to draw in colour *c*, and how to do it across the
640x480 torus. Every function takes coordinates **in plane 0** -- the
framebuffer coordinates a monochrome game already draws at -- and works
out the other planes. A rectangle that runs past column 639 or row 479,
or starts at a negative or folded coordinate, is split onto the torus,
so no blitter call ever goes outside the framebuffer (the blitter can
hang on one that does).

| Layout | Planes | Viewport | Use |
|---|---|---|---|
| `z_color_16` | 4, quadrants | (0,0) | 16 colours, single-buffered |
| `z_color_8` | 3 | (0,0) | 8 colours, spare page at (320,240) |
| `z_color_4_right` | 2, plane 1 at +320 | left half | flip y 0/240, or zgame's vertical scroll |
| `z_color_4_below` | 2, plane 1 at +240 | top half | flip x 0/320, or zgame's horizontal scroll |
| `z_color_mono` | 1 | anywhere | monochrome through the palette |

| Function | Does |
|---|---|
| `z_color_begin(&layout)`, `z_color_end()` | colour on with a layout, or off |
| `z_color_palette_load(pal, n)` | n entries, after the next frame boundary |
| `z_color_palette_load_now(pal, n)` | the same, for a caller that has just waited |
| `z_color_plane_xy(&l, k, x, y, &px, &py)` | where plane k of (x,y) is |
| `z_color_fill_rect(&l, x, y, w, h, c)` | a rectangle in colour c |
| `z_color_blit_mono(&l, src, ..., c)` | a 1bpp bitmap in colour c, transparent elsewhere |
| `z_color_blit_planes(&l, src[], ...)` | an opaque n-plane image (a NULL plane clears) |
| `z_color_blit_sprite(&l, src[], mask, ...)` | a masked n-plane sprite |

`z_color_palette_ega` (the power-on colours) and `z_color_palette_c64`
(the Commodore 64's: 0 black, 1 white, so a monochrome picture looks the
same through it) are ready-made palettes.

Tested on a host by `sw/common/tests/test_zcolor.c` (`make -f
Makefile.zcolor`): a software blitter and scanout, every layout,
rectangles inside and across both seams and from negative and folded
origins, every drawing call, and nothing outside the rectangle touched.
It fails when the seam split is broken.

## Programs

**chip8** ([chip8_app.md](chip8_app.md)). Full screen shows XO-CHIP's
four colours as colours. XO-CHIP has exactly two planes, and its colour
index is plane 0 in bit 0 and plane 1 in bit 1 -- the scanout's own --
so colour n is palette entry n. `z_color_4_below` puts plane 1 under
each of the two side-by-side pages chip8 already flips between, so the
flip did not change. Octo's default colours, or the ROM's own from
`chip8.cfg`. The window keeps its dithered greys.

**BASIC** ([basic_app.md](basic_app.md)). `INK`, `PAPER` and `PALETTE`,
sixteen colours, the Commodore 64's palette. Full screen with colour
uses all four quadrants: single-buffered. The window, and full screen on
a machine without colour, show the paper black and every other colour
white, so text stays readable (dithered greys were tried first and
were not). A program that
never uses colour runs exactly as before.

**gamedemo** ([gamedemo.md](gamedemo.md#colour)). Four colours,
double-buffered, `z_color_4_below`: the monochrome layout's scroll
room is spent on plane 1, since the renderer redraws every frame
anyway. Colour per object from the existing 1bpp art; every primitive
clips to the page, because a page is now exactly the viewport. Palette
effects: a day/night cycle, stars, lightning, animated waterfalls in
the pits, and colour-cycling for events. C toggles colour, T skips a
quarter of a day.

**space3d** ([space3d_app.md](space3d_app.md)). Sixteen colours as
a dual playfield: planes 0-2 are the wireframe in seven line colours,
plane 3 a nebula drawn once, which the line erase never touches -- so
the single-buffered display list works unchanged and nothing ever
repairs the background. Palette effects: twinkling stars in two
groups, flickering fire, a drifting nebula. C toggles it.

**Scheme** ([scheme_api.md](scheme_api.md)). `(color-mode)`,
`(palette ...)` and `(color-fill ...)`, for trying it out from a REPL.

## The palette

Sixteen entries of RGB444, written through PALETTE. Power-on contents
are the EGA/CGA 16 colours:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 `000` black | 1 `00a` blue | 2 `0a0` green | 3 `0aa` cyan | 4 `a00` red | 5 `a0a` magenta | 6 `a50` brown | 7 `aaa` light grey |
| 8 `555` dark grey | 9 `55f` light blue | 10 `5f5` light green | 11 `5ff` light cyan | 12 `f55` light red | 13 `f5f` light magenta | 14 `ff5` yellow | 15 `fff` white |

Reset does not reload these: the palette is distributed RAM, which has
no reset. A game sets the palette it wants.

A palette write takes effect immediately, not at a frame boundary. A
write that lands mid-picture can show as one wrong pixel. Write the
palette in vertical blanking — straight after `z_game_wait_frame()`.
Rewriting the palette every frame is cheap, so fades and colour cycling
are a handful of stores per frame.

### Per output

| Output | What the palette becomes |
|---|---|
| DDMI (HDMI/DVI) | each 4-bit channel doubled to 8 bits: any 16 of 4096 colours |
| VGA | the **top bit** of each channel: at most the 8 RGB primaries and secondaries |
| Composite | NTSC or PAL colour from a synthesised subcarrier; black and white available as a setting ([composite.md](composite.md#colour)) |

On DDMI the palette spans the full range. The monochrome inks keep
their historical 0x80 ceiling for compatibility; a game wanting the
desktop's exact white sets `0x888`.

Every VGA board in the tree has one resistor per channel, so VGA colour
is 8 colours whatever the palette says. The EGA default degrades
sensibly: its 0xa levels read as on and its 0x5 levels as off.

## Phosphor modes

The virtual phosphor (white, amber, green, paper —
[socctl.md](socctl.md)) picks ONE ink for a 1bpp picture. With a
palette there is no single ink to pick, so **while colour is on the
phosphor is not consulted at all**:

- colour on: output comes from the palette; paper mode does not invert
- colour off, in game mode: monochrome game mode, in the current
  phosphor, exactly as before
- desktop: always the phosphor, always monochrome

The phosphor setting is not changed or lost. The VIDEO register keeps
tracking it, and the moment colour goes off the monochrome path is
selected again in whatever mode the desktop was in.

## The cursor

In monochrome the pointer XORs the pixel. In colour it XORs the index
with 2^n−1, n being the number of planes in use: 0 becomes 1, 3, 7 or
15. It can never land on a palette entry the game is not using, and
it is visible over every colour that differs from its inverse.

## Drawing

A pixel of colour *c* is bit *k* of *c* in plane *k*, for each active
plane. So:

- **A filled shape in colour *c*:** draw it in every plane, set where
  *c* has a 1 and cleared where it has a 0. N blitter fills.
- **A masked sprite:** in every plane, clear the sprite's mask
  (`ROP_ANDN`), then OR in that plane's bits (`ROP_OR`).
  [gpu_blitter.md](gpu_blitter.md) has both ROPs.
- **Sprite storage:** the spare page, VRAM to VRAM, as in monochrome
  game mode.

Every coloured draw costs one blitter pass per plane. 4 colours is
twice the monochrome cost, 16 colours four times.

To find plane *k* of a point (x,y) in viewport coordinates: start from
the viewport origin, add 320 to x if plane *k* has dx, add 240 to y if
it has dy, wrap modulo 640x480.

## How it works

All in `rtl/gpu/gpu_video.v`, behind its `COLOR_AVAIL` parameter.

### Plane 0

Unchanged. It is the `hline` row buffer every mode has always used,
refilled once per physical line and indexed by the loadable column
counter `x`.

### The plane fetch (48 MHz, `clk`)

After `hline`'s 20 words, the same refill fetches three more rows of 20
words — planes 1, 2 and 3 for the same line — into a plane line buffer.
It uses the VRAM graphics port, which is dedicated to scanout and
otherwise idle in blanking, so it costs the CPU, blitter and rasterizer
nothing.

- **Timing.** 60 words at 48 MHz is 1.25 µs. With the `hline` refill
  and the clock crossing, it finishes about 45 pixel clocks into a
  160-clock horizontal blanking interval. Nothing reads the buffer until
  16 clocks before the line starts.
- **Rows.** Every plane's row is either the viewport's row or that row
  plus 240 (mod 480). The second is computed in the pixel domain,
  already multiplied by 20, and crosses with the same toggle and margin
  the `hline` row has always used. The 48 MHz side adds a word index to
  a latched base, the same depth as the existing refill address.
- **Whole rows, natural order.** The horizontal offset is not applied
  here. Whole rows are fetched and the read side picks the word.
- **Every plane, always.** The port is idle anyway, and skipping words
  would cost a comparison in the clock domain with no slack.

The buffer is 3 x 20 words: one DP16KD on ECP5, one RAMB18 on 7-series.

### The read side (25.2 MHz, `pclk`)

Each plane holds its current 32-bit word (`cur1..cur3`) and the next one
(`nxt1..nxt3`). The pixel's bit is `cur[x[4:0]]`, the same `x` that
indexes `hline`.

That one index serves every plane because a 320-column offset is
exactly ten words. All planes cross word boundaries on the same pixel,
and dx becomes "read word (w+10) mod 20" rather than an arithmetic
shift.

- **At a word crossing**, `cur` takes `nxt`, and a six-clock pass
  refills `nxt` from the buffer (three registered reads). The next
  crossing is at least 64 clocks away in game mode.
- **At the start of a line**, a preload pass loads the viewport's first
  word, moves it to `cur`, and loads the word after it. `x` already
  holds the origin through blanking, so a non-word-aligned origin is
  just a different `x[4:0]`.

### Index, palette, output

The colour index is `{p3,p2,p1,p0}` masked to the active planes and
XORed with the cursor. It reads the palette, a 16 x 12 distributed RAM
written in the wishbone domain and read asynchronously here. Blanking
is forced black.

### Where the logic is

Almost all of it is in the 25.2 MHz pixel domain, deliberately: that
domain closed at about 62 MHz on Lakritz against 25.2 needed, while the
48 MHz domain was at about 54 against 48. With colour, Lakritz measures
51 MHz on the pixel clock and 52.8 on `CLK_48`: the pixel domain took
the cost, as intended, and still has twice what it needs. The 48 MHz side gains only the
fetch sequencer, a 3:1 mux of latched bases and one adder.

### Without `COLOR`

`COLOR_AVAIL` is 0, `color_on` is a constant 0, the fetch never starts
and the block RAM has no writer. Yosys removes all of it, and the output
muxes fold back to the monochrome path.

## Cost

Measured by synthesising `gpu_video` on its own for ECP5 (yosys 0.33
`synth_ecp5`, with `GPU_DDMI`), `COLOR_AVAIL` 1 against 0:

| Cell | Without | With | Added |
|---|---|---|---|
| LUT4 | 995 | 1370 | +375 |
| CCU2C (carry, 2 LUTs each) | 136 | 178 | +42 |
| L6MUX21 | 100 | 132 | +32 |
| TRELLIS_FF | 804 | 1135 | +331 |
| DP16KD | 0 | 1 | +1 |
| TRELLIS_DPR16X4 (palette) | 0 | 3 | +3 |

That is roughly 460 LUT4-equivalents and 330 flip-flops: more than the
first estimate, still well under the ~1,500 LUT4 freed on Lakritz. The
flip-flops are mostly the six 32-bit plane words; flip-flops are the
resource the 25F has most of (about a third used).

With `COLOR_AVAIL` 0 the module synthesises to within one LUT4 of the
version before colour existed (994 LUT4, same flip-flops, same carry
and mux cells). Boards without `COLOR` pay nothing.

These are pre-placement numbers for one module, and **noisier than they
look**: re-synthesising textually different but logically identical
versions of this file (comments and line numbers only) moved the LUT4
count by several hundred, because ABC's mapping of the 640:1 `hline`
mux is sensitive to netlist order. Treat them as rough. The board-level
figure -- Lakritz measured at 98% with colour, all clocks passing
([boards.md](boards.md)) -- is the one to trust.

Other costs: no VRAM (the planes are framebuffer that already exists),
and 60 more words per line on the scanout-only VRAM port.

## Boards

`COLOR` is a per-board define in `rtl/boards.vh`, not universal like
`GAME`: it needs no pins, but it costs a block RAM and logic.

| Board | `COLOR` | Notes |
|---|---|---|
| Lakritz | on | With `USB_HOST`, `USB_CDC` and `AUDIO_MIXER`; paid for by dropping `MONTMUL`, `MONTMUL_REGS` and `SHA256`. 98% full, all clocks pass. TLS and SSH fall back to software: slower, same answers. See [boards.md](boards.md#lakritz-with-game-mode-colour). |
| Mozart ML1, ML2 | on | 45F: plenty of block RAM and logic |
| Sergei ML1, ML2 | on | 45F |
| Noir | on | 45F |
| Schoko | on | 45F, VGA and DDMI: VGA shows the top bit of each channel |
| everything else | off | 12F and 25F boards other than Lakritz, iCE40, GateMate, 7-series, composite |

On a composite build of a board with `COLOR`, colour goes out as a
real NTSC or PAL colour signal -- see
[composite.md](composite.md#colour), which also covers keeping black and
white for TVs that do not cope.

## Testing

`rtl/gpu/bench/tb_color.v` is self-checking. It fills the VRAM with a
fixed pseudo-random pattern and compares the DUT's colour index against
an independent model at **every visible pixel of a whole frame**, per
configuration.

Covered:

- 16 colours on the quadrants
- 4 colours double-buffered (lower page)
- 4 colours side-scrolling across the wrap seam from unaligned origins
- 8 colours with plane 3 masked
- odd origins wrapping on both axes, with shuffled offsets
- clamp
- cursor XOR at 2, 4 and 16 colours
- palette writes, VGA output and black blanking
- colour enabled with game mode off (the desktop path)
- colour off in game mode (phosphor honoured)
- colour on with paper selected (not inverted)
- mid-frame changes waiting for the boundary

The bench was checked against two deliberately broken variants of the
read side (no dx word rotation; one plane's word taken a pass late).
Both fail it.

```
sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
    rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
iverilog -g2005 -o /tmp/tb_color.out \
    rtl/gpu/bench/tb_color.v /tmp/gpu_video_fix.v
vvp /tmp/tb_color.out
```

With `-DGPU_DDMI -DTB_STUBS`, adding `rtl/gpu/gpu_ddmi.v` and
`rtl/gpu/tmds_encoder.v`, it also checks the 8-bit TMDS channel inputs.
Each run is about thirty frames: a few minutes, or about twenty with
DDMI, whose 126 MHz serialiser dominates the simulation. Prints
`RESULT: PASS` or `RESULT: FAIL`.

`tb_game_mode.v`, `tb_video_mode.v` and `tb_composite.v` (NTSC and PAL)
still pass: with `COLOR_AVAIL` at its default of 0 nothing they see
changed. `tools/hwmap/hwmap --check` reports no new findings.

## The window manager

Needs no change. Every way out of game mode -- Super+Esc, wm revoking
the grab when the app that holds it goes away, the lock screen -- is a
GAME write with the enable clear, and that clears colour in hardware.
Super+Esc over the desktop therefore always enters monochrome game
mode, whatever the last game left behind.

## Not done yet

- `COLOR` on the remaining 25F boards (ULX3S 25k, Klinge) and the
  ULX3S 85k. Each needs its own fit and `make timing`; the 25F ones are
  as full as Lakritz was.
- Composite colour on a real TV: simulated (`tb_cvbs_color.v`), not yet
  seen on a set.
