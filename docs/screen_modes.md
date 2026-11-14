# Screen modes

The desktop's resolution is chosen at run time. Press **Ctrl+Super+Esc**
for the next one, or use the Display page in `settings`. Either way the
new mode is on trial. A box asks **Keep 1280x720?** and counts down. If
you choose **Keep** or press Enter, the mode is kept and remembered. If
you choose **Go back**, press Esc, or give no answer for fifteen
seconds, it goes back. A display that cannot show a mode stays dark, and
the countdown brings the old mode back.

| Board | Modes |
|---|---|
| 45F and 85F (Mozart ML1/ML2, Sergei ML1, Noir, Schoko, ULX3S 45F/85F) | 640x480, 800x600, 1024x768, 1280x720 |
| Sergei ML2 | 640x480, 800x600, 1024x768, and 1280x720 showing a 640x360 desktop at twice the size each way (below) |
| 25F and 12F with a picture (Lakritz, Mozart ML0, Sergei ML0, Konfekt, Minze, ULX3S 12F/25F) | 640x480, and 1280x720 showing a 640x360 desktop at twice the size each way |
| Obst | 640x480: it is the small build, a 1 MB machine (below) |
| the console profile, headless boards, Artix-7, GateMate | 640x480, as before |

The modes in the cycle and in Settings are the ones the machine has, in
that order, and the cycle wraps round. Game mode's 320x240 is not one of
them. **Super+Esc** is still game mode, and the 320x240 game picture is
always shown on a 640x480 signal:

- A game that takes the screen (`z_game_set_enabled()`) switches the
  machine to 640x480 itself.
- When the game lets go, wm puts back the mode that was kept.
- wm's own game-mode camera over the desktop (Super+Esc, Super+Arrow)
  works only at 640x480. In any other mode, Super+Esc only takes the
  screen back from a game.

### Sergei ML2

Sergei ML2 is the fullest board: DDR3, Keccak, SHA-256, montmul,
S/PDIF, chunky colour and the rest, 75% of a 45F's LUTs. With the 45F
modes' full VRAM (32K words for native 1280x720: 64 DP16KD, 93 of the
108 in all) it did not route reliably: with Keccak on no seed, without
it on some seeds and not others, and a small change elsewhere could
undo a seed that had worked. Other seeds, placement density, router2
and VRAM split into byte lanes did not change that.

So on Sergei ML2 (`rtl/boards.vh`, keyed on the board):

- **No native 1280x720** (`VIDEO_NO_NATIVE_720`). Mode 3 is the 640x360
  desktop shown twice each way, as on the 25F boards; 800x600 and
  1024x768 stay native. VRAM need only hold 1024x768: 24,576 words
  rather than 32K, which saves 16 DP16KD. Game mode's two sheets
  (19,200 words) fit inside, and 800x600 (15,000) costs nothing.
- **No Keccak block** (NO_KECCAK). SHA-3 and ML-KEM take the software
  path, with the same answers (docs/keccak_hw.md). It was the most
  congested logic on the chip, about 6,000 LUT4.

`EXTRA_DEFINES=-DNO_VIDEO_WIDE` builds it with Keccak and the 25F
boards' modes instead (VRAM as before, 69 DP16KD); that routed and met
every clock: bclk 383.9 MHz, cclk 159.6, pclk 50.3, the CPU 53.4.
`NO_VIDEO_WIDE` works on any 45F or 85F board, and
`VIDEO_NO_NATIVE_720` on any of them too.

Built so, with nextpnr's default seed: 27,567 LUT4 (62%) and 77
DP16KD; router1 converged in 1,391 s, and every clock passes -- bclk
413.2 MHz of 378, cclk 162.0, pclk 51.8, the CPU 53.1 of 48
(docs/boards.md).

## The signals

