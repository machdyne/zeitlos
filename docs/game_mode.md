# Game mode

A 320x240 viewport over the unchanged 640x480 framebuffer,
pixel-doubled on scanout.

The display timing does not change. The monitor sees the same
640x480@60Hz signal in both modes -- which is the entire point on a TV
that will not accept anything else, and the reason this is not a
"video mode" in the usual sense at all.

## What it actually is

There is no second framebuffer, no second resolution and no second
pixel format. There is one 640x480x1bpp surface at `0x2000_0000`, the
same one there has always been, and game mode moves a **camera** over
it.

```
  framebuffer, 640x480                    display, 640x480@60Hz
  +---------------------------+
  |         .---------.       |           +---------------------+
  |         | 320x240 |       |    ==>    |                     |
  |         | camera  |       |           |   each pixel 2x2    |
  |         '---------'       |           |                     |
  |                           |           +---------------------+
  +---------------------------+
```

Everything follows from that framing:

- **Nothing is destroyed by switching.** Every window is where it was,
  every app is still running, the window manager still believes the
  screen is 640x480 -- because it is. Only the part being looked at
  changes.
- **The rasterizer and the blitter needed no RTL changes.** As far as
  `gpu_raster.v` and `gpu_blit.v` are concerned there is still exactly
  one 640x480x1bpp surface, which is all there ever was. They draw
  into any part of it, visible or not.
- **No new BRAM, no extra VRAM bandwidth.** `gpu_video.v`'s scanline
  buffer already held a full 640-pixel framebuffer row per physical
  line. Game mode indexes it differently and reads a different row; in
  game mode each framebuffer row is fetched twice, once per physical
  row, exactly as the old `GPU_PIXEL_DOUBLE` scheme did.

## Registers

In `socctl.v` at `0x7000_02xx` -- not a new peripheral. A dedicated
slave would have cost a term in `sysctl.v`'s `wbm_dat_i` mux, which
resolves through a `32'hzzzz_zzzz` default that yosys handles badly
(measured at ~800 LUT4 the last time one was tried; see `sysctl.v`'s
own note). `socctl` was already decoded and already wired to
`gpu_video`.

| addr | name | contents |
|---|---|---|
| `0x7000_020c` | GAME | W: bit0 enable, bit1 wrap. R: `{ 0x5A47, 13'b0, avail, wrap, en }` |
| `0x7000_0210` | VIEW | bits 9:0 = x, bits 25:16 = y, in framebuffer pixels |
| `0x7000_0214` | FRAME | R/O: `{ 15'b0, vblank, frame[15:0] }` |

`0x5A47` ("ZG") is a presence signature, for the same reason the VIDEO
register has one: `socctl` shipped before these registers existed, and
on such a bitstream the read falls through `socctl`'s default case and
returns 0 -- indistinguishable from a working block reporting "game
mode off". `z_game_present()` (`sw/common/zsoc.h`) checks it.

There is **no hang hazard** here, unlike the RTC, TRNG and audio
blocks: game mode has no address window of its own, and every
bitstream with `socctl` already acks this whole window. No probe
ordering rule to obey.

## Clamp and wrap

`Z_GAME_WRAP` selects what happens at the framebuffer's edge.

**Clamp (default).** The origin is limited to x <= 320, y <= 240, so
the viewport can never hang off the edge. This is what the desktop
wants: scrolling right to find the dock and having the screen wrap
around to the left instead would be disorienting rather than useful.

**Wrap.** Column 639 is followed by column 0, row 479 by row 0. The
640x480 surface becomes a torus, i.e. an infinitely scrollable world
where only the leading edge has to be redrawn as it comes around,
rather than a bounded playfield two screens wide. This is what a
scrolling game wants and is almost certainly wrong for anything else.

The clamp is applied **in hardware at the frame boundary**, not on the
write path. That is deliberate: it depends on the wrap bit, so
clamping on write would make the stored origin depend on the order the
two registers happened to be written in. One rule, one place --
whatever is adopted is what gets scanned, and it is always in range.

The consequence to know about: `z_game_get_view_x()` reads back what
was **written**, not the clamped value in use.

Separately, `socctl` range-limits the origin to 0..639 / 0..479 on the
write path. That is a blunter guarantee and a different one: scanout
can never be pointed outside the 9600 words `vram_wb` actually has,
whatever software writes and in whatever order.

## Page flipping

The hardware has no concept of a page. The origin is an arbitrary
(x,y) pair, and 640x480 simply happens to hold four non-overlapping
320x240 regions:

```
  (0,0)      (320,0)
  (0,240)    (320,240)
```

