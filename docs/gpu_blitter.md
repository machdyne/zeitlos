# Zeitlos GPU Blitter Developer Guide

## Overview

The Zeitlos GPU Blitter is a high-performance 2D graphics accelerator designed for efficient rectangular fills, sprite blitting, and text rendering. It operates on a 512×384 monochrome (1-bit-per-pixel) framebuffer, with word-level optimization and clipping available (but not automatic in every mode -- see "Clipping Behavior" and "Error Handling" below before assuming safety you haven't actually requested).

For fill mode specifically, `sw/common/zgfx.c`'s `z_fb_hw_fill_rect()` is the recommended, actually-safe way to drive this hardware -- see `docs/app_runtime.md`, "The GPU blitter" and "Best Practices" below.

## Key Features

- **Hardware-accelerated rectangular fills**
- **Screen boundary clipping when `CTRL_CLIP` is requested** -- not automatic otherwise; see "Clipping Behavior" below
- **Word-level optimization with pixel-precise masking**
- **Simple memory-mapped register interface**
- **Optimized for font rendering (6×12 characters)**
- **`z_fb_hw_fill_rect()` (fill mode only) adds real, unconditional bounds safety and cross-process protection on top of the raw hardware -- see "Best Practices" below**

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

## Control Register (BLIT_CTRL)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | START | Start operation (write 1 to begin) |
| 1 | FILL | Operation mode (0=copy, 1=fill) -- ignored in glyph mode |
| 2 | CLIP | Clipping enable (0=disabled, 1=enabled) -- ignored in glyph mode |
| 3 | GLYPH | 0=normal fill/copy, 1=glyph blit mode |

## Status Register (BLIT_STATUS)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | BUSY | Operation in progress (1=busy, 0=idle) |

## Screen Specifications

- **Resolution**: 512×384 pixels
- **Color depth**: 1-bit per pixel (monochrome)
- **Framebuffer**: Located at 0x20000000
- **Stride**: 64 bytes per line (512 pixels ÷ 8 bits/byte)

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

#define CTRL_START      (1 << 0)
#define CTRL_FILL       (1 << 1)
#define CTRL_CLIP       (1 << 2)

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
    BLIT_WIDTH = 512;
    BLIT_HEIGHT = 384;
    BLIT_PATTERN = PATTERN_BLACK;
    BLIT_CTRL = CTRL_START | CTRL_FILL;  // No clipping needed
    
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

// Font-sized character block
fill_rect(100, 100, 6, 12, PATTERN_WHITE);
```

### Text Rendering (real glyph blit mode)

The hand-drawn-boxes approach previously shown here was a placeholder
written before glyph blit mode existed. Real hardware-accelerated
text rendering is now implemented -- see
`docs/window_manager.md`, "Hardware glyph blitting" for the full
design (register layout, the bit-reversal needed to reconcile font
byte order with framebuffer bit order, word-straddle handling, and
known risk areas), and `sw/common/zgfx.c` (`Z_GFX_HW_BLIT` build) for
the actual C driving it. In short: `gpu_blit_wb` reads glyph row data
from a separate glyph memory (`rtl/mem/glyph.v`, loaded by software
via `z_gfx_hw_font_load()`), and blits one glyph per trigger with
solid foreground/background per pixel (a true text-cell fill, not a
transparent overlay like the software-only renderer) -- see
`BLIT_GLYPH_ADDR`/`_W`/`_H`/`FG_COLOR`/`BG_COLOR` in the memory map
above.

Glyph mode is unclipped by design -- the caller is responsible for
only triggering it for glyphs that are already fully on-screen (and
within any window clip rect), falling back to software rendering
otherwise. This keeps the hardware path simple at the cost of pushing
that one piece of judgment into the C driver.

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
        if (x <= 0 || x + box_size >= 512) dx = -dx;
        if (y <= 0 || y + box_size >= 384) dy = -dy;
        
        // Draw new position
        fill_rect(x, y, box_size, box_size, PATTERN_WHITE);
        
        delay();
    }
}
```

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
| 6×12 character | ~8-16 | Excellent |
| 10×10 box | ~20-40 | Very Good |
| 32×32 square | ~64-128 | Excellent |
| Full screen | ~1,200 | Blazing Fast |

## Clipping Behavior

### Automatic Clipping

When `CTRL_CLIP` is enabled, the blitter automatically:
- Clips rectangles to screen boundaries (0,0 to 511,383)
- Handles negative coordinates safely
- Prevents buffer overruns
- Uses pixel-level masks for partial word operations

**This is conditional, not automatic in the unqualified sense the
name suggests.** With `CTRL_CLIP` *not* set (which the "Best
Practices"/"Optimize Common Operations" sections below recommend for
"known-safe" operations), `ST_CLIP` in `gpu_blit.v` computes the
destination address directly from the raw, unvalidated
`dst_x`/`dst_y`/`width`/`height` -- no bounds check of any kind. Worse,
`ST_WAIT_READ`/`ST_WAIT_WRITE` wait on the framebuffer bus's ack with
no timeout of their own, so an out-of-range destination this way can
hang the blitter's hardware state machine forever, not just produce a
wrong pixel. See `sw/common/zgfx.c`'s `z_fb_hw_fill_rect()` and
`docs/app_runtime.md`, "The GPU blitter" for the actual fix
(coordinates clamped to the real screen bounds *unconditionally*,
regardless of `CTRL_CLIP`) -- **that function, not raw register
writes, is the safe way to drive fill mode now**; the raw-register
examples throughout this document (including immediately below)
predate that fix and no longer reflect the recommended way to use
this hardware.

### Examples

```c
// These are all safe with clipping enabled:
fill_rect(-10, -10, 50, 50, PATTERN_WHITE);      // Clips to screen
fill_rect(500, 300, 100, 100, PATTERN_WHITE);    // Clips to screen
fill_rect(256, 192, 1000, 1000, PATTERN_WHITE);  // Clips to screen
```

## Pattern Values

The `BLIT_PATTERN` register accepts 32-bit values:

```c
#define PATTERN_BLACK   0x00000000  // All pixels black
#define PATTERN_WHITE   0xFFFFFFFF  // All pixels white
#define PATTERN_CHECKER 0xAAAAAAAA  // Alternating pixels
#define PATTERN_DOTS    0x11111111  // Sparse dots
```

## Error Handling

The blitter's design *intent* is to be robust, but see "Automatic
Clipping" above before relying on any of this -- most of it holds
only when `CTRL_CLIP` is actually set:
- **Out-of-bounds coordinates**: Safely clipped -- *only if
  `CTRL_CLIP` is set*. Otherwise unchecked, and can hang the
  hardware state machine (see above).
- **Zero dimensions**: Operation completes immediately.
- **Negative coordinates**: Handled by clipping -- same `CTRL_CLIP`
  caveat as above.
- **Concurrent register access**: NOT arbitrated between processes.
  The Wishbone bus serializes individual read/write transactions, but
  nothing stops two processes from interleaving their own register
  writes if both try to set up a blit operation at once -- one
  process's dst_x/dst_y/etc. writes could land in between another's,
  corrupting both operations. Same class of problem as the GPU line
  rasterizer's shared clip register. Both fill mode
  (`z_fb_hw_fill_rect()`) and glyph mode (`z_fb_draw_char()`/
  `z_fb_draw_char2()`/`z_fb_draw_icon()`) already wrapped their own
  register-writes-then-trigger sequence in `maskirq()`, so two
  processes' register writes themselves can't interleave. **What
  WASN'T covered**, until `gpu_blit_acquire()` (`sw/common/zgfx.c`):
  every one of those callers checked "is the hardware idle?" with
  IRQs still enabled, *then separately* masked IRQs before its own
  writes+trigger -- a real gap between the check and the mask. A timer
  IRQ landing in that gap can switch to a different process, which
  observes the same "idle" state, wins the race, and starts (and
  fully masks) its own operation; when the original process resumes
  and finally masks IRQs, the hardware is no longer idle, but nothing
  re-checked that -- its own START trigger lands while `draw_busy` is
  still 1 and the state machine isn't in `ST_IDLE`, and is silently
  dropped (`start_trigger`'s `!draw_busy` gate is only evaluated
  inside the `ST_IDLE` case -- see `rtl/gpu/gpu_blit.v`). No error,
  no effect, and the caller has no way to know its operation never
  happened. `gpu_blit_acquire()` closes this by folding the busy-check
  into the SAME masked section as the trigger (re-checking under the
  mask, unmasking and retrying if it lost the race) -- used by all
  four callers above. See its own (long) comment in `zgfx.c`, and
  `docs/window_manager.md`'s "Known limitations" entry on `term`'s
  horizontal-garbage-near-typed-text report, for the full reasoning
  behind why this is a strong candidate for that bug specifically
  (not confirmed on real hardware).

## Bugs found (and fixed) during hardware bring-up

Worth knowing if you're debugging something that touches this module,
since all three were subtle and easy to reintroduce:

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
4. **`glyph_mem`'s CPU-facing read port aliased to the previous
   transaction's address** (`rtl/mem/glyph.v`) -- found while chasing
   the still-open "horizontal garbage near freshly-typed text" report
   below. A Wishbone read of word N would return word (N-1)'s data
   instead, and byte-at-a-time writes (exactly how `sw/common/
   zgfx.c`'s `z_gfx_hw_font_load()`/`z_gfx_hw_icon_load()` actually
   load data -- one `volatile uint8_t*` store per byte) could land
   at the wrong byte entirely. Root cause, isolated via direct A/B
   simulation testing (Icarus Verilog 12.0) rather than by
   inspection: the array was indexed through an intermediate `wire
   word_addr = wb_adr_i[...]`, reused across the read and all four
   conditional per-byte-lane writes in the same clocked block. That's
   textbook-correct, conventional Verilog and reads fine -- but
   reproducibly aliased in this specific simulator. Rewriting every
   use to index `mem[]` with an inline part-select of the raw port
   signal directly (`mem[wb_adr_i[ADDR_WIDTH-3:0]]`, no intermediate
   wire at all) -- exactly how `vram_wb` (`rtl/mem/vram.v`) already
   indexes its own `vram[wb_adr_i]`, which was verified clean under
   the identical test -- eliminated it completely. **Given the
   symptom would have to be present on literally the very first
   character ever rendered after boot (font loading, not just this
   port's readback, was affected) if this were happening on real
   hardware, and the reported garbling is intermittent, not constant
   or present from boot, this is believed to be an Icarus-Verilog-
   specific code-generation quirk rather than a real hardware/
   synthesis defect** -- included here, and fixed regardless, because
   it's a genuine, reproducible bug either way, and because nothing
   in this codebase currently performs a Wishbone READ of glyph
   memory at all (font/icon loading is write-only from the CPU side),
   so this particular instance was practically dormant. Also relevant
   to anyone writing future testbenches against this module: this
   specific "wire-typed intermediate memory-array index, reused
   across a read plus multiple conditional writes in one block" shape
   is now a known Icarus hazard in this project, worth checking for
   elsewhere before trusting a simulation result that depends on it.
   `gpu_raster.v`'s own `fifo_mem` was checked and does NOT share this
   shape (`fifo_wr_ptr`/`fifo_rd_ptr` are plain registered counters,
   not wire-derived part-selects) -- see the "horizontal garbage" note
   below for the broader investigation this came out of. **Update:**
   the "clean, passing, full end-to-end simulation" mentioned here in
   an earlier version of this entry turned out to be a false negative
   -- see bug #5 below for what it was actually missing and why.
5. **Straddling glyphs could corrupt the framebuffer word to the right
   of a character, deterministically, with no multi-process contention
   needed at all.** This is believed to be the actual root cause of
   the long-running "horizontal garbage near freshly-typed text"
   report -- see docs/window_manager.md's own entry for the full
   investigation trail (several earlier, real but ultimately
   insufficient fixes: `gpu_blit_acquire()`'s cross-process race,
   bug #4 above). Found by building a real Icarus Verilog testbench
   around the actual `gpu_blit_wb` + `glyph_mem` + `vram_wb` +
   `arbiter` sources and checking framebuffer content after a
   realistic multi-character string draw (word-straddling character
   included) with zero contention -- something the project's earlier
   per-piece testbenches (`tb_straddle.v`, `tb_line.v`,
   `tb_arbiter_stress.v`) never happened to combine: a straddling
   character with OTHER, non-zero ink already present in the low word
   from characters drawn before it in the same string.
   Root cause: `ST_GLYPH_WAIT_WRITE_LO` used to release the bus
   (`m_cyc_o`/`m_stb_o` low) for exactly ONE cycle before
   `ST_GLYPH_READ_HI` re-asserted it to start the high word's own
   read. That one cycle isn't enough settling time for either of two
   independently-registered pipelines to actually clear: `vram_wb`'s
   own ack (`rtl/mem/vram.v` re-asserts `wb_ack_o<=1` every single
   cycle `wb_active` is high, with no "already acked" guard the way
   `glyph_mem` has), and the arbiter's own response-routing
   (`rtl/arbiter.v`, itself registered one cycle behind its own state
   transitions, which briefly drop to `IDLE` when `m_cyc_o` goes low
   even for one cycle). The high word's read could see a stale,
   already-consumed ack left over from the low word's write, and
   capture the LOW word's read-modify-write data into `read_data`
   instead of the high word's own -- so the high-word write combined
   the (wrong) low-word content with the new glyph's spillover bits,
   corrupting whatever was in the high word (typically, whatever the
   NEXT few characters would go on to occupy) with a wide, wrong
   pattern. Confirmed cycle-by-cycle via direct signal tracing (a
   `$display` inside a copy of `gpu_blit_wb`, `glyph_mem`, and
   `rtl/arbiter.v`, watching `m2_ack_o`/`m2_dat_o`/arbiter `state`
   through the exact transition) before writing the fix, and confirmed
   NOT to reproduce with nothing already drawn in the low word first
   -- which is exactly why an isolated single-straddling-character
   testbench (no prior ink in that word) never caught it: a stale read
   of an all-zero word is indistinguishable from a correct one.
   Fixed by widening the low-word-to-high-word transition to match the
   row-to-row transition's own (already-safe) three-cycle margin --
   two new states, `ST_GLYPH_HI_SETTLE1`/`ST_GLYPH_HI_SETTLE2`, sit
   between the low-word write's ack and the high-word read's own
   trigger. Verified via simulation: a direct 13-character string draw
   (no contention) that previously produced ~24 bits of wrong ink per
   affected row now renders bit-for-bit correct; an 80-character
   string (many straddle crossings) also renders clean; the original
   contention-stress scenario (a straddling character under a
   continuously-requesting second bus master) still passes. On the
   strength of this, `term.c`'s `resweep_right_of_cursor()` mitigation
   has been removed outright rather than kept behind a flag -- see
   that function's own removal note (git history) for what to
   reintroduce if garbage is somehow still observed on real hardware
   after this fix, which would mean this wasn't (the whole of) the
   cause after all.

## Best Practices

**Use `sw/common/zgfx.c`'s `z_fb_hw_fill_rect()` for fill mode rather
than the raw register sequences below.** It does everything "Always
Wait for Completion" and "Use Clipping for Safety" describe, plus the
two things raw register access can't: coordinates clamped to the
actual screen bounds *unconditionally* (not just when `CTRL_CLIP` is
requested), and the register-writes-then-trigger sequence protected
against interleaving with another process's own writes. The
raw-register examples below are kept for reference/understanding the
hardware, not as a recommended way to drive it directly anymore.

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
// forever (see "Automatic Clipping" above). Only ever skip this for
// coordinates you can prove are always in-range by construction (a
// fixed, compile-time-constant full-screen clear, say) -- and even
// then, z_fb_hw_fill_rect() (which clamps regardless of CTRL_CLIP)
// removes the need to make that judgment call at all.
BLIT_CTRL = CTRL_START | CTRL_FILL;
```

### 3. Optimize Common Operations
```c
// Fast screen clear
void clear_screen_fast(void) {
    BLIT_DST_X = 0;
    BLIT_DST_Y = 0;
    BLIT_WIDTH = 512;
    BLIT_HEIGHT = 384;
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
- Check coordinate bounds (0-511 for X, 0-383 for Y)
- Verify pattern value (0x00000000 for black, 0xFFFFFFFF for white)
- Ensure width and height are non-zero

### Performance Issues
- Use clipping sparingly for maximum performance
- Batch operations when possible
- Consider object size and alignment

## Technical Details

### Hardware Architecture
- Word-level operations with pixel-precise masking
- Bresenham-style line generation for efficient memory access
- Automatic stride calculation for framebuffer addressing
- Pipelined state machine for maximum throughput

### Memory Interface
- 32-bit wide memory access via Wishbone protocol
- Burst-capable for large operations
- Automatic address generation and bounds checking
- Optimized for 1-bit-per-pixel framebuffer format