| Mode | Lines | Line rate, frame rate | Pixel clock | Syncs | HDMI |
|---|---|---|---|---|---|
| 640x480 | 800 x 525 | 31.5 kHz, 60.0 Hz | 25.2 MHz | - - | VIC 1, 4:3 |
| 800x600 | 998 x 628 | 37.88 kHz, 60.3 Hz | 37.8 MHz | + + | no VIC, 4:3 |
| 1024x768 | 1328 x 806 | 56.93 kHz, 70.63 Hz | 75.6 MHz | - - | no VIC, 4:3 |
| 1280x720 | 1680 x 750 | 45.00 kHz, 60.00 Hz | 75.6 MHz | + + | VIC 4, 16:9 |

The line and frame rates are the standard ones: VESA's for 800x600 and
1024x768, CEA-861's for 720p. Displays recognise a mode by its line and
frame rates, and over DVI/HDMI they find the picture by its
data-enable signal, so a longer line does not change what an HDMI
display sees. 720p's line has 1680 pixels rather than 1650; every other
count is CEA's.

A VGA monitor (or an HDMI-to-VGA adapter, which passes the line on
pixel for pixel) is different: it recognises the mode and then samples
the standard number of pixels per line, and its auto-adjust can only
correct a few per cent. 800x600's 998 for 1056 (5.5%) and 720p's 1680
for 1650 (1.8%) are within that. **1024x768 is VESA's 70 Hz mode** for
that reason: its line is exactly 1328 pixels (75.0 MHz), which at our
75.6 MHz is 70.6 Hz rather than 70.1, and a VGA monitor samples our
pixels exactly. (The first version used VESA's 60 Hz line rate with a
1562-pixel line, 16% longer than the monitor's 1344, and on the first
hardware test, through an adapter, the picture came out narrow and
auto-adjust could not fix it.)

## The gateware

`rtl/boards.vh` defines **VIDEO_MODES** for an ECP5 desktop with a
picture, and **VIDEO_WIDE** on a 45F or 85F chip. The Makefile passes
ECP5_BIG for DEVICE=45k or 85k, so a ULX3S gets it from its DEVICE.
**NO_VIDEO_MODES** builds the 640x480-only gateware, exactly as before;
**NO_VIDEO_WIDE** gives a 45F or 85F board the 25F boards' modes;
**VIDEO_NO_NATIVE_720** keeps the 45F modes but doubles 1280x720 from a
640x360 desktop, as the 25F boards do, so VRAM need only hold 1024x768.
The console profile builds that too, and so do headless builds,
composite, Artix-7, GateMate and Obst.

### Clocks that never change

`rtl/clk/pll1.v` makes three clocks from one 756 MHz VCO, and
CLKDIVF a fourth. On ULX3S,
`rtl/clk/pll1_25.v` makes them from 25 MHz, 0.15% fast.

| Clock | Frequency | Used by | Role |
|---|---|---|---|
| pclk | 37.8 MHz | `rtl/gpu/gpu_video.v` | The pixel logic. At 640x480 it is enabled two clocks in three, an effective 25.2 MHz, so every count is as it always was. |
| cclk | 75.6 MHz | `rtl/gpu/gpu_vchar.v` | The character side: one character on the wire every 3 clocks at 640x480, every 2 at 800x600, every clock at 1024x768 and 1280x720. |
| bclk | 378 MHz | the serialiser | Two bits a clock into an ODDRX1F, or (DDMI_ODDRX2) the ODDRX2Fs' edge clock: 756 Mb/s per lane. |
| sclk | 189 MHz | the serialiser, DDMI_ODDRX2 only | bclk halved by CLKDIVF. Twenty bits every five clocks, four a clock into each lane's ODDRX2F. |

Only the two-pixel modes use the full 756 Mb/s. The slower modes keep
the same serialiser and clocks and **repeat each bit**: three half-bits
a bit at 640x480 (252 Mb/s), two at 800x600 (378 Mb/s). So no clock is
ever switched, stopped or re-locked. A mode change is a change of
counters, made at a frame boundary.

### Two serialisers

