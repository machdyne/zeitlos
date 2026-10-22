# The hardware cursor

The mouse pointer is drawn by the video output itself
(`rtl/gpu/gpu_video.v`), on top of the picture as it is sent to the
display. It is not in the framebuffer, so nothing has to redraw under it,
and it shows before any software runs: the hardware mouse path
(`rtl/usb_hid.v`) moves it from the moment the FPGA is configured. That
makes it an early sign that the system is working.

It looks the way it always has: an X, or a Z while the system is busy.
What changed is how it is drawn. The pointer is now one of four 16x16
shapes kept in a block RAM, instead of two shapes built out of
comparators (`gpu_cursor.v`, gone). That takes fewer LUTs, adds nothing
to the pixel path's timing, and lets software load shapes of its own.

## The four slots

| Slot | At power-on | Changed by |
|---|---|---|
| 0 | today's X, pixel for pixel | nothing |
| 1 | today's Z | nothing |
| 2 | a 9x9 X | wm: the large pair, `/data/wm/cursor.zsp` |
| 3 | a 9x9 Z | wm, as slot 2 |

```
 slot 0       slot 1       slot 2         slot 3
 #...#        #####        #.......#      #########
 .#.#.        ...#.        .#.....#.      .......#.
 ..#..        ..#..        ..#...#..      ......#..
 .#.#.        .#...        ...#.#...      .....#...
 #...#        #####        ....#....      ....#....
                           ...#.#...      ...#.....
                           ..#...#..      ..#......
                           .#.....#.      .#.......
                           #.......#      #########
```

- **Every pixel inverts what is under it** (the XOR the pointer has always
  used), so a shape is visible over black, white and any dither. In
  game-mode colour it inverts the active planes, as before. A pixel
  that is not part of the shape leaves the picture alone. Opaque pixels
  (a white arrow with a black edge) were measured and left out: they
  cost more logic than the whole old cursor (below, "Cost").
- **The cell's centre, pixel (8, 8), is drawn at the pointer.** That
  is the only "hot spot" there is, and it is in the hardware, so
  nothing in software changes: the click point is `curs_x/curs_y`, as
  it always was. X and Z sit centred on it, exactly where they always
  did. An arrow is drawn with its tip at the centre and the arrow in
  the bottom-right quarter of the cell.
- **Which slot is "normal" and which "busy"** is socctl's CURSOR
  register. CTRL bit 0 still picks between the two, so wm's busy and
  idle code is unchanged, and CTRL bit 1 still hides the pointer.
  **CURSOR resets to slots 0 and 1**, so every reset, a warm reboot
  included, shows the X and the Z until wm says otherwise, whatever
  was loaded into the block RAM before.

## The large pair, and Super+M

- **Super+M** switches between the small pair (slots 0 and 1) and the
  large pair (slots 2 and 3), until the next reload. With speech on it
  says "Large pointer" or "Small pointer".
- **`system.cursor.large`** in `zeitlos.cfg` (`no` by default) says
  which pair shows when wm starts and after `cfg reload`.
- **Your own large pair**: put `/data/wm/cursor.zsp` on the card. wm
  reads it at start and on `cfg reload`, and loads it into slots 2 and
  3; without it, they hold the built-in 9x9 X and Z. Nothing in flash
  or on a release card changes the pointer: only a file of yours does.

`/data/wm/cursor.zsp` is a sprite sheet in pix's text format, with two
16x16 sprites named `normal` and `busy`:

```
zsp 1
# an arrow: its tip at the centre, (8, 8)
sprite normal 16 16
	................
	................
	................
	................
	................
	................
	................
	................
	........#.......
	........##......
	........#.#.....
	........#..#....
	........#...#...
	........#.###...
	........##......
	........#.......
sprite busy 16 16
	...                     (16 rows, as above)
```

- A row is 16 characters, indented or not. `.` and `0` leave the pixel
  alone; anything else inverts it.
- Every other line is ignored: the header, other sprites, `anim`
  lines, comments. So the same sheet can hold other things.
- A file that is not usable (a row that is not 16 characters, or no
  `normal` or no `busy`) is reported on wm's console with the line,
  and the built-in pair is loaded instead:

