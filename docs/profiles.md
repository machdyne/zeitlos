# Profiles

Because the hardware is an FPGA, the system can be built for different
purposes. Zeitlos has three **profiles**:

| Profile | What it is for | Graphics |
|---|---|---|
| Desktop | Everyday work, learning, programming, and games | 640x480 monochrome (white, amber, green or paper), [desktop colour](color.md#desktop-colour) on the 45F boards, and the 320x240 [game mode](game_mode.md) in a window or full screen |
| Console | Games, demos and 3D | Exclusively the 320x240 game mode in up to [256 colours](color.md), with the [T2](zgl_t2.md) geometry engine |
| Headless | Servers, BBSes and controllers | Optional [remote desktop](remote_desktop.md) |

## How a build says which

A define in the board's block in `rtl/boards.vh`, or in a release
target's spec (`release/targets/*.spec`, [releases.md](releases.md)):

| Define | Profile |
|---|---|
| neither | desktop (every build before the profiles) |
| `` `ZEITLOS_CONSOLE `` | console |
| `` `ZEITLOS_HEADLESS `` | headless |

The bitstream reports it in `rtl/csrs.v`'s **FEATURES3** (word 4 of the
CSR window, `0x7000_0010`): `{ 16'h5A33, features3[15:0] }`, the profile
in bits 1:0 (0 desktop, 1 console, 2 headless). Software asks
`z_profile()` (`sw/common/zsoc.h`); a bitstream without FEATURES3 is a
desktop.

## The console

What the OS does differently:

- **wm** creates no dock and no desktop. It starts the **launcher**
  (`sw/apps/launcher`, [launcher_app.md](launcher_app.md)) -- a menu of
  `/apps` in game mode, the games and demos first -- and starts it
  again whenever it stops.
- **The arcade runtime** ([arcade.md](arcade.md)) goes full screen as
  soon as a game opens, F11 does nothing (there is no window to go back
  to), and Escape ends the game -- back to the launcher.
- A game chooses its own mode, as on a desktop: the arcade games their
  bitplanes (as many colours as the board has planes for, and the
  blitter; on Lakritz two planes, below); the 3D ones chunky game mode,
  256 colours, and T2 (`ZGL_CHUNKY`, [zgl.md](zgl.md),
  [arcade.md](arcade.md), "Chunky").

What the hardware does differently is the target's business; the
console's first is Lakritz.

### Lakritz (`lakritz_console`)

Lakritz is an ECP5-25F: 24,288 LUT4 and 56 block RAMs. Its board build
is already at 98% of the LUTs (docs/boards.md, "Lakritz with game-mode
colour"), and 256 colours of VRAM (40 block RAMs) plus T2 (30 of its
own) cannot both be there. So the console splits the screen:

- **The planes stay in block RAM**: two 320x240 bitplanes (4,800 words,
  10 DP16KD), framebuffer rows 0..239. The blitter and the CPU draw
  there exactly as before, a word a cycle.
  COLOR reports two planes (XP 3); writes past row 239 are dropped.
  The arcade runtime draws its games in monochrome there for now (it
  uses colour planes from 16 colours up).
- **Chunky's 256 colours are in SDRAM** (`` `VRAM_SDRAM ``): a linear
  picture of 76,800 bytes at socctl's CKBASE. T2 writes it there
  (`gpu_t2.v` `LINEAR`, with posted writes), software draws in it as it
  is, and `rtl/gpu/gpu_ckfetch.v` reads it out on gpu_video's graphics
  port a line ahead of the beam -- two lines in a block RAM of its own,
  the next read in four-word bursts while the one before is shown.
  CKBASE is taken at the frame's line 0, so writing it is a page flip
  (docs/color.md, "In main memory").
- The scanout's fetch and T2's output share wb_arbiter_main's third
  port with the audio mixer, behind `rtl/arbiter_gfx.v` (the fetch
  first, then the mixer, then T2).

Software needs nothing new: `z_color_set_chunky(true)` allocates the
picture and points CKBASE at it, `z_chunky_seg()` and
`z_chunky_put_rows()` write into it, and zgl keeps T2's VBASE on it.
FEATURES3 bit 2 (`z_vram_sdram()`) says it is such a machine.

**Checked:** `rtl/gpu/bench/tb_ckfetch.v` (every pixel of whole frames
from a memory with random latency, shared with a busy master; a page
flip mid-frame; the planes from VRAM), `tb_gpu_t2.v -DLINEAR` on four
of test_t2's frames, and in the simulator chunkytest with
`ZSIM_CONSOLE=1` (software strips and T2 into the picture in main
memory): all pass.

**What else goes.** The console runs one game at a time, so it does
without the **MPU** (`NO_MPU`) and the hardware random generator
(`NO_TRNG`: zrng seeds in software). It has **no line rasterizer**
(`-GPU_RASTER`): T2 draws the 3D games' lines and triangles, and zgfx
draws a 2D line itself where FEATURES says there is no rasterizer --
the same pixels, as `sw/common/tests/sim_lines` checks against a model
of `gpu_raster.v` -- while sysctl answers the rasterizer's registers
(writes dropped, reads 0) so nothing waits on it for ever. The
picorv32 register file goes into LUT RAM (`PICORV32_REGS_LUTRAM`),
freeing two block RAMs for the data cache.

**The fit.** Synthesis (yosys 0.33 `synth_ecp5`, the board build
synthesised the same way for comparison):

| | DP16KD | LUT-equivalents (a carry cell two) |
|---|---|---|
| `lakritz` (the board build: 98% in nextpnr, meets 48 MHz) | 52 | 28,895 |
| `lakritz_console`, T2 as on sergei_ml2, nothing else cut | 65 | 32,053 |
| `lakritz_console` as specified | **56** | **28,864** |

As specified: `NO_MPU`, `-GPU_RASTER`, `NO_TRNG`,
`PICORV32_REGS_LUTRAM`, both caches 2 KB, `AUDIO_FIFO_LOG2=9`, T2's
strips 6 rows (`T2_STRIP_H=6`, +19 k cycles a frame) and its write
queue 512 (`T2_POST_LOG2=9`, +7 k). T2 alone is 29 block RAMs then
(record list 11, code 6, stream FIFO 4, strip buffers 3, write queue
2, data 2, the rest 1). The serial console (`USB_CDC`) stays. Not yet
placed and routed: the logic is where the board build is, which is
tight.

## The headless profile

Defined, reported, and not yet acted on: a headless build today is a
desktop build without a screen. What it will do (no wm, the network
services from boot) is to come.