ECP5 has edge clocks, and so ODDRX2F, only in its left and right I/O
banks. `rtl/boards.vh` defines **DDMI_ODDRX2** for the boards whose HDMI
pins are in one of those, keyed on the board so release builds have it
too:

| Serialiser | Boards | Fabric clock |
|---|---|---|
| ODDRX2F | Lakritz, Konfekt, Noir | sclk, 189 MHz |
| ODDRX1F | Mozart and Sergei (ML0, ML1, ML2), ULX3S, Schoko | bclk, 378 MHz |

Schoko's pins (R5, P4) may be in a side bank; if they are, it can move
to the first row. On a top or bottom bank nextpnr refuses ODDRX2F
("only supported for left and right IO").

With ODDRX1F the last register before each pin can be far from it,
and at 378 MHz a clock is 2.6 ns: the first version, with one register,
routed at 322 MHz on Mozart ML1 and 354 on ULX3S 85F. So there are three
registers a lane between the shifter and the ODDRX1F, for the placer to
walk the bits out in short hops. With ODDRX2F the 378 MHz clock stays
inside the I/O cells and the fabric runs at 189 MHz.

### The serialiser's place

On a board whose HDMI pins are along the top (ODDRX1F), each lane's
378 MHz flip-flops -- shift register, ring, lock, three output stages
-- are pinned beside its pin: `tools/ddmi/pin_serialiser.py`, run
between synthesis and nextpnr, puts nextpnr's BEL attribute on them,
in rows 2..4 of the pin's column and the column beside it, a tile for
each kind of control signal (a tile's slices share them). Left to
itself, nextpnr put them 35 to 65 columns from the pins on Sergei ML2,
and bclk passed or failed by a few per cent with the seed (363 to 413
MHz over builds of the same RTL). The one long hop left is the
character clock's words into the shift register, which the design
gives 7.9 ns or more and nextpnr does not time.

A flip-flop fed by a LUT is placed with it, by the LUT, so the script
pins the LUT that feeds each pinned flip-flop into the same slice
(without that, nextpnr ignored the flip-flops' BELs, and bclk fell to
243-258 MHz). With both pinned, Sergei ML2 routed with bclk at 453.9
MHz of 378: the longest 378 MHz path 2.2 ns, the character clock's
crossing 2.0 ns.

The pins' columns are a table in the script, measured from nextpnr's
placement: 45F in CABGA256 (Mozart and Sergei ML1/ML2) and ULX3S 85F.
On any other board or device it pins nothing and says so, and the
build goes on as before.

### Pixels

`gpu_video.v` makes a *sample* on each enabled pclk:

- **One pixel** at 640x480 and 800x600.
- **Two pixels** at 1024x768 and 1280x720. On a WIDE build these are two
  columns; there, h counts pairs of pixels.
- **One pixel shown twice** at 1280x720 on a 25F. Rows are also shown
  twice, as game mode does, so the desktop is 640x360.

Each sample carries its syncs, blanking, hc and vc.

`gpu_vchar.v` moves the sample across into cclk and spreads it into
characters:

1. pclk toggles a bit on every clock. The clock after that bit changes
   in cclk, the registered sample is a whole one.
2. A two-sample FIFO absorbs 640x480's uneven arrivals. The rates are
   equal by construction, so the FIFO never fills and, once running,
   never runs dry.
3. The characters go to the TMDS encoders (`rtl/gpu/tmds_enc_p.v`,
   pipelined in three stages, enabled once a character) and to the
   data-island scheduler.
4. The encoder's output is spread over the character's clocks, ten bits
   a clock with each bit repeated. Every other clock two of these words,
   twenty bits, go to the serialiser together.
5. The serialiser finds the phase of the words once, from a cclk
   toggle registered in its own clock, and makes its first load well
   inside the time a word holds still. After that a ring of five loads
   every fifth clock and the toggle is not looked at again: the clocks
   come from one VCO, so the phase never moves, and a synchroniser can
   never make the load wander. (With ODDRX2F, sclk is exactly 2.5 cclk,
   so the words go in pairs: twenty bits every five sclk.)
6. VGA takes the character's bits, registered, for as long as the
   character lasts.

The other modes are **monochrome**, in the phosphor's ink. Game mode,
the colour window and desktop colour are 640x480's only; the hardware
turns them off in any other mode (`vm_m0`), and
`z_color_window_available()` says so.

### Rows, the cursor and VRAM

| Item | Change |
|---|---|
| Row length | On a WIDE build: 20, 25, 32 or 40 words. The row's VRAM address is worked out in the pixel domain and crosses to the fetch with the row. |
| Line buffer | Holds a 40-word row in its first two plane rows. Plane rows are only fetched at 640x480. |
| Blitter | `rtl/gpu/gpu_blit.v` takes the mode for its row length: 80, 100, 128 or 160 bytes. |
| Rasteriser | Stays 640 wide, so `zgfx` draws lines in software in the other modes. |
| VRAM | 32,768 words on a WIDE build: 64 DP16KD. |
| Cursor | 11 bits across. |
| Pointer's x | The 11th bit is bit 28 of the USB cursor register and bit 23 of the virtual mouse register; both bits read 0 before this change. |
| Pointer limits | Kept inside the mode's screen by `rtl/usb_hid.v` and `rtl/usb/usb_hid_compat.v`, which also pull it back on when the screen gets smaller. |

### socctl's VMODE (word 19, 0x7000_024C)

| Access | Bits |
|---|---|
| Write | `[1:0]`: the mode. A mode this machine does not have is ignored. |
| Read | `{ "ZM", 4'b0, avail[3:0], 2'b0, wide, dbl, 2'b0, mode }`: wide, 800x600 and 1024x768 native; dbl, 1280x720 doubled from 640x360 |

The mode is adopted at the next frame boundary.

On older bitstreams:

- A bitstream without screen modes reads avail = 0001: 640x480 only.
- One from before VMODE reads 0.
- One that decodes only four address bits aliases word 3 (GAME), whose
  signature is not "ZM". Software reads the signature first
  (`z_vmode_present()`).

### Timing, at speed grade 6

At nextpnr's default grade, the slowest. The clocks to check are bclk
(378 MHz) on an ODDRX1F board or sclk (189 MHz) on an ODDRX2F one, cclk
(75.6 MHz) and pclk (37.8 MHz). On Lakritz nextpnr puts the edge clock in bank 6 on dedicated
routing, and its estimate after placement is 194 MHz for sclk, 151 for
cclk and 54 for pclk, before routing. Routed figures for each board go
in docs/boards.md.

The first serialiser (ODDRX1F, fabric at 378 MHz) routed at 426 MHz on
Lakritz and 416 on Konfekt, but 322 on Mozart ML1 and 354 on ULX3S 85F.

## The software

| Part | What it does |
|---|---|
| `zsoc.h` | `z_vmode_present()`, `_avail()`, `_get()`, `_doubled()`, `_geometry()`, `_tmds_khz()`, and `z_vmode_set_raw()` (wm's). |
| `zgfx.h` | `Z_SCREEN_W` and `Z_SCREEN_H` are now variables: the framebuffer's geometry, read from VMODE by `z_fb_geometry_refresh()`. `zwin.c` calls it when a window is made, before each region and before each redraw, so an app picks up a new mode before it next draws. Buffers that need a compile-time size use `Z_FB_640_W/H` (a 640x480 page: `draw`'s canvas, `learn`'s page) or `Z_FB_MAX_W/H`. |
| Small build (`Z_SMALL`) | Keeps them as constants and has no screen modes. |
| Kernel | `Z_SYS_SCREEN` (`z_screen_size()`) answers the mode's desktop, unless `system.screen.size` asks for another size to test. |
| `zwm.h` | `Z_WM_SCREEN`, wm to every program: the screen is now w x h in mode m. `Z_WM_SCREEN_MODE`, a program to wm: change to this mode, with the question. `z_screen_set_mode()` in `zeitlos.h` sends it. |
| wm | Switches the mode, waits two frames, then lays the desktop out again (below). The question box stops every window, as the reboot dialog does. The kept mode is stored under `apps.wm.screen` in the flash key/value store (docs/kvstore.md), so the machine starts in it, with or without a card. |
| `zaudio.h` | HDMI sound's clock regeneration (N, CTS) is measured against the mode's character clock, so wm sets it again after a change (`z_audio__ddmi_sync()`). |
| `settings` | The Display page has a Resolution row: one button per mode the machine has. |

When wm lays the desktop out again after a switch:

- Every window is moved or made smaller so that it fits.
- Maximized and full-screen windows take the new screen.
- The dock goes to its side.
- The screen is cleared and repainted.
- Every program is told with `Z_WM_SCREEN`.

### The accent plane

Desktop colour keeps its accent plane in VRAM's second sheet, the
rows below a 640x480 screen (docs/color.md). In any other mode there
is no colour, and those rows are not a second sheet: at 1024x768 they
are past the end of Sergei ML2's 24,576 words, where a fill wraps onto
the top of the screen (on the first hardware test, a white box there
over the windows). So wm (`acc_rect()`, `acc_on()`, `acc_service()`)
and `z_win_accent_rect()` draw the accent only at 640x480, and
`z_win_accent_ok()` says no in the other modes.

### Screenshots

The kernel console's `ss` dumps the framebuffer of the mode the screen
is in (`/home/ss.bin`, width/8 bytes a row) and writes its size to
`/home/ss.txt`; `tools/ssconv.py` reads that to make the PNG. Game
mode's framebuffer is 640x480.

### What stays 640x480

These see 640x480 whatever the mode:

- the remote desktop's picture and its DIRTY stripes (`zscreen.h`);
- T2;
- the rasteriser;
- game mode.

### The small build

Obst's 1 MB has its RAM budgets spoken for to the byte (docs/flash_apps.md,
"Budgets"). wm's switching and question come to about 6 KB, and every
app that draws would need about 400 bytes more to read its geometry at
run time. So the small build has neither: Obst's gateware is built with
NO_VIDEO_MODES and its apps with constant geometry.

## The emulator

`sim/sys` has VMODE. `ZSYS_VMODES` chooses which board it acts as:

| Setting | Acts as |
|---|---|
| `wide` (the default) | a 45F board |
| `dbl` | a 25F board: 1280x720 is 640x360 |
| `widedbl` | Sergei ML2: the 45F modes, with 1280x720 from 640x360 |
| `none` | a bitstream without screen modes |

Screenshots and the window take the mode's size. The emulator shows the
desktop's pixels; the hardware's doubling is not drawn.

```
./zsys --flash out/boot.img --snapshot --fast --seconds 12 --after "init: done" \
    --wait 2000 --keys '\{Ctrl+Super+Esc}' --wait 3000 --screenshot out/800x600.png
```

## The bench

`rtl/gpu/bench/tb_vmodes.v` takes each mode off the wire, and
`tools/ddmi/test_vmodes.py` checks it against the mode's table:

- line and frame lengths, and sync widths and polarities;
- every pixel of a whole frame against the framebuffer, doubled where
  the mode doubles;
- with data islands: preambles, guard bands, BCH parity, and the AVI
  InfoFrame's VIC and aspect.

`make test_vmodes` runs all of them, on both builds. By default the
bench captures the encoders' characters (`-DFAST`): about ten minutes a
mode on one core. `VMODES_SLOW=1` rebuilds the capture from the four
pins instead, so the serialiser and the bit repetition are checked
too; that is several times slower. `VMODES_X2=1` builds the ODDRX2F
serialiser, otherwise the ODDRX1F one.

The syncs are read only from control periods and data islands. The
video guard band's channel 0 is the same code as TERC4's 8, and a
pixel can be any code, so between the video guard band and the next
control character the checker holds the syncs where they were.
