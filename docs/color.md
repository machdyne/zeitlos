# Colour

Game-mode colour: up to 16 colours in the 320x240 game-mode viewport,
from the unchanged 640x480x1bpp framebuffer, for one block RAM -- and
64 or 256 colours on boards built with `GAME_PLANES` 6 or 8, from a
second sheet of VRAM below the screen ([More than four
planes](#more-than-four-planes)).

The desktop stays monochrome -- except, on boards built with
`COLOR_WINDOW`, one window of it: the [colour
window](#the-colour-window), 8 or 32 colours for the focused screen
app; and on the eight-plane (45F) boards, [desktop
colour](#desktop-colour): the whole desktop in four colours, as accents
on window chrome, the dock and dialogs, in one of 21
[themes](#themes). Nothing about the 640x480 mode, the framebuffer, the blitter, the
rasterizer or any existing program changes. Colour is off at reset and
off whenever game mode is off (the colour window aside).

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
position, and a 16-entry palette turns the index into a colour (64 or
256 entries with [more planes](#more-than-four-planes)).

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

## More than four planes

A board built with `GAME_PLANES 6` or `GAME_PLANES 8` (`rtl/boards.vh`,
with `COLOR`) shows up to six or eight planes: 64 or 256 colours
through a palette of 64 or 256 entries. A board without it has four,
as before, and software asks `z_color_planes_max()`.

```
  sheet 0: VRAM words 0..9599          sheet 1: words 9600 up
  (framebuffer rows 0..479)            (rows 480..959, or 480..719)
  +-----------+-----------+            +-----------+-----------+
  |  plane 0  |  plane 1  |            |  plane 4  |  plane 5  |
  +-----------+-----------+            +-----------+-----------+
  |  plane 2  |  plane 3  |            |  plane 6  |  plane 7  |   (8 only)
  +-----------+-----------+            +-----------+-----------+
```

- **Planes 4..7 are planes 0..3 of a second sheet.** VRAM grows from
  one 640x480 sheet to one and a half (6 planes, 14,400 words) or two
  (8 planes, 19,200 words). Plane 4+j sits in the second sheet exactly
  where plane j sits in the first, 480 rows further on, with plane j's
  offsets: no new offset bits, and every layout above works in both
  sheets at once.
- **Six planes: the second sheet is 240 rows**, and plane 4/5's row is
  taken mod 240. So a 6-plane layout keeps planes 0 and 1 apart by dx,
  as `z_color_16` (and `z_color_64`) do; planes 4 and 5 then sit side by
  side. A plane 1 at dy would put plane 5 on top of plane 4.
- **The desktop never sees it.** Rows 480 up are below the screen and
  outside DIRTY (the screen streamer); only game-mode colour reads them.
- **Drawing is unchanged.** The blitter and rasterizer address rows up
  to 959 already (17-bit VRAM byte addresses, an 11-bit scissor).
  zcolor.h draws there through the same `zgfx` calls, which a game lets
  past row 479 with `z_fb_set_rows()` -- `z_color_begin()` does it.
- **Cost:** +12 DP16KD (6 planes) or +20 (8) of VRAM, and on the 25F
  boards about the same number of LUTs as before (below). The palette
  moved into the cursor's block RAM on every colour board, so a bigger
  one costs no block of its own. See [Cost](#cost).

| `GAME_PLANES` | Colours | VRAM words | VRAM DP16KD | Boards |
|---|---|---|---|---|
| (none) | up to 16 | 9,600 | 20, and a read mux (about 220 LUT4) | 12F, ULX3S, 7-series |
| 6 | up to 64 | 14,400 | 32, no read mux | Lakritz, Mozart ML0, Sergei ML0 (25F) |
| 8 | up to 256 | 19,200 | 40, and the same mux as 9,600 | Mozart ML1/ML2, Sergei ML1/ML2, Noir, Schoko (45F) |

DP16KD here are true dual port (the CPU and the scanout each have a
port), so at most 18 bits wide. Yosys (0.70, and 0.33 the same) builds
9,600 words from 20 blocks several deep, with a read multiplexer on each
port. For 14,400 it uses the 16,384 x 1 shape, 32 blocks side by side
and no multiplexer at all: two blocks more than the tightest packing
(15 pairs of 1,024 x 18 would be 30) and about 210 LUT4 fewer than the
9,600-word VRAM it replaces. The 25F boards are as short of LUTs as of
block RAM (Lakritz runs at 96–98% of its LUTs), so that is the better
trade. Packings of 30 were built and measured -- fifteen 1,024-word
banks, and a 14,336 + 64 split -- and yosys gives both 15:1 read
multiplexers, 1,000 LUT4 and more. 19,200 is 40 blocks with the
multiplexer again; splitting it at 16,384 to reach 38 came out larger
too.

### One game, any number of planes

The same game should run on every board: in 256 colours where there
are eight planes, in 64 or 16 where there are fewer, as fast either
way. The hardware side is above; the rest is how art is stored and
loaded -- sprite sheets ([zsp.md](zsp.md)), drawn in pix
([pix_app.md](pix_app.md)), and the arcade runtime's 64 and 256
colours ([arcade.md](arcade.md)):

- Art keeps up to 8 bits a pixel and a 256-entry palette whose first 16
  (and first 64) entries are the smaller palettes, plus, for each
  entry, which entry stands in for it at 64, 16, 4 and 2 colours.
- A game asks `z_color_planes_max()` and loads its art reduced to
  that once, at load time, into that many planes. Drawing is then the
  same code over fewer planes, so a 16-colour board draws exactly as
  fast as it does today.

For a program that does not reduce its art, `zcolor.h` draws as many of a
layout's planes as the machine shows and drops the rest: a 256-colour picture on a 16-colour
machine shows its low four planes (the power-on palette is built for
that, below).

## The colour window

On a board built with `COLOR_WINDOW` (with `GAME_PLANES` 6 or 8), the
desktop -- still 640x480, one bit a pixel -- can show **one rectangle in
colour**: the focused screen app's window ([app_kinds.md](app_kinds.md)),
up to 320x240. Its colours come from the second VRAM sheet, which only
game mode reads otherwise:

| `GAME_PLANES` | The window's planes | Colours | Palette entries |
|---|---|---|---|
| 6 | 2 | 8 | 0, 1, 16, 17, 32, 33, 48, 49 |
| 8 | 4 | 32 | 0, 1, 16, 17, ... 240, 241 |

A pixel's index is the **desktop's own bit** (bit 0) and the window's
planes (bits 4..7): the desktop pixel is part of the colour, so what
the window shows with the colour off is still a picture. Planes 1..3
are the desktop's other quarters and take no part.

### Where the window's planes are

At the pixel's own screen position, folded into the second sheet --
which is exactly how game mode reads planes 4..7 with the 16-colour
layout's offsets and the view at (0, 0), so the scanout's fetch and
read side needed no change:

| Window plane (index bit) | Desktop pixel (x, y) is kept at |
|---|---|
| 0 (bit 4) | sheet 1, (x, y) |
| 1 (bit 5) | sheet 1, (x + 320, y) |
| 2 (bit 6) | sheet 1, (x, y + 240) -- eight planes only |
| 3 (bit 7) | sheet 1, (x + 320, y + 240) -- eight planes only |

Columns are taken mod 640 and rows mod the sheet's height (240 with six
planes, 480 with eight); sheet 1's row r is framebuffer row 480 + r.
`z_color_window_plane_xy()` works it out. A window up to 320x240 has a
place of its own for every pixel in every plane, **wherever it sits on
the desktop** -- no alignment. When it moves, its planes are drawn
again at the new position (or copied there by the blitter).

### Rules

- **Desktop mode only.** The scanout ignores the window in game mode,
  and **entering game mode turns it off** (socctl clears EN on a GAME
  write with bit 0 set): a full-screen game takes the second sheet and
  the palette for its own. Whoever owns the window sets it again, with
  its planes and palette redrawn, afterwards.
- **One window at a time**, and at most 320x240: the fold tells no more
  pixels apart. `z_color_window_set()` clips to that.
- **Only while wholly uncovered.** The hardware knows nothing of
  windows: anything drawn over the rectangle is shown through the
  window's palette. So the window is set only while nothing covers it,
  and turned off before anything does (before the SET_CLIP ack that
  lets wm draw over it).
- **The palette is shared** with game mode. Entries 0 and 1 are the
  desktop's black and white for the window's own pixels with bit 0
  only; set them to the desktop's ink (0x888 is the DDMI white at the
  default brightness) so the window's edge matches the desktop.
- **The cursor** inverts every plane in use inside the window, as in
  game-mode colour.
- VGA shows the top bit of each channel (8 colours at most); there is
  no window on composite (its viewport is permanent).

### Who has it

**The focused screen app.** A program on the arcade runtime that asks
(`ar_window_colour(true)` before `ar_init()`: snake, blocks, bricks, pix)
creates its window with `Z_WIN_FLAG_COLOR` -- only on a board with the
colour window. Then:

- **wm decides who.** On a focus change wm clears COLWIN and tells the
  window losing focus `Z_WM_COLOR` 0 and the window gaining it 1 (only
  windows with the flag hear either). The lock screen, the rescue
  console and the reboot box count as no focus. And the moment the
  window with the colour has its region narrowed or frozen -- a window
  dragged or raised over it, a drag of its own -- wm clears COLWIN
  before drawing anything over it.
- **The window decides when.** Told 1, the runtime takes the colour at
  a frame while its window is wholly uncovered, the desktop is showing
  (not game mode) and it is not frozen: COLWIN set to its content, the
  picture drawn again in colour. Covered, or told 0, it draws in black
  and white again (and clears COLWIN itself when it set it). It sets it
  again when the window has moved, and when the hardware dropped it --
  a full-screen game cleared it, and took the planes and the palette
  meanwhile -- drawing everything again.
- **F10 -- or the colour titlebar icon, a rainbow -- turns it off** in that program, and on again
  ([keys.md](keys.md)): off, the window draws in black and white and
  gives COLWIN back as when covered, whatever wm says, until F10 again.
  Full screen, F10 is game mode's colour key (as C).

That split is for wm's budgets: wm's part is about 300 bytes (paid
for in [dock.md](dock.md)'s account of wm's size), the decision about
the window's region is the runtime's, which knows it anyway.

So with two screen apps on the desktop, the focused one is in colour
and the other in black and white; click the other and the colour moves.
A full-screen game takes the planes; back on the desktop the focused
screen app has them again.

`colwin`, below, sets COLWIN itself, without the flag: a test of the
hardware.

### Registers

| Address | Name | Contents |
|---|---|---|
| `0x7000_0240` | COLWIN | W: bits 9:0 x0, bits 25:16 y0, bit 30 DESK, bit 31 EN (lanes 0/1 x0, 2/3 y0, DESK and EN). R: `{ 0x5A57 ("ZW"), avail, davail, 12'b0, desk, en }`. Flips the load toggle: adopted at the next frame boundary with GAME, VIEW and COLOR. DESK: [desktop colour's exact path](#the-exact-path-color_desktop) |
| `0x7000_0244` | COLWINEND | W: bits 9:0 x1, bits 25:16 y1, exclusive. R: `{ 6'b0, y1, 6'b0, x1 }`. Does not flip the toggle: written first, adopted with the next COLWIN |

The window is x0 <= x < x1, y0 <= y < y1. EN is forced low, and avail
reads 0, without `COLOR_WINDOW`; DESK likewise, and davail, without
`COLOR_DESKTOP`. socctl's decode is five bits wide for
these two; on a four-bit bitstream words 16 and 17 alias CTRL and
MAGIC, so software reads COLWIN's signature before it writes either
(CTRL reads back with bit 31 set, never `0x5A57`).

### Software (`sw/common/zsoc.h`)

| Function | Does |
|---|---|
| `z_color_window_available()` | this bitstream has the colour window |
| `z_color_window_planes()` | the window's planes: 2 or 4 (0 without); colours `2 << planes` |
| `z_color_window_entry(c)` | the palette entry colour c shows through: `(c & 1) \| (c >> 1) << 4` |
| `z_color_window_plane_xy(k, x, y, &px, &py)` | where window plane k of desktop pixel (x, y) is kept |
| `z_color_window_set(x0, y0, x1, y1)` | on, over that rectangle, from the next frame (clipped to the screen and 320x240) |
| `z_color_window_off()`, `z_color_window_on()` | off from the next frame; read back |

Drawing the window's planes is drawing past row 479 with the visible
region cleared: `z_gfx_clear_visible()`, `z_fb_set_rows(z_color_rows())`,
blitter fills and blits, then `z_fb_set_rows(480)`; the next `z_win_*`
call puts the window's region back.

**`colwin`** (`sw/apps/colwin`, in the dev pack) is the test: a 320x240
window of 8 or 32 numbered swatches, the palette's window entries in
dark (bit 0 clear) and light (bit 0 set) pairs, so it still reads in
black and white with the colour off. It sets the window while it is
wholly uncovered and draws its planes again after every move. Esc or
the close box ends it with the colour off.

### How it works

In `gpu_video.v` (`WIN_AVAIL`), all in the pixel domain:

- the rectangle, captured with the GAME/VIEW/COLOR payload and adopted
  at the frame boundary; not in game mode;
- a registered test of x and y against it (`win_px`), on the same edge
  as the palette's read of the same pixel;
- in the window, the 16-colour layout's offsets in place of COLOR's and
  a plane mask of bit 0 and the second sheet's planes;
- the outputs (VGA, DDMI) choosing the palette or the phosphor's ink
  per pixel instead of per frame;
- the palette's read is registered, so while the window is on x runs a
  clock ahead of the raster (`x_lead`, as for game-mode colour), and
  the monochrome pixel outside the window is taken from a register to
  meet it. Inside and outside line up to the pixel; the cursor too.

### Cost

`gpu_video` synthesised on its own (yosys `synth_ecp5`, DDMI, colour,
the cursor, eight planes), `WIN_AVAIL` 1 against 0: **+156 LUT4, +20
CCU2C, +85 flip-flops, no block RAM**. `socctl`: +22 LUT4, +41
flip-flops. No more VRAM reads: every plane is already fetched every
line, in desktop mode too. Pre-placement numbers, and noisy as the ones
under [Cost](#cost) are; not yet placed or timed. On by default only
on the 45F boards (`GAME_PLANES 8`: Mozart ML1/ML2, Sergei ML1/ML2,
Noir, Schoko); the 25F boards are as full as they are, and may get it
if it fits.

### Testing

`rtl/gpu/bench/tb_colwin.v` (`-DNPLANES=6` or `8`, and with
`-DGPU_DDMI -DTB_STUBS` the TMDS inputs): every visible pixel of a whole
frame per case, inside the window the index and its colour, outside the
desktop's own pixel -- a 320x240 window at an odd position, one across
the fold to the corner, the lower-right quarter, the cursor, paper mode
outside, the window off (x not leading), game mode with the window
still asked for (ignored), and back. `rtl/tests/tb_socctl_colwin.v`:
the registers -- staging, the toggle, lanes, read-back, game mode
clearing EN, and a build without the window.

## Desktop colour

On a board with the colour window and **eight** planes (the 45F boards:
Mozart ML1/ML2, Sergei ML1/ML2, Noir, Schoko), wm shows the whole
desktop in four colours. No new hardware: it is the colour window
stretched over the screen.

- **COLWIN over 0,0..640,480** (`z_color_desktop_on()`, zsoc.h; the
  hardware does not limit the rectangle -- only `z_color_window_set()`
  cuts it to 320x240). With eight planes the second sheet is 640x480, so
  window plane 0 is every desktop pixel's own **accent bit**, at sheet 1
  (x, y), unfolded.
- **The palette makes bits 5..7 irrelevant.** Those read sheet 1 320
  columns and 240 rows on -- other pixels' accent bits -- so every one of
  the 256 entries is set from its bits 0 and 4 alone
  (`sw/common/zaccent.c`):

| Entry | Bit 4 | Bit 0 | Colour |
|---|---|---|---|
| 0 | 0 | 0 | black (the phosphor's background) |
| 1 | 0 | 1 | ink (the phosphor) |
| 16 | 1 | 0 | A0, tinted black -- dark |
| 17 | 1 | 1 | A1, tinted ink -- light |

An accent bit **tints** a pixel rather than adding a colour: black
shows as A0, ink as A1, so tinted text is still text. The colours follow
the phosphor and the brightness (`system.video.mode`, Super+P,
Ctrl+Super+Minus/Equal: wm rereads the VIDEO register every pass):

| Phosphor | black | ink | A0 | A1 |
|---|---|---|---|---|
| white -- slate and sand | 000 | 888 | 123 | CA8 |
| amber -- umber and gold | 000 | C90 | 210 | EC6 |
| green -- pine and mint | 000 | 080 | 021 | 3B5 |
| paper -- blue-grey and navy | 888 | 000 | 9AB | 235 |

(at the default brightness; the accents scale with it the same way.)
Two rules hold at every level (`sw/common/tests/test_accent.c`): A0 is
dark and A1 light, so a pointer that inverts the accent bit too (the
palette-only path, below) still lands on a contrasting colour
(black/A1, ink/A0); and on a board with a 1-bit VGA output too
(Schoko), every channel stays on its side of 8, so there the accents
fold onto black and the ink and the desktop looks as it always has.

### Themes

The colours are a matter of taste, so there are 21 built-in themes
(`z_accent_themes`, `sw/common/zaccent.c`) and room for any others in
`zeitlos.cfg`. **settings** steps through them with its *Desktop
colours* `<` `>` buttons (Off, then the themes), saving each at once;
wm picks a change up within a second.

There are two kinds:

- **Accent themes** change only A0 and A1, the dark and the light tint.
  Black and the ink stay the display colour's own -- exact on gateware
  with DESK (below) -- and follow the phosphor and the brightness. On
  paper the pair swaps, so the paper takes the light tint and the ink
  the dark one.
- **Full themes** bring all four colours and do not consult the
  phosphor. Black and the ink then have to go through the palette, so
  wm leaves DESK off: the colours are the theme's own, the pointer's XOR
  tints (black/A1, ink/A0), and each full theme keeps those pairs apart
  in brightness (`sw/common/tests/test_accent.c` checks every theme at
  every level). They dim with the brightness below the default but do
  not go brighter.

| Theme (`desktop_theme`) | Name in settings | Kind | Colours (black, ink, A0, A1 at level 7) |
|---|---|---|---|
| `phosphor` (default) | Phosphor (follows the display colour) | accents | each display colour's own pair (above) |
| `slate` | Slate and sand | accents | the phosphor's, 123, CA8 |
| `umber` | Umber and gold | accents | the phosphor's, 210, EC6 |
| `pine` | Pine and mint | accents | the phosphor's, 021, 3B5 |
| `navy` | Navy and ice | accents | the phosphor's, 013, 9CE |
| `plum` | Plum and rose | accents | the phosphor's, 302, E9B |
| `oxblood` | Oxblood and cream | accents | the phosphor's, 300, EDB |
| `teal` | Teal and coral | accents | the phosphor's, 033, F87 |
| `graphite` | Graphite and silver | accents | the phosphor's, 222, AAA |
| `midnight` | Midnight and amber | accents | the phosphor's, 012, FB4 |
| `aubergine` | Aubergine and lilac | accents | the phosphor's, 213, BAE |
| `ocean` | Ocean and seafoam | accents | the phosphor's, 024, 7DC |
| `cocoa` | Cocoa and peach | accents | the phosphor's, 321, FCA |
| `moss` | Charcoal and lime | accents | the phosphor's, 121, BE5 |
| `solarized-dark` | Solarized dark | full | 023, 899, 034, BA0 |
| `solarized-light` | Solarized light | full | FFE, 678, EEC, 28D |
| `nord` | Nord | full | 223, DDE, 445, 8BC |
| `gruvbox` | Gruvbox | full | 222, EDB, 332, FB3 |
| `dracula` | Dracula | full | 223, EEE, 446, B9F |
| `workbench` | Workbench (blue, orange) | full | 05A, FFF, 037, F80 |
| `cga` | CGA (cyan, magenta) | full | 000, FFF, 0AA, F5F |

**Custom colours**, over any theme (`docs/config.md`):

- `system.video.desktop_accents: 123,CA8` -- A0 and A1 (dark, light), in
  RGB444 hex (`#` optional). Keeps the phosphor's exact black and ink.
- `system.video.desktop_base: 223,DDE` -- black and the ink. Makes it a
  full theme: the palette shows all four colours, and DESK is off.

Choosing a theme in settings removes both lines, so what was chosen is
what shows. What cannot be changed: there are four colours, and which
parts of the desktop are tinted (the background can be left out with
`system.video.desktop_background: no`). On a board with a 1-bit VGA
output (Schoko), each accent channel is kept on the side of 8 of the
colour it tints, so VGA keeps showing two colours whatever the theme.

### The exact path (`COLOR_DESKTOP`)

On the 45F boards the gateware has one more bit for this: COLWIN's
**DESK** (bit 30; `z_color_desktop_exact()`, and `z_color_desktop_on()`
sets it). With DESK, inside the window:

| Pixel | Shown as |
|---|---|
| accent bit set, the pointer not on it | the palette, index `0x10 | desktop bit` -- A0 or A1 |
| anything else | the **monochrome pixel**: the phosphor's ink at its brightness, paper's inversion, the pointer's XOR |

So the black and the ink are bit for bit what a monochrome desktop
shows (entries 0 and 1 are never read), nothing shifts when desktop
colour turns on or off or a colour window takes the sheet, and the
pointer is the plain monochrome pointer everywhere -- ink over A0, black
over A1. The index is masked to bits 0 and 4.

It costs one flop of state, one register and a gate into the palette
select in the pixel domain (`desk_active`, `desk_px` in
`rtl/gpu/gpu_video.v`), and a bit in socctl. It needs eight planes: on
a 25F build (`GAME_PLANES 6`) it is a constant 0 and none of it is
built, and `COLOR_DESKTOP` is defined only on the 45F boards. Without it
(older gateware) wm's desktop colour works the same through the palette
alone: the black and the ink rounded to four bits a channel (the
default white 0x88 rather than 0x80), and the pointer's XOR flipping
the accent bit too.

**More colours, one day.** The rule generalises: a pixel with any
per-pixel accent plane set goes through the palette, the rest through
the ink. Each further VRAM sheet holding one more plane at the pixel's
own position would add a bit to the mask (`DESK_MASK`) and double the
desktop's colours, up to 256 with eight per-pixel planes -- 300 KB of
VRAM, which no board has. With today's two sheets there is one accent
bit, and four colours.

**What is tinted** (wm, `acc_*` in `sw/apps/wm/wm.c`):

- the desktop's background (`system.video.desktop_background`);
- the focused window's titlebar, separator and frame -- the bar shows
  A1 with A0 text; the one-pixel margin inside the frame never;
- the dock's outline, its running dots, a launching slot and the
  selection ring;
- wm's lock screen and reboot box;
- inside windows, through the widgets: the keyboard-focused button (a
  dialog's default) and a list's selected row (`zwidget.c`, `zflist.c`,
  `z_win_accent_rect()`, zwin.h).

Unfocused windows, icons and everything an app draws itself stay as
they are. wm writes each tint in place, so nothing blinks; it clears a
window's content tints wherever it asks the window to repaint, and the
window draws its own again.

**Who has the second sheet:**

| | |
|---|---|
| desktop colour | wm, while it is on (`system.video.desktop_colour`, Super+F10) and nothing below wants the sheet |
| a colour window | the focused screen app (`Z_WIN_FLAG_COLOR`) -- **it takes precedence**. wm turns COLWIN off, tells every app it may not tint (the WINDOW rectangle of its region, `Z_WM_CLIP_ACCENT`, zwm.h), and hands the colour window over (`Z_WM_COLOR` 1) once they have acked. A dual app on its desktop face does not want it (`Z_WIN_FLAG_COLOUR_LATER`) until it attaches (`Z_WM_COLOR_WANT`), and a screen app whose colour is off (F10) gives it up the same way. `learn` wants it the same way while a lesson's figure shows whole, and covers only the figure ([learn_figures.md](learn_figures.md#colour)) |
| game mode | the game: the GAME write clears EN, and wm keeps off while a grab is held |

Taking it back, `ACC_RETAKE_MS` after the colour window loses the focus
(the old owner's last plane writes): the whole sheet cleared, the tints
painted, the 256 entries written, COLWIN over the screen, and the apps
allowed to tint again -- those that had (`Z_WM_ACCENT_USED`) are asked to
repaint. About 1-2 ms; a frame or two without tints.

**Cost:** no block RAM or VRAM; the exact path's few gates (above), on
the 45F boards only. wm: about 5.6 KB of code;
apps with widgets about 0.8 KB (`z_win_accent_rect()` and its calls).
Monochrome boards, and the 6-plane ones (their second sheet is 240
rows, so two rows would share an accent bit): `z_color_desktop_available()`
is false and nothing is drawn. Screenshots and the screen streamer read
sheet 0 only, so they show the desktop without tints.

Schoko's VGA has one resistor per channel (its schematic and PCB, in
github.com/machdyne/schoko), so the accents fold away there as above;
DDMI shows them.

`sw/apps/wm/tests/test_accent.c` runs the real wm.c on the build host:
nothing on a monochrome board; COLWIN, the tints and the apps' flag on
an eight-plane one; content tints cleared on a repaint request; focus
moving the tint; the background option; the hand-over to a colour
window and back; a dual app; off and a game's grab; DESK on exact
gateware. `rtl/gpu/bench/tb_deskcol.v` checks the exact path pixel by
pixel (palette for accent pixels, the exact ink for the rest, at two
phosphors and levels, paper, the pointer, a smaller window, DESK off,
game mode), and that a build without it, or with six planes, ignores
DESK.

## Registers

In `socctl` (`rtl/socctl.v`), alongside GAME and VIEW
([game_mode.md](game_mode.md), [socctl.md](socctl.md)).

| Address | Name | Contents |
|---|---|---|
| `0x7000_0224` | COLOR | W: bit 0 EN, bits 3:1 planes−1, bits 9:8 / 11:10 / 13:12 plane 1/2/3 `{dy,dx}`. R: `{ 0x5A50, avail, 0, offsets, 00, xp, np, en }` |
| `0x7000_0234` | PALETTE | W only, whole-word store: bits 31:24 entry, bits 11:0 RGB444. Reads 0 |

- **XP** (bits 5:4, read-only) is the most planes the bitstream
  shows: 0 = 4, 1 = 6, 2 = 8. An older bitstream reads 0 there.
- **NP** is three bits now. A count above XP's is stored as the
  maximum, and reads back so. A bitstream from before `GAME_PLANES`
  ignores bit 3, so software asks for at most `z_color_planes_max()`.
- **PALETTE's entry** is eight bits; those above the palette's size
  (16, 64 or 256 entries) are ignored. Older bitstreams decode 27:24.

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
free and read as zero. (Since taken: 8 and 15 by the cursor, 10..12 by
DDMI audio, 14 by CVBS; and the decode is five bits wide for the colour
window's 16 and 17.)

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
| `z_color_set(on, planes, offsets)` | enable, plane count 1..8, plane 1..3 offsets |
| `z_color_enabled()`, `z_color_planes()` | read back |
| `z_color_planes_max()` | 4, 6 or 8 (0 without colour) |
| `z_color_palette_size()` | 16, 64 or 256 |
| `z_color_rows()` | framebuffer rows there is VRAM for: 480, 720 or 960 |
| `z_color_set_palette(index, rgb444)` | one palette entry, index 0..255 |

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
| `z_color_32` | 5: `z_color_16` + plane 4 | (0,0) | 32 colours, 6- or 8-plane boards |
| `z_color_64` | 6: + planes 4, 5 side by side | (0,0) | 64 colours, the most a 6-plane board shows |
| `z_color_256` | 8: `z_color_16` in both sheets | (0,0) | 256 colours, 8-plane boards |

A layout with more planes than the machine shows draws only the
planes it has (`z_color_hw_planes()`; `z_color_assume_planes(n)`
pretends to have n, for host tests).

| Function | Does |
|---|---|
| `z_color_begin(&layout)`, `z_color_end()` | colour on with a layout, or off |
| `z_color_palette_load(pal, n)` | n entries, after the next frame boundary |
| `z_color_palette_load_now(pal, n)` | the same, for a caller that has just waited |
| `z_color_plane_xy(&l, k, x, y, &px, &py)` | where plane k of (x,y) is (planes 4..7 in rows 480 up) |
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
origins, every drawing call, and nothing outside the rectangle touched,
on 4-, 6- and 8-plane machines (the 256-colour layout on a 6-plane one
too). It fails when the seam split is broken, and when the second
sheet's height is.

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

**mausrun** ([mausrun.md](mausrun.md#colour)). Four colours,
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

**snake** ([snake_app.md](snake_app.md)), through the arcade runtime
([arcade.md](arcade.md)). Sixteen colours, single-buffered: the picture
is painted by dirty rectangles and everything that moves is an overlay,
so no back buffer is needed. The snakes' bodies are four palette
entries each, recomputed every frame, so a rainbow flows down the snake
with no cell redrawn; the food breathes, the bonus glints, sparks
flicker, and a crash flashes the walls and shakes the viewport. C
toggles colour.

**blocks** ([blocks_app.md](blocks_app.md)), on the same runtime.
Sixteen colours as seven shapes plus a light edge each: a shape is
planes 0-2, its edge adds plane 3, which leaves black and one theme
colour (8) that follows the level and breathes with the music. When rows
clear, the stack above falls by VRAM-to-VRAM copies of every plane, not
by redrawing. C toggles colour.

**bricks** ([bricks_app.md](bricks_app.md)), on the same runtime. Six
brick colours, silver and gold, the paddle and the walls; the effects
are all palette writes -- the gold glints, the paddle's ends flash on a
hit, bolts and trails breathe, the backdrop takes each round's colour,
pulses with the music and flashes red or white, and on the title the
brick colours rotate through the letters. C toggles colour.

**Scheme** ([scheme_api.md](scheme_api.md)). `(color-mode)`,
`(palette ...)` and `(color-fill ...)`, for trying it out from a REPL.

## The palette

Sixteen entries of RGB444 -- 64 or 256 with `GAME_PLANES` -- written
through PALETTE. Power-on contents are the EGA/CGA 16 colours:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| 0 `000` black | 1 `00a` blue | 2 `0a0` green | 3 `0aa` cyan | 4 `a00` red | 5 `a0a` magenta | 6 `a50` brown | 7 `aaa` light grey |
| 8 `555` dark grey | 9 `55f` light blue | 10 `5f5` light green | 11 `5ff` light cyan | 12 `f55` light red | 13 `f5f` light magenta | 14 `ff5` yellow | 15 `fff` white |

With more entries, entry *i* powers on as EGA colour *i* mod 16, so a
64- or 256-colour picture shows as its low four planes until a game
sets its own palette.

Reset does not reload these: the palette is block RAM, which has no
reset. A game sets the palette it wants.

A palette write takes effect immediately, not at a frame boundary. A
write that lands mid-picture can show as one wrong pixel. Write the
palette in vertical blanking — straight after `z_game_wait_frame()`.
Rewriting the palette every frame is cheap, so fades and colour cycling
are a handful of stores per frame.

### Per output

| Output | What the palette becomes |
|---|---|
| DDMI (HDMI/DVI) | each 4-bit channel doubled to 8 bits: any 16 (64, 256) of 4096 colours |
| VGA | the **top bit** of each channel: at most the 8 RGB primaries and secondaries; the top **three** on a board with a 3-bit DAC (`GPU_VGA_DAC3`: Obst, Minze) |
| Composite | NTSC or PAL colour from a synthesised subcarrier; black and white available as a setting ([composite.md](composite.md#colour)) |

On DDMI the palette spans the full range. The monochrome inks keep
their historical 0x80 ceiling for compatibility; a game wanting the
desktop's exact white sets `0x888`.

Most VGA boards in the tree have one resistor per channel, so VGA
colour is 8 colours whatever the palette says. The EGA default degrades
sensibly: its 0xa levels read as on and its 0x5 levels as off.

Obst (270R, 536R, 1K) and Minze (536R, 1K, 2K2) have a 3-bit resistor
DAC per channel, driven with `GPU_VGA_DAC3`: VGA_R/G/B stay the top bit
and VGA_R1/R2, G1/G2, B1/B2 are the two below (`boards/obst_v0.lpf`,
`boards/minze_v1.lpf`). Neither has `COLOR`, so on both it is the ink
that benefits. There the palette shows its
top three bits a channel (512 colours) and, more usefully on a board
without `COLOR`, the monochrome ink shows the phosphor's colour and
brightness as DDMI does: amber is amber rather than red and green, and
the brightness levels show in eight steps. The default white, 0x80, is
the top resistor alone -- what a 1-bit build drives on the same board.

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
twice the monochrome cost, 16 colours four times, 256 colours eight.

To find plane *k* of a point (x,y) in viewport coordinates: start from
the viewport origin, add 320 to x if plane *k* has dx, add 240 to y if
it has dy, wrap modulo 640x480. For plane 4..7, take plane 0..3's
point and add 480 rows -- after taking the row mod 240 on a 6-plane
board.

## How it works

All in `rtl/gpu/gpu_video.v`, behind its `COLOR_AVAIL` parameter.

### Plane 0

The framebuffer row every mode uses, refilled once per physical line
into row 0 of the line buffer below and read exactly like planes 1..7:
a held word `cur0`, indexed by the loadable column counter `x`. (It
used to be `hline`, a 640-bit register read through a 640:1 mux; see
"The line buffer in block RAM" below.)

### The plane fetch (48 MHz, `clk`)

After the row's 20 words, the same refill fetches three more rows of 20
words — planes 1, 2 and 3 for the same line, and 4..7 with `GAME_PLANES`
— into the same line buffer.
It uses the VRAM graphics port, which is dedicated to scanout and
otherwise idle in blanking, so it costs the CPU, blitter and rasterizer
nothing.

- **Timing.** 60 words at 48 MHz is 1.25 µs. With the row refill
  and the clock crossing, it finishes about 45 pixel clocks into a
  160-clock horizontal blanking interval; eight planes (140 words)
  about 90 in. Nothing reads the buffer until 24 clocks before the line
  starts (136).
- **Rows.** Every plane's row is either the viewport's row or that row
  plus 240 (mod 480). The second is computed in the pixel domain,
  already multiplied by 20, and crosses with the same toggle and margin
  the plane 0 row has always used. The second sheet's two rows (or one,
  mod 240, at six planes) are worked out in the pixel domain too, one
  clock later, already as VRAM word addresses. Every plane's base is
  latched when the row refill ends, and the 48 MHz side adds a word
  index to the current one; the next plane's base is chosen between
  registers, off that path.
- **Whole rows, natural order.** The horizontal offset is not applied
  here. Whole rows are fetched and the read side picks the word.
- **Every plane, always.** The port is idle anyway, and skipping words
  would cost a comparison in the clock domain with no slack.

The buffer is up to 8 x 20 words, row *p* plane *p*: 256 words of one
DP16KD on ECP5, one RAMB18 on 7-series, at any plane count.

### The read side (25.2 MHz, `pclk`)

Each plane holds its current 32-bit word (`cur0..cur7`) and the next one
(`nxt0..nxt7`). The pixel's bit is `cur[x[4:0]]`.

That one index serves every plane because a 320-column offset is
exactly ten words. All planes cross word boundaries on the same pixel,
and dx becomes "read word (w+10) mod 20" rather than an arithmetic
shift.

- **At a word crossing**, `cur` takes `nxt`, and a pass of planes + 3
  clocks (seven at four planes, eleven at eight) refills `nxt` from the
  buffer, one registered read per plane. The next crossing is at least
  64 clocks away in game mode, 32 on the desktop.
- **At the start of a line**, a preload pass loads the viewport's first
  word, moves it to `cur`, and loads the word after it. `x` already
  holds the origin through blanking, so a non-word-aligned origin is
  just a different `x[4:0]`.

### Index, palette, output

The colour index is `{p7..p0}` masked to the active planes and XORed
with the cursor. It addresses the palette: words 256..511 of the
cursor's block RAM ([cursor.md](cursor.md)), written through its write
port in the wishbone domain, read through its registered read port
here. The cursor reads its row through the same port on one clock a
line, in horizontal blanking, and holds it in a register.

The read is registered, so a pixel's colour arrives one clock after its
index. On VGA and DDMI that clock is taken back by starting `x` one
clock early while colour is on: the index runs a clock ahead and the
palette lands on the raster, so blanking, sync and the picture keep
their timing. Colour is only on in game mode, where a source pixel
lasts two clocks, so the early clock is the first half of the first
pixel. On composite the read is the first of the two stages that path
already had. Blanking is forced black.

The 16-entry palette used to be distributed RAM read through a 16:1
mux in the pixel path; at 64 or 256 entries that would have been
hundreds of LUTs. In the block RAM, a palette of any size costs neither
LUTs nor a block, and the pixel path ends in a register.

### Where the logic is

Almost all of it is in the 25.2 MHz pixel domain, deliberately: that
domain closed at about 62 MHz on Lakritz against 25.2 needed, while the
48 MHz domain was at about 54 against 48. With colour, Lakritz measures
51 MHz on the pixel clock and 52.8 on `CLK_48`: the pixel domain took
the cost, as intended, and still has twice what it needs. The 48 MHz side gains only the
fetch sequencer, a 3:1 mux of latched bases and one adder.

### Without `COLOR`

`COLOR_AVAIL` is 0, `color_on` is a constant 0 and the plane fetch never
starts. Yosys removes the plane words and the palette, and the output
muxes fold back to the monochrome path. The block RAM stays: plane 0
lives in it.

### The line buffer in block RAM

Plane 0 used to be `hline`, a 640-bit register shifted in from the
48 MHz side and read as `hline[x]` in the pixel domain. That was 640
flip-flops, a 640:1 multiplexer that was the pixel clock's critical
path, a 640-bit clock-domain crossing, and a mapping that ABC handled
differently on every build (see the note under "Cost"). Putting the row
in the block RAM the planes already used, read a word at a time like
them, replaced all of that with one more 32-bit word and one more read
per pass. Measured on `gpu_video` alone (ECP5, yosys `synth_ecp5 -abc9`,
nextpnr out of context, no DDMI):

| | Before | After |
|---|---|---|
| With colour: LUTs (TRELLIS_COMB) / FFs | 1291 / 1135 | 795 / 560 |
| Without colour: LUTs / FFs / DP16KD | 989 / 804 / 0 | 519 / 254 / 1 |

A board without `COLOR` now spends one DP16KD on it.

#### Room left in it (not used yet)

The DP16KD is 512 x 36; the buffer uses 80 of its 512 words (four rows of
20, at a stride of 32), so about 384 words, 12 Kbit, are free. Its write
port (48 MHz) is busy only during horizontal blanking and its read port
(pixel clock) only for a few clocks every 32 or 64 pixels. Options
noted for later, none implemented:

- **A cursor sprite.** Done, though not in this buffer: the pointer is
  now drawn from four loadable 16x16 shapes in a block RAM of its own
  ([cursor.md](cursor.md)). Sharing this one would have needed its
  write port for the CPU as well (a 32-bit multiplexer in front of it)
  or its read port to write (true dual port, two blocks at this width),
  and a block RAM is what the design has to spare, not logic. It
  replaced `gpu_cursor.v`'s comparators for fewer LUTs than they took.
- **A few hardware sprites for games.** The cursor sprite generalised:
  three or four more 32x32 1bpp sprites with masks would fit in the
  free space, each costing a position compare and a shift register.
- **Not the palette, here.** It is read every pixel, and this read
  port is shared with the plane passes. It went into the cursor's
  block RAM instead, whose read port the cursor needs only once a line
  (above).

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
mux was sensitive to netlist order. Treat them as rough; they predate
the line buffer moving into block RAM (above). The board-level
figure -- Lakritz measured at 98% with colour, all clocks passing
([boards.md](boards.md)) -- is the one to trust.

Other costs: no VRAM (the planes are framebuffer that already exists),
and 60 more words per line on the scanout-only VRAM port.

### More planes (`GAME_PLANES`)

Measured on the pieces, as for the cursor ([cursor.md](cursor.md),
"Cost"): `gpu_video` with DDMI, colour and the cursor, synthesised with
yosys 0.33 (`synth_ecp5 -abc9`) and placed out of context with
nextpnr-ecp5 0.6 on an LFE5U-25F, five seeds each. "Before" is the
cursor round (the 16-entry palette in distributed RAM).

| | Before | 4 planes | 6 planes | 8 planes |
|---|---|---|---|---|
| LUTs (TRELLIS_COMB, placed) | 1,651 | 1,699 | 1,870 | 2,076 |
| flip-flops | 724 | 747 | 938 | 1,150 |
| DP16KD (line buffer, cursor + palette) | 2 | 2 | 2 | 2 |
| pixel clock Fmax, five seeds (needs 25.2) | 46.7–47.9 (mean 47.4) | 48.1–50.2 (49.3) | 44.7–46.5 (45.5) | 47.3–49.3 (48.6) |
| wishbone clock Fmax (needs 48) | 109.8–129.9 (124.8) | 135.6–143.9 (140.3) | 134.2–140.1 (136.8) | 114.6–133.6 (126.7) |

And VRAM (yosys 0.70 and 0.33 agree), the other half of the bill:

| | 4 planes (9,600 words) | 6 planes (14,400) | 8 planes (19,200) |
|---|---|---|---|
| DP16KD | 20 | 32 (+12) | 40 (+20) |
| LUT4 (the read multiplexers) | about 220 | about 10 (−210) | about 220 |

- **On a 25F with six planes the LUTs come out about even**: about +220
  in `gpu_video` (two more plane words and their muxes, the second
  sheet's row arithmetic and its crossing) and about −210 in VRAM. The
  bill is 12 block RAMs.
- **Both clocks keep their margin.** The wishbone clock is the one that
  matters (r6 of the cursor lost 15 MHz of it): every plane count is at
  or above the cursor round's mean. That took one change: the first
  version latched all eight row bases from one branch of the fetch
  sequencer, which fanned out to 105 flip-flops and cost 22 MHz at eight
  planes (mean 102). The bases are now latched by a registered strobe of
  their own, and the next plane's base is chosen between registers,
  off the address adder's path.
- **Four planes** (a `COLOR` board without `GAME_PLANES`): +48 LUTs, a
  little more than the 16-entry LUT RAM palette it replaces, and both
  clocks faster than before.
- The pixel clock's critical path is still the framebuffer word's 32:1
  mux into the palette's address; at 6 planes 45 MHz against 25.2.
- `socctl`: +2 LUT4 and +5 flip-flops (the wider plane count and
  palette index).

## Boards

`COLOR` is a per-board define in `rtl/boards.vh`, not universal like
`GAME`: it needs no pins, but it costs a block RAM and logic.

| Board | `COLOR` | `GAME_PLANES` | Notes |
|---|---|---|---|
| Lakritz | on | 6 | With `USB_HOST`, `USB_CDC` and `AUDIO_MIXER`; paid for by dropping `MONTMUL`, `MONTMUL_REGS` and `SHA256`. 98% full, all clocks pass (before `GAME_PLANES`). TLS and SSH fall back to software: slower, same answers. See [boards.md](boards.md#lakritz-with-game-mode-colour). |
| Mozart ML0, Sergei ML0 | on | 6 | 25F, with `GPU_CURSOR_LUTRAM` (below) |
| Mozart ML1, ML2 | on | 8 | 45F: plenty of block RAM and logic |
| Sergei ML1, ML2 | on | 8 | 45F |
| Noir | on | 8 | 45F |
| Schoko | on | 8 | 45F, VGA and DDMI: VGA shows the top bit of each channel |

The 45F boards also define `COLOR_WINDOW` and `COLOR_DESKTOP`
([desktop colour](#desktop-colour)).
| everything else | off | — | 12F and 25F boards other than these, iCE40, GateMate, 7-series, composite |

On a composite build of a board with `COLOR`, colour goes out as a
real NTSC or PAL colour signal -- see
[composite.md](composite.md#colour), which also covers keeping black and
white for TVs that do not cope.

### Block RAM per release target

Counted with yosys 0.70 (`synth_ecp5` up to LUT mapping, where block
RAMs are final), every colour target, with the cursor and `GAME_PLANES`:

| Target | Planes | DP16KD |
|---|---|---|
| lakritz_gpio, lakritz_langkatze, lakritz_ddmi_audio | 6 | 52 / 56 |
| lakritz_kabellos | 6 | 56 / 56 (the ESP32 receive FIFO's four) |
| lakritz_katze | 6 | 57 / 56 with the cursor's block; 56 / 56 with `GPU_CURSOR_LUTRAM` |
| mozart_ml0, sergei_ml0 | 6 | the same: 57, and 56 with `GPU_CURSOR_LUTRAM` |
| mozart_ml1, mozart_ml2, sergei_ml2 | 8 | 67 / 108 |
| sergei_ml1 | 8 | 68 / 108 |
| noir | 8 | 64 / 108 |
| schoko_langkatze_gpio | 8 | 63 / 108 |

The three at 57 all have the RMII MAC with four receive slots (5
DP16KD; `rtl/ethmac_rmii.v`). They build with `GPU_CURSOR_LUTRAM`
(`rtl/boards.vh`): the cursor's four shapes (64 x 16) and the palette
(64 x 12) go into LUT RAM, read the same way and registered the same
way, and the block they shared is given back. A few dozen LUTs,
estimated, not yet measured or placed: lakritz_katze is the target to
check first. (`ETH_RX_SLOTS 2`, 3 DP16KD, would have been the other way
to find room, at the cost of two buffered frames.) The release NOTES'
Block RAM column will show the placed counts.

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

`rtl/gpu/bench/tb_planes.v` is the same method at six and eight planes
(`-DNPLANES=6` or `8`): a VRAM of one and a half or two sheets, a model
that puts planes 4..7 in the second sheet (rows mod 240 at six), and
every visible pixel of a frame checked, index and colour, for: every
plane on the quadrants with the power-on palette; wrap from odd origins
with shuffled offsets; clamp; five planes and four on the same build;
the cursor; every palette entry rewritten, and an entry above the
palette's size; the desktop untouched. With `-DGPU_DDMI -DTB_STUBS` it
checks the TMDS inputs as `tb_color.v` does. `tb_color.v` itself checks
the index on screen (`cidx_o`, the index whose colour is out of the
palette) rather than the index a clock ahead of it.

## The window manager

wm owns the colour window (it gives it to the focused screen app) and
desktop colour ([window_manager.md](window_manager.md#desktop-colour)).
Game mode itself needs nothing from it. Every way out of game mode -- Super+Esc, wm revoking
the grab when the app that holds it goes away, the lock screen -- is a
GAME write with the enable clear, and that clears colour in hardware.
Super+Esc over the desktop therefore always enters monochrome game
mode, whatever the last game left behind.

## Not done yet

- A brief tint: COLWIN changes at a frame boundary and the window's
  planes are drawn at the frame after wm's message, so a window taking
  or losing the colour can show its old planes or its black-and-white
  picture through the palette for a frame or two.

- The art side of [one game, any number of
  planes](#one-game-any-number-of-planes): pix (the pixel editor,
  planned) writes 256-colour art with its smaller palettes, and the
  loader reduces it.
- `GAME_PLANES` measured on the pieces only (gpu_video placed out of
  context, [Cost](#cost)); not yet on a board.

- `COLOR` on the remaining 25F boards (ULX3S 25k, Klinge) and the
  ULX3S 85k. Each needs its own fit and `make timing`; the 25F ones are
  as full as Lakritz was.
- Composite colour on a real TV: simulated (`tb_cvbs_color.v`), not yet
  seen on a set.