`Z_GAME_PAGE0_X` and friends name them for convenience. A game is free
to ignore them: two half-height 640x240 buffers (two pages side by
side) give double buffering *plus* 320px of horizontal scroll room in
each, which is the right split for a horizontal scroller. Nothing in
the RTL cares.

A flip is one register write, adopted at a frame boundary, so it
cannot tear:

```c
for (;;) {
    draw_everything_into(back);          /* raster + blit, as usual */
    z_game_set_view(back_x, back_y);     /* flip */
    z_game_wait_frame();                 /* flip has now been adopted */
    swap(front, back);
}
```

`z_game_wait_frame()` polls the FRAME counter. There is deliberately no
vblank interrupt: polling costs no IRQ line, no latency budget and no
kernel involvement, and a full-screen game's main loop is already a
loop. The counter wraps every ~18 minutes at 60Hz, so compare for
inequality or unsigned-subtract for elapsed frames; do not test with
`>`.

## Sprites

Use an off-screen page as sprite storage and blit from it.

`gpu_blit.v`'s VRAM-to-VRAM copy mode (`CTRL_SRCMEM=0`) already exists
and its own header already anticipated this -- "offscreen VRAM to
on-screen VRAM, i.e. sprites, once there is spare VRAM to keep them
in". Game mode is what makes that true.

One off-screen 320x240 page is **76,800 bits, over four times an 18Kbit
BRAM block**, and costs nothing because it already exists. It is also
the faster source: VRAM-to-VRAM never touches `arbiter_main`, SDRAM
latency or the instruction cache.

This is why there is no hardware sprite engine and why the spare BRAM
block was not spent on sprite memory.

## Keyboard

Handled in `sw/apps/wm/wm.c`, which owns global hotkeys because it is
the one process that sees every keystroke before any app does.

| key | action |
|---|---|
| Alt+Esc | toggle game mode |
| Ctrl+Alt+Arrow | move the viewport 20px |
| Super held + mouse | viewport follows the pointer |

Ctrl+Alt, not plain Alt, because Alt+Arrow already moves the focused
window. `dispatch_keys()` must test the Ctrl+Alt case **first** --
Ctrl+Alt+Left also satisfies the Alt+Left test, so the other order
would move the window and the viewport would never move at all.

Pointer following is behind Super because a full-screen game may well
use the mouse, and a viewport that chased the pointer unprompted would
fight it constantly. Super is the one modifier nothing else in the
window manager binds.

Entering game mode centres the viewport rather than starting at the
origin: the dock is bottom-left and windows cascade from the top-left,
so a corner start would often show an empty region.

## Boards

`GAME` in `rtl/boards.vh`, universal and on by default, for the same
reason `RTC` and `TRNG` are: no pins, no external part, no board
support of any kind. It is not even a new block -- a loadable counter
and an adder in `gpu_video.v` plus two registers in `socctl.v`.

On a board without `GPU` it does nothing, and `sysctl.v` makes that
explicit rather than implied: it ands `GAME` with `GPU` before handing
`socctl` its `GAME_AVAIL` parameter, so the enable bit is forced low in
hardware and reads back low. A board with no scanout cannot be talked
into a scanout mode.

Prefer `z_game_available()` over testing `Z_FEATURE_GAME`. The feature
bit says `GAME` was defined; the helper asks the hardware, which also
accounts for the no-GPU case.

## A behaviour change you should know about

Building this turned up a pre-existing off-by-one on **both** axes in
the old scanout, and fixed both:

- Horizontal: `if (hc > h_disp_start) x <= hc - h_disp_start` produced
  column 0 on the first *two* visible pixels and never produced column
  639.
- Vertical: `y` was assigned at end-of-line from the current `vc` but
  took effect on the *next* line, so row 0 displayed twice and row 479
  never displayed.

The net effect on every bitstream before this one is that the picture
sits one pixel right and one row low, with the last column and last row
pushed into overscan. At 1:1 that is invisible, which is exactly why it
survived.

It does not stay invisible under 2x doubling: it becomes a
three-pixel-wide first column and three-row-tall first row against two
everywhere else -- a visible seam on two edges of a scrolling
playfield. So both are fixed.

**Desktop output therefore moves one pixel left and one row up relative
to every previous bitstream, and gains its rightmost column and bottom
row back.** Nothing in software depends on the old behaviour, but it is
a real change to what appears on the glass.

## Testing

`rtl/gpu/bench/tb_game_mode.v` is self-checking and tests addresses
rather than pixels, since the feature is an address transform: for a
given origin and mode, which framebuffer column and row does each
physical pixel come from? That is exactly the `x`/`y` output pair.

