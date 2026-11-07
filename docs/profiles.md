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

- **wm** creates no dock and no desktop, and draws no window: every
  window is the whole screen, without titlebar or frame. One that will
  take game mode itself (an arcade program's) is left to, with the
  launcher keeping game mode until it does; for any other wm takes game
  mode from the launcher so that it shows. F11 and Escape are the
  program's, not wm's. It starts the **launcher**
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

One thing the profile does itself: a console build with a DDMI output
has **sound over HDMI** (`GPU_DDMI_AUDIO`, [ddmi_audio.md](ddmi_audio.md))
by default -- a console goes to a television -- set in rtl/boards.vh's
"The console's defaults". It is still off at power-on until the OS sets
it (`system.video.ddmi_audio = yes`, or the shell's `ddmi on`).
`NO_DDMI_AUDIO` in a target leaves it out: about 930 LUT4, no block
RAM -- lakritz_console leaves it out: with it, and the line fetch four
lines ahead, the build came to 24,560 of 24,288 LUT4.

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
without the **MPU** (`NO_MPU`), the hardware random generator
(`NO_TRNG`: zrng seeds in software) and the wall clock (`NO_RTC`). Its
serial console is UART0 on PMOD A (`pmods = uart@a`, a 3.3 V USB-serial
adapter on pins 2 and 3) rather than the USB-C socket (`-USB_CDC`).
It has **no line rasterizer** (`-GPU_RASTER`): T2 draws the 3D games'
lines and triangles, and zgfx draws a 2D line itself where FEATURES
says there is no rasterizer -- the same pixels, as
`sw/common/tests/sim_lines` checks against a model of `gpu_raster.v` --
while sysctl answers the rasterizer's registers (writes dropped, reads
0) so nothing waits on it for ever. And **no data cache** (`-DCACHE`):
of the two last big blocks, it and the hardware audio mixer, the mixer
saves the CPU more (software mixing is about 15% of it while music
plays; the D-cache about 10% on data-heavy code). The picorv32
register file is in LUT RAM (`PICORV32_REGS_LUTRAM`), and the block
RAM the data cache had goes to the instruction cache (4 KB) and T2's
write queue (1,024 words).

**The fit.** Measured as a release build does it -- yosys 0.70
`synth_ecp5 -abc9`, then nextpnr-ecp5's packer -- TRELLIS_COMB:

| | here | a release build |
|---|---|---|
| everything but the MPU, rasterizer and TRNG | 24,738 (101%) | 25,549 (105%), did not place |
| also `NO_RTC`, `-USB_CDC` | 23,471 (96%) | about 24,300 |
| also `-AUDIO_EXACT` | 23,440 | |
| also `-AUDIO_MIXER` instead | 22,119 (91%) | |
| **as specified** (also `-DCACHE`; I-cache 4 KB, T2's queue 1,024) | **22,359 (92%)** | **about 23,200 (95%)** |

Block RAM 56 of 56: T2 30 (record list 11, code 6, stream FIFO 4,
write queue 3, strip buffers 3 at 6 rows, data 2, the rest 1), VRAM
10, BIOS 4, I-cache 3, glyphs 2, video 2, the two USB ports 1 each,
audio 1, the fetch 1, the ROM 1. A release build places, and meets
timing: 53.03 MHz against the 48 needed.

### Sergei ML2 (`sergei_ml2_console`)

The same console on the board that is easier to test on: the launcher,
two planes in block RAM, chunky's picture in main memory -- DDR3 here,
read a word at a time (the DDR3 controller holds a 16-byte block, so a
line's words after each block's first come in a cycle) -- T2 writing it
there, no rasterizer, no MPU, no RTC, no TRNG; no pointer, and none of
the crypto blocks (Keccak, MONTMUL, SHA-256: software does without
them); the HID-only USB (`USB_HID`, as on Lakritz) rather than the full
host. It keeps the network (RMII), so
software can be updated over the wire while it is being tested. What
works here should work on Lakritz, bar the memory controller.

Three things only it has, the 45F having the room:

- **zeitlos32p** (`CPU_ZEITLOS32P`, [zeitlos32p.md](zeitlos32p.md)) in
  picorv32's place: the 3D games' CPU part runs at about 15 cycles an
  instruction on picorv32 here (mausrun3d.md, "Frame budget"), and
  zeitlos32p ran Doom at 0.65 instructions a cycle against picorv32's
  0.14. Without the MPU (`NO_MPU`), as on Lakritz.

- **Write combining in the DDR3 controller** (`DDR3_WCOMBINE`,
  [ddr3.md](ddr3.md)): stores merge into whole 16-byte blocks, so T2's
  picture goes out at about 8 cycles a word rather than 19.
- **T2's textures** (`T2_TEX_LOG2=13`, [zgl_t2.md](zgl_t2.md),
  "Textures"): an 8 KB texture memory, perspective-correct.

With picorv32, MONTMUL, SHA-256, the TRNG and the full USB host:
82 of 108 block RAMs and 76% of TRELLIS_COMB at packing (yowasp yosys
0.70, `-abc9`); as specified now, to be measured. On it gldemo's chunky scene runs at 65 frames a second
(T2, the picture in main memory, three pages: zgl_t2.md, "Pages"), from
37 before the pages; with textures built in, 59-60. Textures in the
bitstream needed the texture combine in a stage of its own to meet 48
MHz (44.9 without). mausrun3d runs textured at 18-20 frames a second,
the same as untextured, and space3d at 15 (zgl_t2.md, "On hardware").

## The headless profile

Defined, reported, and not yet acted on: a headless build today is a
desktop build without a screen. What it will do (no wm, the network
services from boot) is to come.
