# Zeitlos GPU Blitter Developer Guide

## Overview

The Zeitlos GPU Blitter is a high-performance 2D graphics accelerator designed for efficient rectangular fills, sprite blitting, and text rendering. It operates on a 640×480 monochrome (1-bit-per-pixel) framebuffer, with word-level optimization and clipping support.

`sw/common/zgfx.c` is the recommended way to drive this hardware rather
than raw register writes -- `z_fb_hw_fill_rect()` and
`z_fb_hw_fill_pattern()` for fills, `z_fb_hw_blit_mem()` and
`z_fb_hw_blit_vram()` for copies. See "Best Practices" below.

## Key Features

- **Hardware-accelerated rectangular fills**
- **Screen boundary clipping when `CTRL_CLIP` is requested** -- see "Clipping Behavior" below
- **Word-level optimization with pixel-precise masking**
- **Simple memory-mapped register interface**
- **Optimized for font rendering** (any glyph up to 8 pixels wide; the
  board-wide font is `z_font_5x8`)
- **Hardware glyph blit mode** for accelerated text rendering
- **Copy from main memory or from VRAM**, at arbitrary bit alignment --
  see "Copy modes" below
- **`z_fb_hw_fill_rect()` (fill mode only) adds unconditional bounds safety and cross-process protection on top of the raw hardware** -- see "Best Practices" below

## Memory Map

| Address | Register | Description |
|---------|----------|-------------|
| 0xD0000000 | BLIT_CTRL | Control register |
| 0xD0000004 | BLIT_STATUS | Status register (read-only) |
| 0xD0000008 | BLIT_DST_X | Destination X coordinate (pixels) |
| 0xD000000C | BLIT_DST_Y | Destination Y coordinate (pixels) |
| 0xD0000010 | BLIT_WIDTH | Width in pixels (fill/copy mode only) |
| 0xD0000014 | BLIT_HEIGHT | Height in pixels (fill/copy mode only) |
| 0xD0000018 | BLIT_PATTERN | Fill pattern (fill/copy mode only) |
| 0xD000001C | BLIT_GLYPH_ADDR | Glyph mode: byte offset into glyph memory |
| 0xD0000020 | BLIT_GLYPH_W | Glyph mode: glyph width in pixels |
| 0xD0000024 | BLIT_GLYPH_H | Glyph mode: glyph height in pixels |
| 0xD0000028 | BLIT_FG_COLOR | Glyph mode: foreground pixel value |
| 0xD000002C | BLIT_BG_COLOR | Glyph mode: background pixel value |
| 0xD0000030 | BLIT_SRC_ADDR | Copy mode: source address (see "Copy modes") |
| 0xD0000034 | BLIT_SRC_STRIDE | Copy mode: source row pitch in bytes |
| 0xD0000038 | BLIT_SRC_SHIFT | Copy mode: window bit offset + prime flag |

Note there is **no BLIT_SRC_X / BLIT_SRC_Y**. Earlier revisions of this
document listed them at 0xD000001C and 0xD0000020; those addresses were
reassigned to the glyph path (`BLIT_GLYPH_ADDR` / `BLIT_GLYPH_W`) and the
registers no longer exist. The source rectangle's position is folded into
`BLIT_SRC_ADDR` and `BLIT_SRC_SHIFT` instead -- see "Copy modes".

The register file is addressed by `wb_adr_i[3:0]`, so index 15
(0xD000003C) is the last one available. Anything further would need the
decode widened.

## Control Register (BLIT_CTRL)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | START | Start operation (write 1 to begin) |
| 1 | FILL | Operation mode (0=copy, 1=fill) -- ignored in glyph mode |
| 2 | CLIP | Clipping enable (0=disabled, 1=enabled) -- ignored in glyph mode |
| 3 | GLYPH | 0=normal fill/copy, 1=glyph blit mode |
| 4 | SRCMEM | Copy mode source: 0=VRAM, 1=main memory. Ignored unless FILL=0 and GLYPH=0 |

## Status Register (BLIT_STATUS)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BUSY | Operation in progress (1=busy, 0=idle) |

## Screen Specifications

- **Resolution**: 640×480 pixels
- **Color depth**: 1-bit per pixel (monochrome)
- **Framebuffer**: Located at 0x20000000
- **Stride**: 80 bytes per line (640 pixels ÷ 8 bits/byte)

## Programming Interface

### C Header Definition

```c
#define BLITTER_BASE    0xD0000000

#define BLIT_CTRL       (*(volatile uint32_t*)(BLITTER_BASE + 0x00))
#define BLIT_STATUS     (*(volatile uint32_t*)(BLITTER_BASE + 0x04))
#define BLIT_DST_X      (*(volatile uint32_t*)(BLITTER_BASE + 0x08))
#define BLIT_DST_Y      (*(volatile uint32_t*)(BLITTER_BASE + 0x0C))
#define BLIT_WIDTH      (*(volatile uint32_t*)(BLITTER_BASE + 0x10))
#define BLIT_HEIGHT     (*(volatile uint32_t*)(BLITTER_BASE + 0x14))
#define BLIT_PATTERN    (*(volatile uint32_t*)(BLITTER_BASE + 0x18))

#define BLIT_SRC_ADDR   (*(volatile uint32_t*)(BLITTER_BASE + 0x30))
#define BLIT_SRC_STRIDE (*(volatile uint32_t*)(BLITTER_BASE + 0x34))
#define BLIT_SRC_SHIFT  (*(volatile uint32_t*)(BLITTER_BASE + 0x38))

#define CTRL_START      (1 << 0)
#define CTRL_FILL       (1 << 1)
#define CTRL_CLIP       (1 << 2)
#define CTRL_GLYPH      (1 << 3)
#define CTRL_SRCMEM     (1 << 4)

#define SRC_PRIME_ZERO  (1 << 8)   /* in BLIT_SRC_SHIFT */

#define PATTERN_BLACK   0x00000000
#define PATTERN_WHITE   0xFFFFFFFF
```

### Basic Usage

#### 1. Wait for Completion

Always wait for the blitter to complete before starting a new operation:

```c
void wait_blitter_done(void) {
    while (BLIT_STATUS & 1) {
        // Wait for busy flag to clear
    }
}
```

#### 2. Fill Rectangle with Clipping

```c
void fill_rect(int x, int y, int width, int height, uint32_t pattern) {
    BLIT_DST_X = x;
    BLIT_DST_Y = y;
    BLIT_WIDTH = width;
    BLIT_HEIGHT = height;
    BLIT_PATTERN = pattern;
    BLIT_CTRL = CTRL_START | CTRL_FILL | CTRL_CLIP;
    
    wait_blitter_done();
}
```

#### 3. Clear Screen (Fast)

```c
void clear_screen(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = 640;
    BLIT_HEIGHT = 480;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping needed for a known full-screen op
    
    wait_blitter_done();
}
```

## Common Use Cases

### Drawing Pixels and Small Rectangles

```c
// Single pixel
fill_rect(10, 20, 1, 1, PATTERN_WHITE);

// Small rectangle
fill_rect(50, 50, 10, 10, PATTERN_WHITE);

// Font-sized character block (z_font_5x8)
fill_rect(100, 100, 5, 8, PATTERN_WHITE);
```

### Text Rendering (glyph blit mode)

Hardware-accelerated text rendering is implemented as a dedicated glyph
blit mode -- see `docs/window_manager.md`, "Hardware glyph blitting"
for the full register-level design, and `sw/common/zgfx.c`
(`Z_GFX_HW_BLIT` build) for the C driving it.

`gpu_blit_wb` reads glyph row data from a separate glyph memory
(`rtl/mem/glyph.v`, loaded by software via `z_gfx_hw_font_load()`),
and blits one glyph per trigger with solid foreground/background per
pixel (a true text-cell fill, not a transparent overlay) -- see
`BLIT_GLYPH_ADDR`/`_W`/`_H`/`FG_COLOR`/`BG_COLOR` in the memory map
above.

Glyph mode is unclipped by design -- the caller is responsible for
only triggering it for glyphs that are already fully on-screen (and
within any window clip rect), falling back to software rendering
otherwise. This keeps the hardware path simple at the cost of pushing
that one piece of judgment into the C driver.

### Copying a bitmap from main memory

Almost always via `sw/common/zgfx.h` rather than by hand:

```c
/* an offscreen 1bpp document, framebuffer bit order, 80 bytes/row */
static uint32_t doc[480][20];

/* show the region at (doc_x, doc_y) at screen (sx, sy) */
z_fb_hw_blit_mem(doc, sizeof(doc[0]), doc_x, doc_y, sx, sy, w, h);
```

Note the source bitmap must use the framebuffer's own bit order --
pixel x at bit `(x & 31)` of word `(x >> 5)`, least significant bit
leftmost, matching `z_fb_set_pixel()`. A bitmap stored MSB-first (the
convention used for glyphs, icons and fill patterns) will come out
mirrored in 32-pixel blocks.

`sw/apps/draw` is the worked example: a full-screen-sized document in
main memory, blitted into a window's canvas area, clipped by the app to
its viewport first because the hardware only clips to the screen.

### Bouncing Animation

```c
int main() {
    clear_screen();
    
    int x = 0, y = 0;
    int dx = 1, dy = 1;
    const int box_size = 10;
    
    while (1) {
        // Erase previous position
        fill_rect(x, y, box_size, box_size, PATTERN_BLACK);
        
        // Update position
        x += dx;
        y += dy;
        
        // Bounce off walls
        if (x <= 0 || x + box_size >= 640) dx = -dx;
        if (y <= 0 || y + box_size >= 480) dy = -dy;
        
        // Draw new position
        fill_rect(x, y, box_size, box_size, PATTERN_WHITE);
        
        delay();
    }
}
```

## Copy modes

With `CTRL_FILL` and `CTRL_GLYPH` both clear, the blitter copies a 1bpp
rectangle into the framebuffer. `CTRL_SRCMEM` picks where the source
comes from:

| CTRL_SRCMEM | Source | Read through | Use |
|---|---|---|---|
| 1 | Main memory | the blitter's main-bus master port | offscreen document buffers (`sw/apps/draw`), bitmaps loaded from disk |
| 0 | VRAM | the same port used for the destination | offscreen VRAM to on-screen VRAM, i.e. sprites |

Both share one engine. The shifter, the edge masking, the clipping and
the state machine are identical; the only difference is which port
issues the source read.