Covered: desktop 1:1 regression, the off-by-one on both axes, 2x
doubling verified exhaustively across a full line, page-aligned and
non-aligned origins, clamp limiting to (320,240), toroidal wrap walked
pixel by pixel across the seam, mid-frame writes not taking effect
until the frame boundary, and the frame counter and vblank flag.

`iverilog` rejects the trailing comma in `gpu_video.v`'s port list
(yosys accepts it, and it is the file's existing style), so point it at
a patched copy -- the same treatment `rtl/gpu/bench/README.md` already
documents for `glyph.v` and `vram.v`:

```
sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
    rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
iverilog -g2005 -o /tmp/tb_game.out \
    rtl/gpu/bench/tb_game_mode.v /tmp/gpu_video_fix.v
vvp /tmp/tb_game.out
```

It runs roughly forty full 800x525 frames, so it takes a few minutes
rather than a few seconds. Prints `RESULT: PASS` or `RESULT: FAIL`
plus the offending values. Currently passes.

## Game runtime (`sw/common/zgame.h`)

`zsoc.h`'s `z_game_*` helpers are the register interface and nothing
more. Every full-screen game then has to invent the same three things
on top: a decision about how to carve the framebuffer into buffers, a
flip that is actually tear-free, and a world-to-framebuffer coordinate
mapping. Getting any of them subtly wrong produces symptoms — a tear, a
one-frame flicker, a sprite drawn into the buffer nobody is looking at
— that are much easier to avoid than to debug. `zgame.h` is that layer.

### Two half-pages, not four quadrants

```
    rows   0..239   page 0
    rows 240..479   page 1
```

Quadrants would give four buffers of exactly viewport size; halves give
two buffers each with **320 pixels of horizontal room to spare**. For a
side-scroller that spare room is the entire point.

With wrap enabled the camera wraps at column 639, so a half-page is a
horizontal torus. A world longer than 640 pixels is drawn as a sliding
window: as the camera advances, draw only the newly exposed columns at
`(camera + 320) mod 640`. At walking pace that is two to four columns
per frame instead of redrawing 320×240 of tiles.

And the wrap costs nothing vertically. A half-page starts at y=0 or
y=240, and 240 + 239 = 479 — the camera's bottom edge lands exactly on
the last row and never crosses the wrap boundary. Toroidal horizontal
scrolling and a fixed vertical position, from one mode bit, with no
special casing.

A game wanting four buffers of exactly viewport size should use the
quadrants and ignore these helpers. Nothing here is mandatory and the
hardware has no opinion.

### The loop

```c
z_game_t g;
if (!z_game_begin(&g, true)) { /* no game mode; say so */ }

for (;;) {
    int32_t from, to;
    if (z_game_scroll_span(&g, &from, &to)) {
        draw_background_columns(from, to, z_game_back_y(&g));
        z_game_mark_drawn(&g, from, to);
    }
    draw_sprites(z_game_back_y(&g));

    uint32_t dt = z_game_flip(&g);   /* frames actually elapsed */
    update_world(dt);
}
```

`z_game_flip()` sets the camera to the back page and waits for the
frame boundary. After it returns, the page just drawn is on screen and
the other one is free. No copy, no tear — the camera cannot move
mid-frame, so a frame is always shown whole.

It returns the number of display frames that actually elapsed, which is
1 when the game is keeping up and more when it is not. Scale movement
by it so a game that drops to 30fps moves at the same speed rather than
in slow motion.

### Per-page draw tracking

The two pages are drawn on alternate frames, so each lags the other by
one frame of scroll. `z_game_scroll_span()` tracks what is drawn in
**each** page separately and returns only the columns that page is
missing. Scrolling 4px per frame, each page needs 8 new columns per
turn, not 4 — which is exactly right and is the sort of thing that is
easy to get wrong by one page.

It also handles the case where the camera jumps further than the
viewport (a level restart, a teleport): there is then no leading edge
to speak of, so it returns the whole viewport, costing one full redraw
before scrolling is cheap again.

`z_game_mark_drawn()` clamps recorded coverage to 640 columns, because
a page is a 640-wide torus and cannot hold more than that. Claiming
otherwise would mean skipping a redraw of columns the wrap has genuinely
overwritten.

### What it does not do

No sprites, no tiles, no collision, no entities. Those belong to a game,
not to a runtime, and every game wants them differently. What is here is
only the part that is genuinely about *this* hardware and that is easy
to get wrong.

## Scheme

`(game-mode)`, `(game-view)`, `(game-frame)`, `(game-wait)` — see
`docs/scheme_api.md`. Useful for poking at the viewport interactively;
`Alt+Esc` gets you back if you scroll away from your terminal.