```
wm: /data/wm/cursor.zsp: unusable (line 7)
```

pix, the pixel editor that is planned, will make these. Until then
they are written in `text`.

## Registers (socctl, [socctl.md](socctl.md))

| Address | Register | |
|---|---|---|
| `0x7000_0200` | CTRL | bit 0 busy (CURSOR's busy slot instead of its normal one), bit 1 hidden; unchanged |
| `0x7000_0220` | CURSOR | bits 1:0 normal slot, bits 5:4 busy slot (reset 0, 1); bits 21:16 (lane 2) the address `{slot, row}` of the next CURDATA row. Reads `{0x5A4B "ZK", 10'b0, busy, 2'b0, normal}` |
| `0x7000_023c` | CURDATA | write-only, whole words: a row, bit c = column c, 1 = invert. Lands on the next clock; the address moves on by one |

Words 8 and 15 alias CTRL and DIRTY on bitstreams with a 3-bit socctl
decode, and read as 0 on a 4-bit one without the shapes, so software
reads CURSOR's signature before it writes either:

```c
#include "zsoc.h"

if (z_cursor_shapes_available()) {
	static const uint16_t dot[16] = { [8] = 0x0100 };	// one pixel at the centre
	z_cursor_load(2, dot);
	z_cursor_slots(2, 1);		// normal: the dot; busy: Z
}
```

`z_cursor_load()` keeps the slots in use; `z_cursor_slots()` sets them.

## How it works (`rtl/gpu/gpu_video.v`)

- **The shapes**: `cbuf`, 512 x 32, one DP16KD on ECP5 and one RAMB18
  on 7-series. Slot s, row r is word 16s + r; bits 15:0 are the row
  (bits 31:16 are not used). Words 256..511 are the game-mode palette
  on a colour board ([color.md](color.md), "Index, palette, output");
  64..255 are free. Written by socctl in the wishbone clock domain
  through the write port (a cursor row or a palette entry), read in the
  pixel domain through the read port. Its initial contents, the four
  shapes, are generated by `tools/gen_cursor_init.py` (`--check` says
  whether `gpu_video.v` is current); the palette's follow them.
  On a board one block short, `GPU_CURSOR_LUTRAM` (`rtl/boards.vh`) keeps
  the shapes (64 x 16) and the palette in LUT RAM instead, read and
  registered the same way: Mozart and Sergei ML0 and the lakritz_katze
  target, which need the block for six bitplanes
  ([color.md](color.md#more-than-four-planes)).
- **Once a line**, in horizontal blanking (`hc == 64`), it chooses the
  row the line shows: the slot (normal or busy, by `curs_alt`) and
  `dy = y - curs_y`. The line has a row when -8 <= dy <= 7, and the
  row is dy + 8, which is dy with bit 3 flipped (no adder). Without
  colour the block RAM's registered read then holds that row for the
  whole line. With colour the read port belongs to the palette at
  every other clock, so the row is read on the clock after and held in
  a 16-bit register.
- **Per pixel**: `dx = x - curs_x` (curs_x latched with the row), the
  row's bit at dx + 8, registered: one clock behind `x`, exactly as the
  old cursor's `pixel` was. A row or column outside the cell draws
  nothing, so the pointer does not wrap to the opposite edge.
- In game mode `x` and `y` are framebuffer coordinates, so the pointer
  is pixel-doubled with the picture, as before.
- **Its own block RAM**, rather than the free words of the plane line
  buffer next to it (`pbuf`, 80 of 512 words used). Both ways were
  built and measured:
  - Loading through the line buffer's **read** port (so that it writes
    too) makes the block true dual port, which on ECP5 is at most 18
    bits wide: two blocks for a 32-bit buffer, and more logic.
  - Loading through its **write** port, past a 16-bit multiplexer, keeps
    it one block and saved about 80 LUTs against the old cursor
    (`gpu_video` and `socctl` placed together). But the 48 MHz system
    clock's Fmax fell by about 15 MHz on average (132 to 116 MHz over
    five seeds), on the fetch sequencer the multiplexer shares the
    port with.
  - A block of its own leaves the system clock where it was and saves
    more logic. A block RAM was judged cheaper than the system clock's
    margin.

## Cost

Measured on the pieces, not the whole SoC: `gpu_video` with DDMI and
colour (Lakritz's configuration), and the old `gpu_cursor` beside it,
synthesised with yosys 0.33 (`synth_ecp5 -abc9`, as the build does)
and placed out of context with nextpnr-ecp5 0.6 on an LFE5U-25F, five
seeds each. The DDMI output's ODDRs were stood in for by flip-flops so
it could be placed out of context; that is the same in both.

| | Before | After | |
|---|---|---|---|
| LUTs (TRELLIS_COMB, placed) | 1,862 | 1,651 | −211 |
| flip-flops | 707 | 724 | +17 |
| DP16KD | 1 | 2 | +1 |
| pixel clock Fmax, five seeds | 46.9–49.3 MHz (mean 47.9) | 46.7–47.9 MHz (mean 47.4) | needs 25.2 |
| wishbone clock Fmax | 108.9–132.6 MHz | 109.8–129.9 MHz | needs 48 |
| socctl (synthesised) | 475 LUT4, 17 carry | 492 LUT4, 20 carry | +17, +3 |

- The pixel clock's critical path is the same before and after: `x`
  through the framebuffer word's 32:1 multiplexer to the palette
  index. The cursor is not on it; the Fmax difference is within the
  spread between seeds.
- The cursor logic on its own: 51 LUT4 and 12 carry cells, against the
  old cursor's 142 LUT4 and 43 carry cells.
- In context the mapper's results move by more than the cursor itself
  with unrelated changes (with the cursor taken out of the new design
  altogether, the same flow gave more LUT4s, not fewer), which is why
  the table is placed results over five seeds rather than a single
  synthesis.
- **Opaque pixels** (ink and background as well as invert) were tried:
  about 200 more LUTs and 3 MHz less on the pixel clock than invert
  alone, in context. Not taken.
- **7-series** (synthesis only, `synth_xilinx`): 1,481 LUTs and 143
  CARRY4 before, 1,491 and 125 after, one RAMB18 more; the cursor
  logic on its own is 52 LUTs and 6 CARRY4 against the old one's 68
  and 24.

## Tests

- `rtl/gpu/bench/tb_cursor.v` (about ten minutes in Icarus): the new
  cursor against the old comparator cursor (`gpu_cursor_ref.v`, kept
  for this), pixel for pixel, through a second `gpu_video` that gets
  the old cursor's pixel the way `sysctl.v` used to give it: the
  monochrome output and the colour index at every visible pixel of
  every checked frame, for X and Z at the centre, every corner and
  edge, half off each side, hidden, in paper mode, in game mode
  (pixel-doubled, across the wrap seam) and in 16 colours. Then the
  shapes themselves against a model of the cell: the 9x9 pair, and
  shapes written through the port while the screen is drawn.
- The other video benches (`tb_color`, `tb_game_mode`, `tb_video_mode`,
  `tb_composite`, `tb_cvbs_color`, `tb_brightness`) pass unchanged:
  without `CURSOR_AVAIL` the module behaves as before.
- `sw/apps/wm/tests/test_dock_cfg.c`: the cursor file reader (rows
  indented and not, CRLF, other sprites and lines ignored, a short row,
  a missing `busy`, a file cut short). `test_dock_files.c`, section 26:
  wm with fixed shapes writes nothing; with them, the built-in pair, a
  file of the user's, a broken file, and Super+M's slots.

## On the board (to check)

- Power on: the Z, then the X once wm is up, exactly as before; it
  moves with the mouse before the OS has booted.
- Super+M: a 9x9 X; busy (an app starting): a 9x9 Z. Super+M again:
  the small pair.
- `system.cursor.large yes`, `cfg reload`: the large pair.
- A `/data/wm/cursor.zsp` of your own, `cfg reload`, Super+M: it shows.
  A broken one: the console says so, and the 9x9 pair shows.
- Reboot (Ctrl+Alt+Del) with the large pair showing: the small X and Z
  until wm starts.
- A game holding the screen: no pointer, as before.