**VRAM-to-VRAM copy did not work before this.** It was a stub from the
day the module was written -- `ST_WRITE`'s copy branch wrote the
destination straight back unchanged -- and the `BLIT_SRC_X`/`BLIT_SRC_Y`
registers this document used to describe had long since been reassigned
to the glyph path. It works now as a side effect of building the
main-memory path properly. Note that it is only *useful* on a bitstream
whose VRAM is larger than the visible framebuffer: `rtl/mem/vram.v` is
currently exactly 640x480/32 words with nothing spare, so today there is
no offscreen region to keep a sprite in. Enlarging VRAM is what makes
this mode worth having.

### Source addressing

The source rectangle's position is **not** given as an x/y pair. The
hardware assembles each destination word from a two-word sliding window
over the source row, and what it needs to know is where that window
starts:

- `BLIT_SRC_ADDR` -- byte address (word aligned) of the source word the
  window starts on, for the first row.
- `BLIT_SRC_STRIDE` -- bytes per source row; added to `BLIT_SRC_ADDR`
  once per row.
- `BLIT_SRC_SHIFT` -- bits [4:0], the bit offset within that word of the
  pixel that lands on bit 0 of the first destination word. Bit [8]
  (`SRC_PRIME_ZERO`) starts the window with zeros instead of reading
  that word at all.

Software derives all three from the source x/y it actually has:

```c
int sbit0 = src_x - (dst_x & 31);   /* may be negative */
if (sbit0 >= 0) {
    sword = sbit0 >> 5; sshift = sbit0 & 31; prime = 0;
} else {
    sword = 0; sshift = sbit0 + 32; prime = SRC_PRIME_ZERO;
}
BLIT_SRC_ADDR   = src_base + src_y * stride + sword * 4;
BLIT_SRC_STRIDE = stride;
BLIT_SRC_SHIFT  = prime | sshift;
```

`sbit0` is the source-row bit index landing on bit 0 of the first
destination word -- that word begins at screen x = `dst_x & ~31`, which
is `dst_x & 31` pixels left of `dst_x`. It can go negative, but by at
most one word (`dst_x & 31` is at most 31 and `src_x` is at least 0),
which is the whole reason `SRC_PRIME_ZERO` exists: rather than reading
the word before the buffer, the window starts empty. Every pixel that
would have come from that phantom word sits left of `dst_x` and is
masked out of the destination anyway.

Don't write these by hand. `z_fb_hw_blit_mem()` and
`z_fb_hw_blit_vram()` (`sw/common/zgfx.h`) do the derivation, the
clamping and the IRQ masking.

### Source addresses are physical

`BLIT_SRC_ADDR` is a **physical** address. The blitter is its own bus
master and does not go through the MTU (`rtl/mtu.v`), which only
translates addresses the CPU issues. An app runs at virtual
`0x8000_0000`, so handing one of its pointers straight to the blitter
points it at whatever lives at *physical* `0x8000_0000` -- not that
app's data, and quite possibly not memory at all.

`z_fb_hw_blit_mem()` translates for you, by reading the MTU's own
translation base (`reg_mtu_base`, `sw/common/zeitlos.h`) and adding the
offset. A base of 0 means no translation is active and the address is
already physical.

### Two master ports, and the deadlock they could cause

A main-memory source needs a bus the blitter previously had no access
to. Its framebuffer master goes through `rtl/arbiter_vram.v`, whose
selected master is wired only to VRAM; main memory is on the main bus,
which the CPU owned outright (`sysctl.v` said as much: *"CPU controls
the main bus (will share with DMA controller)"*). So the blitter gained
a second, read-only master port, and `rtl/arbiter_main.v` puts it on
the main bus alongside the CPU.

The two arbiters are named for the bus each one owns:
`wb_arbiter_vram` (3 masters: CPU, rasterizer, blitter framebuffer
port) and `wb_arbiter_main` (2 masters: CPU, blitter source port). The
blitter is a master on both.

There is no simpler arrangement. Routing the existing port to both buses
would mean the VRAM arbiter's output driving the main bus, which the CPU
already drives -- the same arbitration problem one level down. The
alternative of merging all four masters onto a single system-wide
arbiter is a bigger change to a well-tested interconnect, not a smaller
one.

**The two ports are never asserted at the same time**, and that is a
correctness requirement, not tidiness. Both arbiters release a grant
only when the winning master drops `cyc`. A blitter holding VRAM while
waiting for main memory, against a CPU holding main memory while waiting
for VRAM, deadlocks the machine solid. The state machine keeps the ports
strictly alternating, and `rtl/gpu/bench/tb_memblit.v` asserts on every
cycle of every test that both are never high together.

`wb_arbiter_main` is round robin. Fixed priority fails badly in both
directions here: with the blitter on top, a long copy starves a CPU
trying to make progress on the same bus; with the CPU on top, a tight
load/store loop can keep a copy from ever finishing while the blitter
holds `BUSY`, so software polling that flag spins forever.

### Caveats

- **Overlapping source and destination are not handled** in VRAM-to-VRAM
  mode. The copy runs top-to-bottom, left-to-right with no direction
  selection, so an overlap where the destination trails the source in
  that order re-reads pixels the same operation already wrote.
- **The hardware may read one word past** the last source word it needs,
  whenever source and destination are not word-aligned to each other.
  Those bits are masked out of the result, but the read happens -- so a
  source buffer should not end exactly at the last valid byte of a
  mapping.
- **Clipping is to the screen only**, as with fill. A caller needing to
  clip to something smaller (a window's content area, a scrolling
  viewport) must narrow the rectangle itself and adjust `src_x`/`src_y`
  in step.
- **Older bitstreams silently do nothing useful.** A blitter predating
  this mode ignores `CTRL_SRCMEM` and takes the old copy stub, which
  writes the destination back unchanged -- a no-op, not an error.
  `z_fb_hw_blit_mem_available()` probes for the mode (it writes
  `CTRL_SRCMEM` without `CTRL_START` and reads it back) so an app can
  keep a software fallback; `sw/apps/draw` does exactly that.

### Verification

`rtl/gpu/bench/tb_memblit.v` checks every pixel of a 640x480 screen
against a reference model, over 28 cases: all 64 source/destination bit
alignment combinations, the `SRC_PRIME_ZERO` corner, shift-0, sub-word
and multi-word widths, right-edge screen clipping, a 500x40 copy, a
3-wait-state source memory, and six VRAM-to-VRAM copies. It also runs a
fill-mode regression.

```
$ iverilog -g2005 -o tb rtl/gpu/bench/tb_memblit.v rtl/gpu/gpu_blit.v
$ vvp tb
```

One detail in that testbench is worth knowing before you modify it. The
two slave models behave **differently on purpose**, because the real
ones do. `rtl/mem/vram.v` acks on every cycle `cyc && stb` are high,
with no one-ack-per-transaction guard, and the pre-existing fill path
depends on that: it holds `cyc` asserted across its read and its write,
so a stricter slave acks the read twice and swallows the write entirely.
Main memory (`rtl/mem/sram.v`, `rtl/mem/sdram_kianv.v`) *is* strict.
Modelling VRAM strictly makes the testbench report fill-mode failures
that hardware does not have.

## Performance Characteristics

### Optimized Operations

- **Word-aligned fills**: Fastest performance
- **Screen clears**: ~1,200 memory operations
- **Large rectangles**: Word-level efficiency
- **Clipping**: Minimal overhead (~5-10%)

### Performance by Object Size

| Object Size | Memory Operations | Performance |
|-------------|-------------------|-------------|
| 1×1 pixel | ~2-4 | Excellent |
| 5×8 character | ~8-16 | Excellent |
| 10×10 box | ~20-40 | Very Good |
| 32×32 square | ~64-128 | Excellent |
| Full screen | ~1,200 | Blazing Fast |

## Clipping Behavior

### Automatic Clipping

When `CTRL_CLIP` is enabled, the blitter automatically:
- Clips rectangles to screen boundaries (0,0 to 639,479)
- Handles negative coordinates safely
- Prevents buffer overruns
- Uses pixel-level masks for partial word operations

**Important:** clipping only happens when `CTRL_CLIP` is set. With it
unset, coordinates are used as-is with no bounds checking, and an
out-of-range destination can hang the blitter's hardware state
machine (it will wait indefinitely for a bus ack that never comes).
Only skip `CTRL_CLIP` for coordinates you can prove are in-range by
construction (e.g. a fixed, compile-time-constant full-screen clear).
`z_fb_hw_fill_rect()` (see "Best Practices") clamps coordinates to
the real screen bounds unconditionally, regardless of `CTRL_CLIP`,
and removes the need to make that judgment call at all.

### Examples

```c
// These are all safe with clipping enabled:
fill_rect(-10, -10, 50, 50, PATTERN_WHITE);      // Clips to screen
fill_rect(500, 300, 100, 100, PATTERN_WHITE);    // Clips to screen
fill_rect(256, 192, 1000, 1000, PATTERN_WHITE);  // Clips to screen
```

## Pattern Values

The `BLIT_PATTERN` register accepts any 32-bit value, and it is written
into each destination word (masked at the edges). That makes it a
horizontal 32-pixel pattern which repeats identically on every row:

```c
#define PATTERN_BLACK   0x00000000  // All pixels off
#define PATTERN_WHITE   0xFFFFFFFF  // All pixels on
#define PATTERN_CHECKER 0xAAAAAAAA  // Alternating pixels
#define PATTERN_DOTS    0x11111111  // Sparse dots
```

For a pattern that varies **vertically** -- which is what most useful
textures do -- the fill has to be issued per row of the pattern, with
`BLIT_PATTERN` reloaded each time. `z_fb_hw_fill_pattern()`
(`sw/common/zgfx.h`) does this: it takes an 8-byte, row-per-byte,
MSB-first pattern (the same convention as `z_font_t` glyphs and
`zicon.h` icons) and issues one blitter operation **per run of rows
sharing a pattern byte** -- so a solid or vertically-uniform pattern
still costs a single operation, while an alternating one costs up to
`h`.

Two details of that worth knowing:

- The 8-bit row is replicated across all four bytes of the word. Since
  32 is a multiple of 8, the pattern is thereby anchored to the
  **screen's** 8-pixel grid, not to the rectangle's own corner. Two
  adjacent rects filled with the same pattern tile seamlessly into each
  other, which is what makes a pattern read as a texture the shapes are
  cut out of.
- Framebuffer words put the leftmost pixel in the **least** significant
  bit (`z_fb_set_pixel()`), while pattern and glyph bytes are MSB-first.
  `z_fb_hw_fill_pattern()` reverses each row byte on the way in. Getting
  this wrong is an unpleasant bug to chase: every symmetric pattern
  (solid, 50% checker, horizontal lines) looks perfectly correct, and
  only the asymmetric ones come out mirrored -- as a plausible-looking
  diagonal leaning the wrong way.

`Z_PATTERN_COUNT` ready-made patterns live in `z_pattern_table[]`
(`sw/common/zwidget.h`), ordered light to dark then lines and textures.
`sw/apps/draw` uses them for both its palette swatches and its fills.

## Error Handling

- **Out-of-bounds coordinates**: Safely clipped -- *only if
  `CTRL_CLIP` is set*. Otherwise unchecked; see "Clipping Behavior"
  above.
- **Zero dimensions**: Operation completes immediately.
- **Negative coordinates**: Handled by clipping -- same `CTRL_CLIP`
  caveat as above.
- **Concurrent register access**: Not arbitrated between processes at
  the register level -- two processes writing their own
  dst_x/dst_y/etc. and triggering a blit could interleave and corrupt
  both operations. Use the `sw/common/zgfx.c` helpers listed under
  "Best Practices" below -- all of them go through
  `gpu_blit_acquire()`, which serializes access safely across
  processes. This applies to copy mode exactly as much as to fill and
  glyph: a copy latches source address, stride and shift across several
  register writes, so an interleaved second operation produces a blit
  reading from a mix of two callers' parameters.

## Best Practices

**Use `sw/common/zgfx.c` rather than the raw register sequences below.**

| Want | Call |
|---|---|
| solid fill | `z_fb_hw_fill_rect()` |
| 8x8 patterned fill | `z_fb_hw_fill_pattern()` |
| copy from main memory | `z_fb_hw_blit_mem()` |
| copy within VRAM | `z_fb_hw_blit_vram()` |
| text / icons | `z_fb_draw_char()`, `z_fb_draw_char2()`, `z_fb_draw_icon()` |

All of them do everything "Always Wait for Completion" and "Use Clipping
for Safety" describe, plus the things raw register access can't:
coordinates clamped to the actual screen bounds *unconditionally* (not
just when `CTRL_CLIP` is requested), and the register-writes-then-trigger
sequence protected against interleaving with another process's own
writes via `gpu_blit_acquire()`. The copy helpers additionally derive
the source word/shift/prime encoding and, for `z_fb_hw_blit_mem()`,
translate the source pointer to a physical address.

The raw-register examples below are kept for reference and for
understanding the hardware, not as the recommended way to drive it.

### 1. Always Wait for Completion
```c
// Wrong - may corrupt operations
fill_rect(0, 0, 10, 10, PATTERN_WHITE);
fill_rect(20, 20, 10, 10, PATTERN_WHITE);

// Correct
fill_rect(0, 0, 10, 10, PATTERN_WHITE);
wait_blitter_done();
fill_rect(20, 20, 10, 10, PATTERN_WHITE);
```

### 2. Use Clipping for Safety
```c
// Recommended for user input or dynamic coordinates
BLIT_CTRL = CTRL_START | CTRL_FILL | CTRL_CLIP;

// Skipping CTRL_CLIP is NOT just "slightly faster with a bit less
// safety" -- the unclipped path has no bounds check of any kind, and
// an out-of-range destination can hang the hardware state machine
// forever. Only skip this for coordinates you can prove are always
// in-range by construction (a fixed, compile-time-constant
// full-screen clear, say) -- and even then, z_fb_hw_fill_rect()
// (which clamps regardless of CTRL_CLIP) removes the need to make
// that judgment call at all.
BLIT_CTRL = CTRL_START | CTRL_FILL;
```

### 3. Optimize Common Operations
```c
// Fast screen clear
void clear_screen_fast(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = 640;
    BLIT_HEIGHT = 480;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping overhead
    wait_blitter_done();
}
```

## Troubleshooting

### Operation Not Starting
- Check if previous operation completed (`BLIT_STATUS & 1 == 0`)
- Ensure `CTRL_START` bit is set in control register
- Verify register addresses are correct

### Unexpected Results
- Check coordinate bounds (0-639 for X, 0-479 for Y)
- Verify pattern value (0x00000000 for black, 0xFFFFFFFF for white)
- Ensure width and height are non-zero

### Performance Issues
- Use clipping sparingly for maximum performance
- Batch operations when possible
- Consider object size and alignment

## Technical Details

### Hardware Architecture
- Word-level operations with pixel-precise masking
- Automatic stride calculation for framebuffer addressing
- A single state machine shared by fill, copy and glyph modes

There is **no** line generation in this module -- that is the separate
line rasterizer, `rtl/gpu/gpu_raster.v` (`docs/gpu_raster.md`), reached
from C via `z_fb_hw_line()`/`z_fb_hw_box()`. An earlier revision of this
document credited the blitter with Bresenham line drawing; it has never
had any.

### Memory Interface

**Two Wishbone master ports**, both 32 bits wide:

| Port | Bus | Used by |
|---|---|---|
| `m_*` | VRAM, via `rtl/arbiter_vram.v` | all destination reads and writes; also source reads in VRAM-to-VRAM copy |
| `s_*` | main bus, via `rtl/arbiter_main.v` | source reads in main-memory copy only; idle otherwise |

Single transactions, not bursts -- one word per handshake. The two ports
are never asserted simultaneously; see "Two master ports, and the
deadlock they could cause" above for why that is a correctness
requirement and not just a simplification.

Bounds checking is **conditional**, not automatic: it happens only when
`CTRL_CLIP` is set. See "Clipping Behavior".

## Historical Notes

The following is background on the module's development history. It
isn't needed to use the blitter, but may help if you're debugging
something that touches this RTL.

### Bugs found (and fixed) during hardware bring-up

All three were subtle and easy to reintroduce, so they're documented
here in detail:

1. **Clipped fills read undefined data.** The clipped-fill path used
   to skip straight from `ST_CLIP` to `ST_WRITE`, but `ST_WRITE`'s
   partial-word masking (`(read_data & ~left_mask) | ...`) depends on
   `read_data`, which was never populated -- `ST_READ` was only
   reached by the *unclipped* path. Any clipped rectangle fill not
   landing on exact 32-pixel word boundaries (i.e. almost any
   real-world window-sized rectangle) masked against garbage. Fixed
   by routing clipped fills through `ST_READ` like copy mode always
   did.
2. **Control-bit latching race.** `glyph_reg`/`fill_reg`/
   `clip_enable_reg` are set by the Wishbone-slave `always` block, but
   were latched into `work_glyph`/`work_fill`/`work_clip` by the
   *separate* state-machine `always` block on the same clock edge as
   the triggering write -- Verilog evaluates all RHS expressions
   across blocks using pre-edge values, so the trigger latched
   whatever these registers held *before* the write, not the value
   being written. In practice this only mis-fired on the very first
   glyph trigger ever issued (after that, `glyph_reg` stays `1` and
   the staleness is invisible). Fixed by latching directly from
   `wb_dat_i` (the value actually being written this cycle) instead.
3. **Glyph fetch read one cycle too early.** `ST_GLYPH_FETCH` sets
   `glyph_addr_o`, which only becomes visible to `glyph_mem`'s
   `blit_addr` input the *following* cycle -- and `glyph_mem`'s own
   port B is itself a registered (1-cycle-latency) BRAM read, so
   `glyph_data_i` doesn't actually reflect the requested address until
   two cycles after `ST_GLYPH_FETCH`, not one. `ST_GLYPH_FETCH_WAIT`
   sampled `glyph_data_i` a cycle early, so every row's write used the
   *previous* row's glyph byte, and each glyph's true last row was
   fetched but never written anywhere. Symptoms on real hardware:
   every character rendered shifted down one pixel row, with row 0 of
   each glyph showing whatever ink the *previous* glyph's last row
   happened to have (visible as stray pixels bleeding into otherwise-
   blank cells, e.g. spaces right after a character with descenders),
   and the glyph's genuine bottom row never appearing at all. This is
   almost certainly the real explanation for `z_font_5x7`'s "bottom
   row cut off" symptom noted in `zfont.h`/`term.c` -- switching to
   `z_font_5x8` didn't fix the root cause, it just added a blank
   padding row so the row this bug drops is normally blank anyway,
   which is also why the top-row contamination was easy to miss (most
   glyphs' last row is ink-bearing, but plenty of transitions still
   line up to show something). Fixed by adding a second wait state,
   `ST_GLYPH_FETCH_WAIT2`, so the byte is captured two cycles after
   the address is presented instead of one. Confirmed by simulation
   (a testbench instantiating the real `gpu_blit_wb` + `glyph_mem`
   against a bus model matching `rtl/mem/vram.v`'s actual ack timing):
   the original RTL produced exactly the predicted row-shifted,
   previous-glyph-contaminated output; the fix produces the correct
   glyph bit-for-bit. The straddling (word-crossing) split logic
   itself was verified separately and was never at fault -- a
   single-row straddle test passes identically on both the buggy and
   fixed RTL, isolating the bug to the row-to-row fetch timing only.
   Worth revisiting whether `z_font_5x7` (rather than the `z_font_5x8`
   padding-row workaround) now renders correctly with this fix in.

### Concurrent register access race (fixed by `gpu_blit_acquire()`)

Both fill mode (`z_fb_hw_fill_rect()`) and glyph mode
(`z_fb_draw_char()`/`z_fb_draw_char2()`/`z_fb_draw_icon()`) already
wrapped their own register-writes-then-trigger sequence in
`maskirq()`, so two processes' register writes themselves couldn't
interleave. What wasn't covered, until `gpu_blit_acquire()`
(`sw/common/zgfx.c`) was added: every one of those callers checked
"is the hardware idle?" with IRQs still enabled, *then separately*
masked IRQs before its own writes+trigger -- a real gap between the
check and the mask. A timer IRQ landing in that gap could switch to a
different process, which observes the same "idle" state, wins the
race, and starts (and fully masks) its own operation; when the
original process resumes and finally masks IRQs, the hardware is no
longer idle, but nothing re-checked that -- its own START trigger
lands while `draw_busy` is still 1 and the state machine isn't in
`ST_IDLE`, and is silently dropped (`start_trigger`'s `!draw_busy`
gate is only evaluated inside the `ST_IDLE` case -- see
`rtl/gpu/gpu_blit.v`). No error, no effect, and the caller had no way
to know its operation never happened. `gpu_blit_acquire()` closes this
by folding the busy-check into the *same* masked section as the
trigger (re-checking under the mask, unmasking and retrying if it
lost the race) -- used by all four callers above. See its own (long)
comment in `zgfx.c`, and `docs/window_manager.md`'s "Known
limitations" entry on `term`'s horizontal-garbage-near-typed-text
report, for the reasoning behind why this was a strong candidate for
that bug specifically (not confirmed on real hardware).

### Text rendering before glyph blit mode existed

Hardware glyph blit mode (see "Text Rendering" above) didn't always
exist. Earlier revisions of this document showed a hand-drawn-boxes
placeholder approach for rendering text with the blitter, written
before glyph blit mode was implemented. That approach is no longer
relevant; all current text rendering uses the real glyph blit mode
described above, or software rendering as a fallback.

## Raster operations

`gpu_blit.v` CTRL bits **6:5** select how the source word combines with
what is already in the destination.

| | | |
|---|---|---|
| `Z_ROP_COPY` | 0 | `dst = src` |
| `Z_ROP_OR` | 1 | `dst = dst \| src` |
| `Z_ROP_XOR` | 2 | `dst = dst ^ src` |
| `Z_ROP_ANDN` | 3 | `dst = dst & ~src` |

Applies to fill and to both kinds of copy. **Ignored in glyph mode**,
which has its own write path with its own fg/bg/cell merge and no use
for these.

`ROP_COPY` is 0 so every existing caller — which writes neither bit —
keeps behaving exactly as before. That is not politeness: `gpu_blit` has
shipped, and a bitstream where an old binary's blits suddenly XOR would
be a very confusing thing to debug.

### Why ANDN rather than AND

The operation sprites need is "punch a hole shaped like this mask". With
plain AND the caller would have to store an inverted copy of every mask.
ANDN costs the same single LUT input and saves the sprite sheet from
carrying both polarities.

### Masked sprites

```c
z_fb_hw_blit_sprite(data, mask, stride, sx, sy, dx, dy, w, h);
```

Two passes over the same rectangle: **ANDN the mask** to clear exactly
the pixels the sprite will occupy, then **OR the data** to lay them in.
Order matters — OR first would light pixels the mask pass then clears
again, leaving a sprite-shaped hole instead of a sprite.

Two passes and not one because a single-pass masked blit needs the
hardware to fetch two source streams at once: a second source port and a
second shifter. Two passes reuse everything and cost one extra
read-modify-write per word.

**Not atomic.** Between the passes the sprite's footprint is momentarily
blank, so a sprite drawn into the *visible* page can tear into a
one-frame hole. Draw into the back page and flip — which a game is doing
anyway.

### The correctness hazard: blind fills

Any op other than COPY needs to know what is already in the
destination. A copy always reads it, and a *clipped* fill already reads
it to preserve bits outside the rect — but an **unclipped fill** went
straight to a blind `ST_WRITE`.

With a non-COPY op that would merge against whatever `read_data` held
from the *previous* blit: wrong in a way that depends on what the
blitter did last, which is about the worst debugging experience
available. `rop_needs_read` forces the read. An unclipped COPY fill
still skips it, exactly as before.

### Timing

Two wires, `rop_fill` and `rop_copy`, rather than one fed by a
`fill ? pattern : src` mux — deliberately.

Per bit each is a function of exactly four things: the destination bit,
the source bit, and the two op bits. That is **one LUT4** on ECP5.
`ST_WRITE` then merges with the edge mask, three inputs, one more LUT4.

So this adds **one logic level** to a path that was already
register-to-register (`read_data` is captured in `ST_WAIT_READ`,
`m_dat_o` is a register), on a write path nowhere near the 48MHz
critical path to begin with.

Folding the fill/copy choice into a single wire would make it six inputs
and two levels *before* the merge, for no gain — `ST_WRITE` already
branches on `work_fill`, so the selection is free there.

Cost is roughly 64 extra LUT4 (two 32-bit op networks) plus a 2-bit
register, and no BRAM.

### Detecting support

`z_fb_hw_rop_available()` writes the ROP field with no start bit and
reads it back, the same probe `z_fb_hw_blit_mem_available()` uses.

Worth having, because the failure on an older bitstream is nastier than
usual: the bits are simply ignored, so every op behaves as COPY, and a
masked sprite draws its mask as a solid block with the data over the top
— an **opaque box**, which looks like bad art rather than a missing
hardware feature.

### Testing

`rtl/gpu/bench/tb_rop.v`, self-checking, passes. Covers every op in fill
and memory-copy mode against an independently-computed model; the
COPY-is-unchanged regression; the blind-fill hazard explicitly (it runs
a blit that leaves a known value in `read_data`, then an unclipped OR
fill elsewhere, and distinguishes `dst|src` from `dst|stale`); the
masked-sprite recipe end to end including untouched neighbours; and the
CTRL readback the software probe depends on.

`tb_memblit.v` still passes unchanged.

```
iverilog -g2005 -o /tmp/tb_rop.out \
    rtl/gpu/bench/tb_rop.v rtl/gpu/gpu_blit.v
vvp /tmp/tb_rop.out
```

## Asynchronous blits

Every blit function returns only once the blitter has finished. The
`_async` variants return as soon as the operation has been *started*,
giving the caller the blit duration back to do something else with.

### Why this needed almost no new machinery

`gpu_blit_acquire()` already waits for the blitter to go idle **before**
starting anything, and re-checks with interrupts masked — that is what
closed the cross-process race documented above it in `zgfx.c`.

So the trailing wait is not what makes back-to-back blits correct. The
*leading* wait in the next one is. Dropping the trailing wait changes
when the caller regains control and nothing else: a sequence of async
blits is exactly as correct as a sequence of synchronous ones, including
against another process competing for the same peripheral.

### The one rule

An async blit is still running when the call returns. Anything touching
the affected pixels **with the CPU** — `z_fb_get_pixel()`, a software
sprite path, a direct VRAM poke — must call `z_fb_hw_sync()` first.
Another *blit* is fine; only CPU access needs the barrier.

`gamedemo` needs this in two places, and both are the kind of thing that
would otherwise fail once a year on a loaded system rather than in
testing:

- before `z_game_flip()`, or the frame shown could be one blit short
- before the software sprite fallback, which reads VRAM to composite

### How much it buys

Nothing if the caller immediately blocks. The gain is exactly the CPU
work done between starting a blit and needing it finished, capped at the
blit's duration.

So the useful shape is: start the blit for item N, prepare item N+1,
start its blit (which waits for N as a side effect), and so on. In
`gamedemo`'s tile loop the per-tile work — indexing the map, folding the
world x into the page, computing the destination — is roughly 20 cycles
against a ~120 cycle blit, so it overlaps almost entirely.

The synchronous functions are unchanged and remain the default. This is
opt-in on purpose: the callers that benefit are the ones with real work
to overlap, and the ones that don't would only gain a new way to get it
wrong.

### Masked sprites are only half async

`z_fb_hw_blit_sprite_async()` is two blits internally, so it necessarily
waits for the ANDN pass before starting the OR pass — only the second is
left running when it returns.

That is the strongest remaining argument for a **single-pass cookie-cut
mode**: not the raw cycle count, but that a one-pass sprite is fully
asynchronous and atomic, where a two-pass one can be neither.

## Single-pass masked sprites (cookie cut)

CTRL bit **7**. Fetches two source streams and combines them with the
destination in one operation:

```
dst = (A & B) | (~A & dst)
```

A is the mask (`gpu_blit_src_addr`, 1 where the sprite is opaque), B is
the data (`gpu_blit_src_b_addr`). The ROP field is ignored while this is
set — the combine *is* the op.

`z_fb_hw_blit_sprite()` uses it automatically where present and falls
back to ANDN-then-OR where not, so most callers never need to know.

### Measured

| | cycles, 16×16 |
|---|---|
| two-pass ANDN + OR | 544 |
| one-pass cookie cut | **400** (73%) |

Four bus transactions per destination word instead of six, and one
register setup instead of two.

### The structural wins matter more than the speed

**Atomic.** Between two passes the sprite's footprint is momentarily
blank, so a two-pass sprite can only safely be drawn into a back buffer.
One pass can go straight onto the visible page.

**Fully asynchronous.** `z_fb_hw_blit_sprite_async()` could only ever
leave the *second* pass running, because starting it had to wait for the
first. One pass gives the caller the whole blit duration back.

### Why it needs no second shifter

This is what makes it cheap, and it's the one design decision worth
recording.

A and B are read on **different cycles**, so one barrel shifter serves
both as long as its output is latched per stream — which
`ST_MEM_READ_WAIT` now does, selecting on `mem_stream`. The Amiga
blitter needed two shifters only because its A and B could be
*independently positioned*; here they are always the same sprite
geometry, so same stride, same `src_x`/`src_y`, therefore the same
shift.

The shifter is by far the most expensive thing in the source path
(~64–96 LUT4). Sharing it takes the cost of this feature down to a
duplicate of the row/word walk registers — roughly 160 flops — plus one
LUT4 per bit for the combine and one more for the mux against the ROP
result.

No second bus port and no new arbiter client either: A and B share the
`s` port with alternating reads, which is free because the walk was
already sequential.

### What it deliberately is not

Not a general minterm unit. The Amiga took an 8-bit function select over
A, B and C, which is an 8:1 mux per bit — roughly 112 LUT4 and two logic
levels, against this fixed function's one LUT4 and one level. Cookie-cut
is the only minterm anyone actually reaches for, and the 2-bit ROP field
already covers XOR fills and the rest.

### Timing

Three logic levels in the write merge: the combine (3 inputs), the mux
against `rop_copy` (3 inputs), then the edge-mask merge (3 inputs). Up
from two.

The path is register-to-register — `read_data` is captured in
`ST_WAIT_READ`, `m_dat_o` is a register — and the blitter's write path
is nowhere near the 48MHz critical path. **COPY blits are unchanged**: a
320×240 unclipped fill still measures 9,624 cycles, so the OS fill path
pays nothing.

### Testing

`rtl/gpu/bench/tb_rop.v` covers it, and the cases were chosen for what
they'd catch rather than for coverage:

- one pass equals two, against an independently computed model rather
  than against the two-pass result, so a shared bug still fails
- **multi-row** — B has its own row pointer, and a stride advance
  applied to A but not B would show up only from row 1 onward, which a
  single-row test passes regardless
- **unaligned** — both streams must take the same shift from the one
  shared shifter; a stream holding a stale shifted value corrupts only
  this case
- neighbouring words untouched
- the CTRL readback the software probe depends on

`tb_memblit.v` still passes unchanged.

Two bugs this caught during development, both from patches that silently
failed to apply: `mem_stream` never being initialised (X propagation
through the shared shifter, which broke plain memory copies), and
`ST_MEM_READ_WAIT` keeping its single-stream behaviour. Worth noting
that the first showed up as *existing* tests failing, which is exactly
what the regression cases are for.

## Shaded fills (ordered dither)

CTRL bit **8**. In a fill with this set, `PATTERN` is not a bit pattern
— its low five bits are a **grey level**, 0 to 16, and the hardware
generates a 4x4 ordered dither for it.

`z_fb_hw_fill_shade(x, y, w, h, level)`.

### Why a level, not a pattern table

The obvious design is four pattern registers indexed by `y & 3`. That
needs four new registers — the file is already at 16, so it would need a
wider address too — and leaves every caller to compute dither matrices.

A level needs **no new register at all**: `PATTERN` is already there and
is meaningless as a bitmask in this mode. The Bayer matrix is a
constant, so the four row patterns are a combinational function of five
bits. No storage whatsoever, roughly one LUT per bit.

### Screen-aligned by construction

This is the property that makes patterns useful rather than decorative,
and here it comes for free.

The dither **row** is the destination's absolute framebuffer row. The
dither **column** is the bit's position within the 32-bit word — which,
because the framebuffer is a flat bitmap at 32 pixels per word, *is* the
absolute screen column mod 4. Neither is relative to the rectangle being
filled, and no offset is ever added.

So two adjacent fills share one continuous pattern with no seam, and a
region redrawn at a different rectangle offset comes out identical. A
rectangle-relative pattern gets both wrong: seams where fills meet, and
a shimmer whenever anything scrolls.

`tb_rop.v` tests this specifically, because **no single-rectangle test
can catch it** — every other dither check passes just as well with a
rectangle-relative pattern. The alignment test fills a 64-pixel span as
one rectangle, then as two adjacent rectangles, and requires the results
to be bit-identical.

### What it is for

Greys, on a display that has none. Seventeen stable levels from one
5-bit value:

- flat-shaded 3D — a shaded **span is one fill**, which is what lets a
  software triangle rasterizer emit flat-shaded polygons with no
  triangle hardware at all
- disabled controls, scrollbar troughs, selection washes
- chart and histogram shading
- textured backgrounds drawn as one fill rather than a blit per tile —
  the case that prompted this, after `gamedemo`'s ground went from 63
  tile blits to one fill and lost its texture

### Cost

No storage. The Bayer matrix is a constant function of two row bits and
two column bits, and the column is fixed per bit lane, so each bit is
about one LUT4. It sits where `PATTERN` already fed the raster op, so
everything downstream — the ROP, the edge-mask merge, clipping —
is unchanged and a dithered fill composes exactly like any other.

`COPY` fills measure identically to before.

### Testing

`tb_rop.v`: level 0 is exactly black and level 16 exactly solid (both
ends must be reachable or "off" and "on" are not); a mid level checked
bit by bit against the matrix; and the split-fill alignment test above.
`tb_memblit.v` still passes.

## Hardware vertical scroll

`z_fb_hw_scroll(x, y, w, h, dy)` moves a rectangle's contents up or
down. Negative `dy` moves content **up** (scrolling forward through a
document); positive moves it **down** (scrolling back). The strip that
scrolls in is not touched — the caller redraws it.

For an 80x25 terminal: **1.07ms against 4.67ms**, about 4.4x. Not a
throughput win — glyph drawing is around 1% of the CPU either way — but
it removes a perceptible hitch every time output scrolls.

### Both directions work, and one of them is not obvious

The blitter walks rows top-down and copies through VRAM, so an
overlapping copy can overwrite a source row before reading it.

**Content moving up** puts the destination above the source, so every
row is read before anything overwrites it. One blit.

**Content moving down** puts the destination below the source, and a
single blit would destroy rows it has not read yet. The obvious
conclusion is that scrolling back cannot be accelerated — and that is
wrong. It is done as a series of blits exactly `dy` deep, issued
**bottom to top**: within one strip source and destination cannot
overlap, and working upward means each strip is read before the strip
below it is written.

The cost is one blit per strip rather than one overall — 24 instead of 1
for a 25-line screen scrolling by one line. Same pixels, so the same
blitter time; only the per-blit setup multiplies, and it is still
several times faster than re-rendering.

### A bug worth recording

The topmost strip is partly off the region, and the first version
clamped its **source** without advancing its **destination**. That lost
exactly one row per scroll, at the top.

It looked correct for `dy == 1`, where the skip is zero, and failed for
every larger step — the kind of thing that survives a quick test of the
common case and shows up later as a mysterious duplicated line.

Verified against a row-level model that also flags any read of an
already-overwritten row: seven cases, both directions, `dy` from 1 to
24, all correct with no overwrite-before-read.

### Wired into term, text and read

All three had a full re-render on every scroll. Each needed a different
integration, and the differences are the interesting part.

**`term`** — the shadow grid made this nearly free. A scroll changes
every cell in the model, so the shadow compare would find them all
different and redraw the screen. Blitting the surviving pixels up *and
shifting the shadow by the same amount* makes the compare find them
matching, so it draws only the rows that genuinely changed. One blit
plus one row of glyphs instead of twenty-five.

The blit and the shift must agree exactly. If they disagree the compare
concludes rows match when they do not, and the terminal shows stale
text with nothing to indicate it.

`zvt100` gained a `scrolls` counter and `vt_take_scrolls()`. A count,
not a flag: several lines can scroll between two renders, and the
renderer needs the total. "Take" because reading clears it — leaving
the count for a second caller would shift the screen twice.

**`text`** — a `scroll_repaint()` kept deliberately separate from
`repaint()`. `repaint()` runs on `Z_WM_REDRAW` after wm has cleared the
region, so it must not assume anything about what is on screen, which
is exactly what scrolling does assume. Conflating them would blit
whatever was underneath the window into the text area.

It also falls back to a full repaint when a selection exists:
`draw_row()` renders selected runs inverted, so moving old pixels would
carry stale highlighting with them.

**`read`** — the hard one, and only half accelerated. Its display lines
are **not a uniform height**, since headings use a larger font, so "n
lines" is not a fixed pixel shift. The shift has to be measured from
the layout, and only lines already laid out can be measured — the ones
scrolling off the top, not the ones scrolling in from above.

So forward scrolling measures `vlines[n].y` before scrolling and blits
by exactly that, while backward keeps the full repaint. Forward is
overwhelmingly the common direction in a reader, and half the win with
no risk of shifting by the wrong amount beats laying the incoming lines
out twice to find the number.

### Debugging the scroll offset

On hardware the scroll blit lands horizontally offset by roughly the
window's own x origin — content from outside the window appearing
inside it — in all three apps, and only on hardware.

**The RTL is not the cause.** `rtl/gpu/bench/tb_vscroll.v` performs
VRAM-to-VRAM copies at x = 32, 44, 12 and 63, single-row and
multi-row, replicating exactly what `z_fb_hw_blit_vram()` programs.
Every pixel matches. So the fault is in the coordinates handed over,
not in what the blitter does with them.

`z_fb_scroll_debug` and `z_fb_scroll_dbg_armed` trace both ends of that
handover: what the app computes, and what `z_fb_hw_scroll()` passes on.
Bounded counters rather than a flag, because scrolls happen at
key-repeat rates and an unbounded print would flood the console and
change the timing being investigated.

`text` currently prints the first six scrolls: its own `delta`, the
content rect, `text_w`, and the resulting blit arguments. The useful
comparison is against `fill_content(0, ...)`, which draws the same text
area and resolves x to `c.x0 + 0` — anything the blit does differently
from that is the bug.

`term` and `read` were kept disabled while this was narrowed down, so
only one app was moving. (All three are re-enabled now -- see "The
fix: a stale ack, not a wrong address" at the end of this file.)

#### What the trace showed

```
text scroll: delta=1 top=1 drawn=0 rows=25 LINE_H=9
  content rect: x0=46 y0=53 x1=361 y1=277  (w=316 h=225)
  text_w=303 TEXT_X0=2  -> blit x=46 w=303
zgfx scroll: x=46 y=53 w=303 h=225 dy=-9
  blit_vram sx=46 sy=62 dx=46 dy=53 w=303 h=216
```

**The coordinates are correct.** Source x equals destination x equals
the content rect's x0. The source row is exactly one line below the
destination row. The width matches the text area. Nothing here explains
a sideways shift, which rules out the app and `z_fb_hw_scroll()` and
leaves the copy itself.

#### The bisection

`z_fb_scroll_align` forces the region onto 32-pixel boundaries, so the
copy is word-for-word with no bit shifting at all. It is **ON by
default** in the debug build.

(It was briefly a Ctrl-F toggle, which was a bad idea twice over: the
key never reached `handle_key`, and in an editor nearly every plain key
inserts text, so there was no good key to spare. A build-time default
asks the question just as well.)

A few pixels at each edge are then not scrolled and will be wrong. That
is expected and is not the thing to look at. The thing to look at is
whether the **sideways offset** disappears:

- **offset gone** → the fault is in the unaligned source path, where
  `blit_copy_setup()` computes a sub-word shift
- **offset remains** → alignment is innocent, and the fault is in the
  VRAM source addressing itself

#### The answer: neither

With the copy forced word-aligned, hardware reported:

```
zgfx scroll: x=64 y=53 w=256 h=225 dy=-9
  blit_vram sx=64 sy=62 dx=64 dy=53 w=256 h=216
  setup: sx=64 dx=64 sbit0=64 sword=2 sshift=0 prime=0
  setup: src_addr=+4968 -> row 62 word 2 (want row 62 word 2)
```

`sshift=0` and `prime=0` — no bit shifting at all — and the derived
source address lands exactly on the row and word it wants. **And the
content was still pushed sideways.**

So every software-side hypothesis is eliminated:

| suspect | verdict |
|---|---|
| app coordinates | traced, correct |
| `z_fb_hw_scroll()` arguments | traced, correct |
| `blit_copy_setup()` addressing | traced, self-checked, correct |
| the unaligned/shifter path | bypassed entirely, fault persists |
| the RTL copy in isolation | `tb_vscroll.v`, correct at these alignments |
| `vram.v` timing | presents data and ack together, as modelled |
| `arbiter_vram` | registers data and ack together, consistent |

#### The unscrolled strip

Reported from hardware: *"the scrolling occurred about 20-30 pixels
inside the edge, content between the frame and that point doesn't
scroll at all."*

In the **aligned** build that is fully explained and expected. The
region starts at x = 46 and gets rounded up to x = 64, so exactly 18
pixels at the left are outside the copy and keep their old content. The
bisection tool was documented as leaving edge pixels unscrolled; what
was not anticipated is how it LOOKS. The boundary between scrolled and
unscrolled content is a hard vertical seam, and a seam reads as a line
-- which is why it was first described as the window frame appearing
inside the window.

That reframes the original report. The symptom is not content shifted
sideways; it is **a left-hand strip that does not scroll**, with a seam
at its right edge. In the unaligned build that strip was 40-50 pixels,
which is wider than any rounding explains and is the thing still to
account for.

Worth recording because the two descriptions suggest completely
different faults: "shifted right" points at addressing, "a strip that
does not move" points at the edge masking of the first destination
word. Two rounds were spent on the first reading.

#### What the corrupted text actually shows

Expected on screen:

```
| the dock. It gives you a text console ...
```

Actually displayed:

```
| the   | the It gives you a text console ...
```

The **start of the row** reappears where `dock.` should be. This is not
a shift and never was: part-way through the copy, the source read
returns data from the ROW ORIGIN instead of from the word it asked for.
Everything after that point is correct again.

That also explains the earlier "unscrolled left strip" and "content
pushed right" descriptions -- both are how a row-start duplication
looks depending on where the eye lands.

Doubling the blit changed nothing, so it is not a settle or
first-transaction effect.

#### Why simulation keeps passing

`tb_vscroll.v` seeds each source word distinctly and compares pixel by
pixel; `tb_edge.v` seeds source all-ones against destination all-zeros
so a skipped word cannot hide. Both pass. The RTL, driven alone,
addresses its source correctly.

So the fault is environmental -- something about the real VRAM path
that no bench here reproduces. A VRAM-to-VRAM copy is the only
operation that issues source read, destination read and destination
write back to back on that one port, and the only one where the source
travels through `arbiter_vram` alongside the CPU.

#### Next step: a source-read probe

Inference has been exhausted -- every static hypothesis was eliminated
by measurement, and the remaining one cannot be seen from software.

`rtl/audio_mixer.v` already has exactly the right pattern for this:
`MIXDBG_ADR`/`MIXDBG_DAT` latch the address and data of the mixer's
last fetch, so software can read the same address itself and compare.
If they differ, the fault is in the bus path rather than in the logic.

The blitter needs the same two registers on its source read. That turns
this from an argument into a measurement: dump the address the blitter
actually presented and the data it actually received for the first few
words of a scroll, and compare against what the copy was asked for.
About twenty lines of RTL and a handful of software.

The one thing simulation has never covered is **concurrency**.
`tb_vscroll.v` connects the blitter straight to a VRAM model with no
other master. On hardware the CPU shares that port through
`arbiter_vram`, and a VRAM-to-VRAM copy is the only operation that
issues three back-to-back transactions on it -- source read,
destination read, destination write -- where every other mode splits
the source onto the separate main-memory port.

That is also the same class of bug this file already records as #5: the
glyph path needed `ST_GLYPH_HI_SETTLE1/2` for a bus-settle gap that was
too narrow, and it was found by simulation rather than reasoning.

The next step is a testbench that arbitrates the blitter against a
competing master, not more tracing. `rtl/gpu/bench/tb_arbiter_stress.v`
is the place to start from.

(In the end no competing master was needed at all: the real arbiter and
the real `vram.v` alone, with every other master idle, reproduce the
corruption deterministically. See "The fix: a stale ack, not a wrong
address" below. The apps were disabled at one line each while this was
open; all three are re-enabled now.)


## Source-read probe

Three read-only registers on the blitter, mirroring what
`rtl/audio_mixer.v` already does for the mixer:

| reg | addr | |
|---|---|---|
| 16 | `0xd0000040` | address last presented for a source read |
| 17 | `0xd0000044` | data actually received |
| 18 | `0xd0000048` | source reads since reset |

Software reads the same address itself and compares. If the values
differ, the blitter is not seeing what the CPU sees, and the fault is in
the bus path rather than in the blitting — a distinction that cannot be
made from software alone. The mixer's own comment says exactly this, and
it could have been written for this bug.

The count matters: without it, reading the same values twice is
ambiguous between "nothing happened" and "it happened again
identically".

`z_fb_hw_scroll_probe()` prints three things rather than two — what the
copy **asked for**, what the blitter **presented**, and what **came
back**. That separates two quite different faults:

- presented ≠ asked → the walk is addressing the wrong word
- presented = asked but data ≠ what the CPU reads → the bus path

Given the corruption duplicates the row's opening text, "presented"
drifting toward the row origin is the thing to watch, and the printout
gives the difference in words directly.

The decode widened from four bits to five to fit these: registers 0-15
were full. Addresses 16-31 previously aliased back onto 0-15, which
nothing relied on and which would have made the probe overwrite CTRL.

Free when unused: three registers and no effect on any datapath. All
four blitter testbenches still pass.

### The probe read zero: multiple drivers

First hardware run of the probe returned `reads=0` with all three
registers zero, while `dst_x` read back correctly — so the block was
decoding fine and the counter genuinely was not counting.

The cause: the probe registers were **latched in the state-machine
`always` block but reset in the register-file block**. Two always
blocks assigning one reg.

Simulation tolerates that, and all four testbenches passed. Synthesis
does not, and on hardware the counter read back as a constant zero
while the latches appeared to work.

A probe that reads zero is indistinguishable from a probe that is
absent, which is what made this cost a build to find — the diagnostic
failed in exactly the way that looks like the thing it was diagnosing.

One reg, one always block. The reset now lives beside the latch.

### The probe's verdict: the source read is correct

Hardware, three consecutive scrolls:

```
blit probe: reads=2376
  asked for  : 200056b8
  presented  : 200056bc  (+1 words from asked)
  blitter got: 00001c90
  cpu reads  : 00001c90  MATCH
```

- **2376 reads per scroll** is exactly 216 rows x 11 words -- a 10-word
  span plus the shifter's prime. The walk issues precisely the reads it
  should, no more and no fewer.
- **+1 word** is the shifter's look-ahead: the last read fetches one
  word past the last needed. Expected.
- **Data matches** what the CPU reads at the same address.

So the copy engine addresses its source correctly and receives the
right data. The fault is downstream -- the shifter, the merge, or the
destination read/write -- and not in anything measured so far.

### A methodology note, because it cost a round

An attempt to bisect this with a fresh testbench produced nonsense:
the NON-overlapping control case reported more wrong pixels than the
overlapping case it was meant to be compared against, which cannot
happen. The expected-value arithmetic in that bench was wrong, not the
blitter.

That also casts doubt on `tb_vscroll.v`'s pass. It compares the
destination against the SOURCE MEMORY after the blit, so a systematic
error affecting both readings identically would go unnoticed --
exactly the trap that let the earlier "verified correct" claim stand.
`tb_edge.v` avoids it by comparing against constants (source all-ones,
destination all-zeros) and is the more trustworthy of the two.

The right test for an overlapping copy is:

1. snapshot the source region into a separate array BEFORE the blit
2. run the blit
3. compare the destination against the SNAPSHOT, never against live
   memory, and never against a formula recomputed independently

Step 3 is the one that matters: a formula can be wrong in a way that
mimics a hardware fault, and live memory can have been overwritten by
the very operation under test.

## The fix: a stale ack, not a wrong address

**Resolved.** The scroll-by-blit corruption was a stale-ack hazard in
the VRAM-to-VRAM copy path -- the same class of bug as the glyph
path's `ST_GLYPH_HI_SETTLE1/2`, in two more places. Fixed in
`rtl/gpu/gpu_blit.v` (`ST_MEM_SETTLE1/2`), reproduced and verified in
simulation by `rtl/gpu/bench/tb_system_vscroll.v`, and re-enabled in
`term`, `text` and `read`.

### The mechanism

`vram.v` re-asserts `wb_ack_o` on **every** cycle `cyc`/`stb` are held
-- it has no "already acked" guard -- and `arbiter_vram`'s response
routing is registered one cycle behind. Put together: after the
blitter consumes an ack and drops `m_cyc_o`, **`m_ack_i` stays high
for two more cycles**, with `m_dat_i` holding a re-read of the *old*
address. (One cycle for `vram.v`'s registered ack to clear once `stb`
is gone, one more for the arbiter's registered routing to follow --
the same arithmetic that sized the glyph settle pair.)

The VRAM-source copy path had exactly two transitions that re-entered
the framebuffer port with only **one** cycle of `cyc` low in between:

- **prime → first source read.** The wait state samples the stale ack
  one cycle after asserting, captures the **prime word again** as
  "source word 1", and the read it just launched dies -- the arbiter
  grants one cycle after `stb` has already gone away, so the
  transaction never reaches VRAM. The shifted stream is one word
  behind at the start of every row, so the **row's opening word
  reappears one word later** -- `| the   | the It gives...` --
  then the stales are consumed and everything re-syncs.
- **source read → destination read.** Same sampling error: `read_data`
  captures the **source** word instead of the destination word,
  corrupting the preserved bits of partial edge words.

### Why every measurement said what it said

Each prior result was honest; each was answering a narrower question
than it appeared to:

- **The probe exonerated the source read** because the ack *count*
  increments once per wait-state visit whether the ack is stale or
  not (2376, exactly right), and the *last* read of a blit -- the
  only one inspectable after the fact -- is clean: the stales are
  consumed mid-row, transactions before the final one.
- **`tb_vscroll.v` and `tb_edge.v` passed** because both model the
  framebuffer as an idealized slave whose ack pulses exactly once. A
  clean-ack model *cannot* express this bug, no matter what it
  checks. It was never the comparison method that was weak -- it was
  the bus model.
- **Only this mode failed** because a VRAM-to-VRAM copy is the only
  operation with back-to-back *released* transactions on the m port.
  A main-memory source interleaves the s port between them; a fill
  holds `cyc` continuously through its read-modify-write, a rhythm
  the always-acking `vram.v` happens to serve correctly. (Which is
  also why the fix must be in the blitter: edge-gating `vram.v`'s ack
  would hang every fill.)
- **Doubling the blit changed nothing** because the fault is inside
  each blit's per-row rhythm, not in its first transaction.
- **No competing master was needed.** The "concurrency" hypothesis
  was half right -- the fault *was* environmental, in the arbiter
  path no bench modelled -- but the blitter races only its own
  previous transaction's ghost.

### The fix

Two settle states, `ST_MEM_SETTLE1`/`ST_MEM_SETTLE2`, plus a
`settle_ret` register naming where to resume -- entered from
`ST_MEM_PRIME_WAIT` and `ST_MEM_READ_WAIT` (including the cookie-cut
A→B loops) **only when the source is VRAM**. A main-memory source
keeps its exact timing. Mirrors the glyph fix in shape and in size.

Cost: two cycles per source read, about 22 cycles per row for the
80-column scroll -- the scroll stays roughly 4x faster than
re-rendering.

`settle_ret` is written and reset in the state-machine `always` block
only. (See "The probe read zero: multiple drivers" above for what
happens otherwise; `yosys` `check -assert` is clean.)

### The testbench that finally sees it

`rtl/gpu/bench/tb_system_vscroll.v` drives the **real**
`gpu_blit_wb` + **real** `wb_arbiter_vram` + **real** `vram_wb`
together -- the trio that runs on hardware -- and checks
snapshot-first, exactly as the methodology note above prescribes.

It covers: the exact hardware case (`x=46 y=53 w=303 h=225 dy=-9`),
an alignment sweep (x = 0, 12, 32, 44, 46, 63, and a 1px-wide copy),
the prime (`sbit0 < 0`) case, `sx != dx` shifted copies, three
back-to-back overlapping scrolls re-snapshotting between each, and
unclipped/clipped/XOR fills through the real arbiter to guard the
held-`cyc` fill rhythm the fix must not disturb.

Against the pre-fix RTL it fails 367 checks -- every one of the 225
rows of the hardware case corrupts identically, destination word 2
holding word 1's data -- so the bench is proven sensitive, not
merely green. Against the fixed RTL everything passes, and
`tb_vscroll`, `tb_edge`, `tb_memblit`, `tb_rop` and `tb_straddle`
still pass unchanged.

**This bench is the gate for any change to the copy path's state
machine.** The idealized-slave benches remain useful for datapath
logic, but they are structurally blind to ack-timing bugs, which is
the class this module keeps producing.

(Noted while regressing: `tb_glyph.v`, `tb_line.v` and
`tb_arbiter_stress.v` fail identically on the RTL both before and
after this fix -- byte-for-byte the same output -- so they were
already failing at this commit, most likely bench rot from an earlier
RTL change rather than a hardware fault, since glyphs demonstrably
work on hardware. Worth a separate look, `tb_arbiter_stress.v`
especially, since it is the designated cross-master canary.)

### Status

`text` uses hardware scrolling. `term` and `read` do NOT -- both are
back on their pre-blit versions; see "Where this ended up" at the end
of this file. All debug scaffolding remains in place and switched
off: `z_fb_scroll_debug`, `z_fb_scroll_dbg_armed`,
`z_fb_scroll_align`, `z_fb_scroll_twice`, `scroll_dbg`, the
source-read probe, and `TERM_INSTRUMENT`/`TEXT_INSTRUMENT`.

### What a correct blit exposed: the apps' own bugs

*(Historical. `term` and `read` have since been reverted to their
pre-blit versions -- see "Where this ended up". The `text` fix below
is live, and the reasoning is kept because the ghost class it
describes applies to any overlay drawn on top of scrolled content.)*

First hardware run with the fixed RTL: `term` perfect, `text` left a
small vertical line at the left of the text, `read` repeated a line
when scrolling forward. Neither is the blitter -- both are app
integration bugs that were UNMEASURABLE while the copy itself
corrupted everything, and `term` being clean is the tell: it is the
one app that fully accounts for its overlay (it explicitly moves its
cursor bookkeeping with the pixels, `draw_cursor_y -= n`).

**`text`: the caret ghost.** The caret is a 1px vertical rule drawn
over the text. The scroll blit MOVES those pixels; `scroll_repaint()`
redrew only the rows that scrolled in and drew the caret at its new
position -- nothing ever erased the translated old rule, and
`move_cursor_ex()`'s scroll path returns before its own old-row
redraw. One ghost per scroll, at the caret's old column -- the left
text edge when moving through line starts, stacking into "a small
vertical line on the left". Fixed with term's own lesson:
`draw_caret()` records the screen row it drew on, and
`scroll_repaint()` redraws the row the translated ghost landed on
(when it survived the scroll; rows that scrolled in were redrawn
anyway).

The same class covers `read`'s selection highlight and focused-link
box: any OVERLAY drawn on top of content is translated by the blit
with nothing erasing the original. `read` now takes the full-repaint
path whenever either is on screen.

**`read`: three integration bugs in one function.** The old
`scroll_forward()` (a) redrew the exposed strip from the STALE
pre-scroll `vlines[]` cache -- repainting rows that had just been
moved, which is the repeated text -- (b) never relaid the screen out,
leaving `vlines[]`/`vlinks[]` (selection, clicks, Tab focus)
describing rows no longer there, and (c) blitted by `vlines[n].y`,
which counts from the window edge, overshooting by the top margin and
by the new top block's `space_before`.

The rewrite lays the new screen out FIRST without drawing
(`draw_body_from()` with an unreachable threshold), so the shift can
be measured from both layouts rather than assumed; finds which old
row became the new row 0 by IDENTITY (per-row `(line, sub)` recorded
at layout), not by step count -- `subs_of()` charges a scroll step
for a blank line or a rule, which have no row, so the two diverge at
every paragraph gap; VERIFIES every surviving row against the old
screen (identity and exact offset, ~50 comparisons) and falls back to
the full redraw on any mismatch; and only then blits -- the body band
only, below the top margin -- and draws the exposed strip from the
fresh layout, band-filling rows that straddle the seam (rows that did
not fit on the old screen and fit now).

`sw/apps/read/render_test.c` gained a `seam` mode that models the
whole pipeline on the host -- layout, blit, strip redraw with band
fill, the verify guard -- and requires the result to equal a full
redraw pixel row by pixel row, from every position, at several
widths. Zero mismatches across five real documents, and the
pre-existing reversibility mode still passes. That test is also what
caught the step-count/row-index divergence before it reached
hardware.

## Correct but slower: when the blit is the wrong tool

Second hardware run: the glitches were gone, `text` was faster in
both scrolling and scrollbar dragging -- and `term` and `read` were
both SLOWER than before hardware scrolling existed. Measured with
`top` over telnet, and with `WELCOME.MD` in the reader.

A blit that is correct is not automatically a blit that pays. Two
different mistakes, and the honest lesson is the same one twice: the
comparison that matters is not "blit versus full re-render", it is
"blit plus what still has to be drawn, versus what the app would
otherwise have drawn" -- and the second term is not always a full
screen.

### What the blit actually costs

`rtl/gpu/bench/tb_scroll_timing.v` measures it, through the same
real blitter + arbiter + VRAM as the correctness bench, so the two
numbers are directly comparable rather than derived from datasheet
arithmetic:

| operation (term geometry, 80x25 of 5x8) | cycles |
| --- | --- |
| whole-area scroll blit | 39,010 |
| one glyph cell (hardware only) | 40 |
| full 2000-cell re-render (hardware only) | 80,000 |

The blit is worth about 975 glyph cells of hardware time, or about
350 cells once each cell's ~112 cycles of software overhead is
counted. That is the number an integration has to beat.

The same bench also confirms the stale-ack fix costs nothing on the
glyph path: 3,200 cycles per 80-cell row before and after, identical.
The copy path itself went from 28,450 to 39,010 cycles -- the settle
pair, paid once per source read. Worth knowing, and still far cheaper
than re-rendering.

### `term`: blitting when the app was going to repaint anyway

*(Historical -- this code is no longer in the tree. The FINDING
stands and is the useful part: see below.)*

`top` does not scroll in the sense the blit assumes. It emits a
newline that scrolls the terminal by one row, then REPAINTS the whole
screen with the same layout. The pixels for that layout are already
on screen, in the right place, so the shadow compare would have drawn
almost nothing.

Blitting destroys exactly that. It shifts the screen and the shadow
up by a row, so every row is now compared against its neighbour,
nearly all 2000 cells mismatch, and the terminal pays the 39,000-cycle
blit AND a full re-render -- strictly worse than doing nothing. The
blit was being used as a reflex where it should have been a decision.

The old `else` branch made it worse still: when it did not blit, it
called `shadow_invalidate()`. That was pure pessimism. The shadow
models what is ON SCREEN, and if the screen is not touched the shadow
is still exactly right; invalidating it forced a full re-render for a
scroll that could have cost nearly nothing.

`term` now counts both options over the model it is about to draw
either way -- cells still differing after shifting the shadow (what
blitting would redraw) against cells differing without shifting (what
doing nothing would redraw) -- and blits only when it wins by more
than the ~400 cells it costs. The count runs over cells already in
cache and exits as soon as the answer is settled, which for streaming
output is within the first few rows. Streaming still blits; `top` now
does not, and the removed `shadow_invalidate()` makes that path
cheaper than the pre-blit behaviour it replaces.

### `read`: a second layout pass that cost more than the blit saved

*(Historical -- this code is no longer in the tree.)*

The first rewrite laid the new screen out twice: once with drawing
suppressed to measure the shift, once to draw the exposed strip.
Correct, and slower than the full redraw it replaced -- a layout pass
re-reads and re-parses the document, which is far more expensive than
the glyph drawing it was saving.

The shift does not need a second layout. `draw_body_from()` now
records a STEP TABLE while it lays out: one entry per scroll step --
every wrapped sub, and also every blank line and rule, which cost a
step but produce no row -- holding each step's identity and the y at
which its block begins, before `space_before`. Because the layout and
`scroll_down()` walk the same blocks by the same rules, "the view
moved k steps" means "the new top is old step k", and

    shift = step_y[k] - MARGIN

falls straight out of the table already in hand. No second pass, no
file access. One layout pass, exactly as before hardware scrolling --
with the full-screen redraw replaced by one strip.

The step table also fixed a real over-conservatism: the previous
version matched rows by identity and could not place a new top that
landed on a blank line or a rule, so it fell back to a full repaint
every time a paragraph gap crossed the top of the screen -- visible
as "sometimes redraws for no reason when moving one line down".

The verify guard stayed, corrected: a step only has to match if its
whole BAND is above the redraw strip. A step reaching into the strip
was redrawn from the fresh layout regardless of what the blit brought,
and demanding a match there rejected most scrolls -- the block at the
seam being precisely the one whose surroundings the scroll changed.
With the band bound exact, the accelerated path is taken on 97-100%
of one-line scrolls across five documents with zero seam mismatches;
the remainder are whole-page scrolls landing past the last recorded
step, where a full repaint is the right answer anyway.


## Measuring instead of guessing

Third hardware run, with the term decision and read's single-pass
layout in place: **no perceptible change to either app.** `read` still
slow, `top` in `term` still slow. (RTL unchanged from the previous
build, apps rebuilt.)

That result is worth more than it looks. Two changes that provably
reduce work -- term no longer blits when repainting wins, read no
longer lays the document out twice -- produced nothing visible. When
removing work does not make something faster, the work being removed
was not the bottleneck, and every further guess is guessing.

What the baseline diff (`56c3796`, before hardware scrolling was
attempted) does and does not show:

- `term`'s render got strictly CHEAPER: the baseline redrew whole
  dirty rows, HEAD redraws only cells that differ from a shadow of the
  glass. That is not the regression.
- The glyph path in `zgfx.c` is untouched; the 586 added lines are new
  functions (shade, scroll, mem blit), not changes to character
  drawing.
- `tb_scroll_timing.v` confirms the stale-ack fix costs the glyph path
  nothing: 3,200 cycles per 80-cell row before and after.
- `term`'s idle spin became `z_proc_wait(Z_TICK_HZ / 30)` in this
  window. Suspicious for throughput, but message delivery unblocks a
  waiting process (`k_proc_unblock`), so a burst of output should not
  be capped at 30Hz. Not ruled out on hardware, only in the source.

So both apps are now instrumented in CYCLES rather than glyph counts,
because glyph counts cannot answer "is drawing the bottleneck at all?"

`TERM_INSTRUMENT` (term.c) reports, every two seconds: total cycles
split into vt_feed / decide / blit / draw, bytes fed with cycles per
byte, and cycles per glyph.

`READ_INSTRUMENT` (read.c) reports cycles per scroll split into
`scroll_down` / blit / layout+draw, and separately the cost of
`read_block()` -- every document access funnels through it, so that
one number is all of the layout's I/O and parsing, including whatever
caching sits underneath.

The specific thing to look for in `read`: if `read_block()` dominates,
the reader is limited by re-reading and re-parsing the document on
every scroll, and no drawing strategy will change what it feels like
-- the work belongs in caching the layout, not in the blitter. Both
counters use `rdcycle`, a real free-running hardware counter here
(picorv32 with ENABLE_COUNTERS); `z_uptime_ticks()` at 732Hz cannot
resolve a single scroll, which is part of why this went unmeasured
for so long.


## Where this ended up

`text` keeps hardware scrolling and is measurably better for it --
faster on both single-line scrolling and scrollbar dragging, with the
caret ghost fixed.

`term` and `read` are reverted to their pre-blit versions
(`56c3796`). Neither ever got faster from the blit on real hardware,
and two rounds of app-side fixes -- each of which provably removed
work -- produced no perceptible change. A change that removes work
and changes nothing is measuring something other than the bottleneck,
and the honest response is to stop rather than keep adding machinery
to apps that were fine before.

A further signal, and an unexplained one: the blit-era `read` binary
was about THREE TIMES the size of the pre-blit build. The source grew
by a few hundred lines, which cannot account for that. Something in
that work pulled in a large dependency -- worth finding before any of
it is reapplied, and possibly related to the slowdown, since a much
larger binary on this machine means a different memory and cache
story entirely.

### What survives, and is worth keeping

- **The RTL fix.** `rtl/gpu/gpu_blit.v`'s `ST_MEM_SETTLE1/2` stale-ack
  fix is correct and independently verified; the corruption it fixes
  was real and would affect any future user of the VRAM-to-VRAM copy
  path. `text` depends on it.
- **The benches.** `tb_system_vscroll.v` (real blitter + arbiter +
  vram, fails 367 checks on the pre-fix RTL) and `tb_scroll_timing.v`
  (what operations actually cost).
- **The cost numbers.** The whole-area scroll blit is ~39,000 cycles,
  a glyph cell ~40 cycles of hardware time and ~112 including software
  overhead. So the blit is worth roughly 350 cells. An app that would
  otherwise redraw fewer than that should not blit at all.
- **The `term` finding**, which is the most reusable result here: a
  full-screen app that REPAINTS (top, vi -- anything using cursor
  addressing) may emit a scroll and then rewrite the same layout. The
  pixels are already correct and in place, so a shadow compare draws
  almost nothing -- unless a blit has just shifted the screen and the
  shadow, at which point every row is compared against its neighbour
  and nearly every cell mismatches. Blitting there costs a full
  re-render MORE than doing nothing. Any future re-attempt has to
  decide per scroll, not per app.

### Postscript: it was not the video path

Confirmed on hardware after the revert:

- **`read` performs the same with and without the blit**, and the
  pre-blit binary is ~50K against ~150K for the blit-era one. Same
  speed, a third of the size. Since the blit changed DRAWING
  substantially and moved nothing, drawing was never the constraint.
  The board is SDRAM with no dcache, where `read` was originally
  developed on 10ns SRAM (`obst`) -- and markdown parsing is
  byte-at-a-time scanning over file buffers, exactly the access
  pattern that collapses without a cache. That is a dcache problem,
  not a blitter one, and no application-level change reaches it.

  The `- cpu: ... MIPS @ ... MHz (... IPC)` line the kernel prints at
  boot (from `rdcycle`/`rdinstret`) quantifies this directly:
  comparing IPC between the two boards measures the stall.

- **`term` under `top` was the ethernet interrupt**, not video at all.
  `Z_IRQ_ETH` is a rising-edge pulse (rtl/sysctl.v edge-detects the
  MAC's level deliberately -- bit 8 is latched and a latched level
  re-fires forever), while `kernel.h` and the ISR both described it as
  a LEVEL and concluded there was "no window in which the interrupt
  has been acknowledged but a packet is still unread". True of a
  level, false of an edge. A frame arriving anywhere in net's loop
  body unblocked a process that had not blocked yet -- a no-op -- and
  the frame sat unread until the backstop timeout expired ~100ms
  later, where the old `z_proc_wait(1)` had capped it at 1.4ms. Fixed
  by making an early wakeup sticky (`Z_PROC_FLAG_WAKE`), and the
  comments corrected.

Both of the real causes were in the memory system and the interrupt
path. The blitter work was a long detour around them, and the reason
it took so long is that nothing was measured on hardware until the
end.

### The exhaustive elimination: do not bisect for text speed

Before spending time bisecting 56c3796..HEAD for a text-drawing
regression, know that every functional delta in that window has been
checked, and the text path is unchanged at every level:

- `term.c`'s entire draw path is byte-identical to 56c3796 when
  reverted, and still felt slow -- identical code, so the code is not
  the cause.
- `z_fb_draw_char2`, `z_fb_draw_char` and `gpu_blit_acquire` in
  zgfx.c: byte-identical to 56c3796 (the file's 586 new lines are new
  functions, not changes).
- The blitter's glyph path: MEASURED identical across all three RTL
  versions -- 56c3796, 0747b98, and 0747b98 with the stale-ack fix --
  3,200 cycles per 80-cell row in every case, through the real
  arbiter and VRAM (`tb_scroll_timing.v`). The "new blitter
  ops/patterns" cost the glyph path nothing.
- `rtl/sysctl.v`'s 77 changed lines are the ethernet interrupt and
  comments; nothing touches the bus, the clocks, or memory.
- `rtl/ethmac_rmii.v`: one added output wire, no datapath change.
- `zvt100.c`: a scroll counter; the mark-all-dirty on scroll predates
  the window.
- `zwin.c` / `zeitlos.h`: additive (new functions and registers).
- wm/repl/net moved from spinning to blocking, which RETURNS CPU to
  the foreground app -- the wrong sign for a slowdown.
- The SDRAM controller (`sdram_kianv.v`) is unchanged in the window
  and already tuned: KEEP_OPEN row policy, CAS 2, per-bank open-row
  tracking. No easy factor-of-two is sitting in it.

A bisection between these commits for text-drawing speed will
terminate with "no commit is bad". The remaining explanation is the
one `read` already demonstrated: this board is SDRAM with no dcache,
and the apps' speed memories come from `obst`, which is 10ns SRAM.
Byte-at-a-time parsing, per-cell MMIO setup, stack traffic -- all of
it stalls on SDRAM and none of it did on SRAM.

**The two-minute test that settles it:** compare the boot line
`- cpu: <name> N.NN MIPS @ N MHz (N.NN IPC)` between sergei_ml1 and
obst. It is computed from rdcycle/rdinstret over real work. If
sergei's IPC is a fraction of obst's, the slowdown is the memory
system, it affects everything equally, and the fix is a dcache -- not
anything findable by bisection.

**Before testing networking on obst:** obst is a `SPI_ETH` board, and
until the `ETH_SPI`/`SPI_ETH` fix in `rtl/sysctl.v` it had TWO drivers
on `eth_rx_ready` -- the interrupt line resolves to `x` when a frame
arrives. Any obst bitstream built for comparison must include that fix
or the networking comparison is meaningless.

### If this is revisited

Measure first, on hardware, with `rdcycle`. The specific question for
`read` is whether `read_block()` -- all of the document I/O and
parsing -- dominates a scroll. If it does, no drawing strategy will
change how the reader feels, and the work belongs in caching the
layout. The question for `term` is whether `vt_feed` dwarfs the draw
loop. Both were left unmeasured through this entire effort, which is
why it ran as long as it did.

One safe improvement is available independently of any blitting: the
old `term` scroll path called `shadow_invalidate()`, forcing a full
re-render. The shadow models what is ON SCREEN, so if the screen is
not touched it stays accurate and the compare would draw only what
changed. Dropping that invalidate makes scrolling cheaper than the
pre-blit baseline with no blit involved at all -- but it should land
as its own change, measured on its own.
