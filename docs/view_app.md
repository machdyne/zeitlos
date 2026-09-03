# view -- the image viewer

`sw/apps/view`, with the decoders in `sw/common/zimg.c` / `zimg.h`.

```
> run wm
> run view
```

or double-click an image in `files`, which launches it with the
filename through wm's pending launch argument (`Z_WM_SET_ARG`,
`zwm.h`).

## What it does

Opens an image, dithers it to 1bpp, and shows it in a resizable
window with scrollbars. The open icon in the titlebar
(`Z_WIN_FLAG_OPEN_ICON`) brings up the file dialog; `o` does the same
from the keyboard.

| Format | Variants decoded | Notes |
|---|---|---|
| BMP | 1/4/8/24/32bpp, uncompressed | RLE4/RLE8 and BI_BITFIELDS refused |
| PNM | P1–P6 | 8-bit samples; 16-bit refused |
| GIF | 87a/89a, LZW, interlaced | first frame only |
| JPEG | baseline sequential, 4:4:4 / 4:2:2 / 4:2:0 | progressive refused |
| ZBM | `zbm.h` | draw's own format, no dithering needed |
| PNG | — | recognised, decoder off by default |

## The constraint everything follows from

There is no dynamic memory (`docs/app_runtime.md`) and the kernel pool
is 1MB shared between every process (`sw/os/mem.h`). A 640×480
truecolour image is 900KB. So no decoded colour image can exist here,
at any size, ever.

Every decoder is therefore **row-streaming**: read one source row,
convert to 8-bit gray, decimate, dither into the document, forget it.
Nothing bigger than a single row is held except what the compression
scheme itself demands — GIF's LZW dictionary, JPEG's MCU row.

Two consequences worth understanding before filing a bug about either:

- **Oversized images are downscaled at decode time**, by a power of
  two, not stored at full resolution and panned. The scrollbars pan
  around the 640×480 *document*, which matters because the window is
  usually smaller than that — not because the image might be bigger.
- **Dithering happens during decode.** There is no greyscale
  intermediate, so there is nothing to re-dither from. Changing the
  scale means decoding the file again.

## Memory

Measured, `--gc-sections`, rv32im, `ZIMG_OPT=-O2` (the recommended
build — see "Third profile" below):

| | text | bss |
|---|---:|---:|
| document (640×480×1bpp) | — | 38,400 |
| decoder scratch (union) | — | 17,664 |
| dither error rows + gray row | — | 3,208 |
| file buffer | — | 1,024 |
| `zimg.c` code | 15,585 | — |
| `view.c` code | 5,069 | — |
| shared `sw/common` after GC | ~25,800 | ~11,600 |
| **total with libc** | **59,992** | **68,220** |

≈145KB once the 16KB stack and 4KB alignment are added. `draw` for
comparison is ≈112KB, so both can be resident at once with most of the
1MB pool still free. At `ZIMG_OPT=-Os` the text figure is ~2.9KB
lower.

### The scratch union

Per-format scratch lives in a `union` in `zimg.c`, not a struct. Only
one decoder ever runs at a time, so GIF's 16KB dictionary and JPEG's
10KB MCU row occupy the same bytes:

| Decoder | private scratch |
|---|---:|
| BMP / PNM | 1,024 |
| JPEG | 15,668 |
| GIF | 17,664 ← sets the size |
| PNG (if built) | 40,753 ← would set it |

Summing instead of unioning would cost 34KB today and 75KB with PNG.

JPEG's figure includes the 2KB of Huffman peek tables and the 512-byte
entropy staging buffer added during optimisation. Both were free in
`.bss`: GIF still sets the union size, and JPEG had headroom
underneath it.

## Why PNG is optional

`Z_IMG_HAVE_PNG` defaults to 0; build with `make VIEW_PNG=1`.

DEFLATE's 32KB sliding window is not negotiable — it is what the
format is defined against, and a smaller one silently corrupts any
file that references further back than it holds. That single buffer is
larger than every other decoder in this file put together, and inflate
is also the slowest thing here by a wide margin: Huffman decoding is
serial and branchy, which is the worst case for this core.

So it is a real cost decision, not an oversight. Enable it once the
JPEG timings below have shown what a comparable decode actually costs
on hardware.

Note `z_img_sniff()` recognises PNG whether or not the decoder is
built, so an unbuilt PNG produces "Support for this format is not
built in" rather than "this is not an image".

## Dithering

Floyd–Steinberg by default. The error rows are `int16_t` rather than
`int` (2.5KB instead of 5KB at this width), and output bits are
accumulated into a 32-bit word and written once per 32 pixels rather
than read-modify-written individually — there is no data cache on this
core, so that is the difference between a visible pause and an
imperceptible one.

**Interlaced GIF falls back to an ordered (Bayer 8×8) dither.** Error
diffusion is inherently sequential: row *y* needs row *y−1* already
quantised. An interlaced GIF delivers rows 0, 8, 16… then 4, 12, 20…,
so Floyd–Steinberg over it produces horizontal streaking that looks
exactly like a decoder bug. `z_img_t.was_ordered` reports which was
used.

### Finding the bottleneck

`make VIEW_PROF=1` reports per-phase cycles, retired instructions and
IPC to UART0 for every image loaded:

```
view: profile
  phase     calls      cycles       min       avg   ipc/1000
  read      10675     3400000       280       318        160
  entropy    2400   115000000     44000     47900        150
  idct       1600   240000000    145000    150000         38
  dither      256    98000000    380000    382000         44
  blit          1      120000    120000    120000         90
```

This uses `sw/common/zprof.h`, which is `sw/apps/play/prof.h`'s
mechanism moved into `sw/common` so `zimg.c` can be instrumented too.
play's copy is deliberately left alone — it carries audio-specific
accounting and its own report format, and rewriting a working
profiler in a working app is a change with no upside.

**Why not `z_uptime_ticks()`.** Because it cannot work. It is a
syscall (a `z_obj_t` dispatched through the `reg_kernel` trampoline),
and its resolution is one KTIMER tick — 1.37 ms, 65,664 cycles. Every
phase worth optimising is far shorter than that. `rdcycle` is one
instruction, no bus transaction, 1-cycle resolution, and costs no
gateware because it is already built.

**Why the brackets are per-row, never per-pixel.** There is no data
cache on this SOC — `rtl/cache.v` caches instruction fetches only — so
the profiler's own slot update costs a load and a store at full
main-memory latency, ~11 cycles per word on SDRAM and ~63 on PSRAM. A
per-pixel counter in the dither loop would cost more than the
arithmetic it was timing, and the resulting profile would be a picture
of the profiler. Around a 640-pixel row it amortises to well under a
cycle per pixel.

**Reading the numbers.** `min` is the cheapest observed call. It is
the phase's cost uninterrupted by the scheduler **only when the phase
is shorter than one KTIMER slice (65,664 cycles)** — otherwise it
necessarily contains preemption and is not a clean figure. In the
first hardware profile, `read` (min 369) and `entropy` (min 2,781)
qualified; `dither` (min 427,379, about 6.5 slices) did not, and
reading its min as an uninterrupted cost would have been wrong.

A large `avg`/`min` ratio means "preempted often", or that the phase
has two populations — `entropy`'s 61× ratio was cheap calls served
from the buffer against expensive ones that triggered an SD refill.

`ipc` is printed per-mille because there is no floating point here;
read 170 as 0.170. It is the column that decides *what kind* of fix a
phase needs:

| ipc | meaning | fix |
|---|---|---|
| near 170 | executing many instructions — normal for this core (`docs/muldiv.md` measured 0.172) | do less work: cheaper algorithm |
| well below | **stalled**, not busy — waiting on the bus | fewer memory transactions, or gateware |

That distinction is exactly the question "should this go in hardware",
and no wall-clock timer can answer it. If `dither` comes back with a
low IPC, it is memory-bound and a hardware ditherer would help; if it
comes back near 0.17, the fix is in software and the RTL would be
wasted.

**Cost when off:** zero. `VIEW_PROF=0` (the default) removes 1,465
bytes of text and the 240-byte slot array, and emits no counter
instructions at all.

**One caveat.** The counters are emitted as raw instruction words
(`.word 0xC0002573`) because binutils 2.36+ moved CSR access into the
Zicsr extension and `-march=rv32im` no longer permits `csrr`. play's
header says objdump disassembles these back to the mnemonic — with
binutils 2.42 and no `_zicsr` in the arch string it does not, showing
`.word` instead. Verify with:

```
grep -cE 'c0002573|c0202573' sw/apps/view/view.dasm
```

**If a board is ever built with `ENABLE_COUNTERS(0)`**, `rdcycle`
becomes an illegal instruction and a `VIEW_PROF=1` build will hang on
the first image, with the silent-spin failure `docs/muldiv.md`
describes. There is no way to probe for that from software without
having already executed it, which is why this is a build flag rather
than always on.

### Wall-clock timing

`make VIEW_INSTRUMENT=1` reports source size, output size and elapsed
milliseconds per load:

```
view: decode JPEG 1600x1200 -> 400x300 in 412 ticks (562 ms)
```

Useful for "is this file slow", and nothing more — one KTIMER tick is
1.37ms, so it cannot attribute time to a phase. Use `VIEW_PROF=1`
above for that.

### The hardware ditherer: answered, no

This was the open question the instrumentation existed to settle, and
it is settled. **A single-line hardware ditherer is not worth
building.**

The first hardware profile put all four phases at 0.082–0.089 IPC,
within 8% of each other. Dithering was not distinctively stalled
relative to anything else, so moving it into gateware would have
accelerated 19% of the runtime and left the other 81% untouched.

Note the estimate that preceded the measurement was right about the
conclusion and wrong about the reasoning: it predicted dithering would
be a small fraction of a decode dominated by entropy coding. On JPEG
that held (19%). On GIF it did not — dithering was **77%** of
bracketed time. What killed the accelerator was not dithering being
cheap, it was dithering being no more bus-bound than anything else,
which only the IPC column could show.

The GIF case did produce a fix, but in software: see "Bilevel GIF"
below.

## Optimisation history

The first hardware profile of a 387x256 JPEG:

```
  phase     calls      cycles       min       avg   ipc/1000
  read      19124    34198555       369      1788        89
  entropy    2400   410019121      2781    170841        82
  idct       1600   136554464     20613     85346        87
  dither      256   139906491    427379    546509        87
```

720.7M cycles, 15.0s of the 16.16s measured end to end — so the four
phases accounted for essentially all of it.

The IPC column settled the hardware-ditherer question: every phase sat
at 0.082–0.089, within 8% of each other. Dithering was not distinctively
stalled, so a line-ditherer in gateware would have accelerated 19% of
the runtime and left the rest untouched. It was not built.

What the profile actually showed was instruction count. At ~0.085 IPC
against this core's 0.172 baseline everything is roughly 2x stalled,
but the loops were also executing far more instructions than they
needed to: ~159 instructions per byte read, ~123 per dithered pixel.

Four changes followed, all verified to produce **bit-identical output**
on every test fixture:

| change | effect (measured) |
|---|---|
| 512-byte staging buffer for the entropy stream | read calls 10,675 → **194** |
| 8-bit Huffman peek table | slow path 100% → **1.2%** of decodes |
| register-carried dither error | 4 load-modify-stores per pixel → 1 store, per-row `memset` gone |
| DC-only IDCT shortcut | **24%** of blocks skip all 256 multiplies |

The peek tables (2KB) and staging buffer (512B) cost 12 bytes of
`.bss` between them, because the scratch union is sized by GIF's
17.6KB dictionary and JPEG had headroom underneath it.

Second profile, after those four changes:

```
  phase     calls      cycles       min       avg   ipc/1000
  read        211     4688557       369     22220        90
  entropy    2400   199354950      1691     83064        83
  idct       1600   135982997       687     84989        87
  dither      256    81091291    229446    316762        94
```

720.7M -> 421.1M cycles, 15.0s -> 8.8s. Derived instruction counts
(`ipc x cycles`, which unlike raw cycles are immune to preemption):

| phase | instructions before | after | ratio |
|---|---:|---:|---:|
| read | 3.04M | 0.42M | **7.2x** |
| entropy | 33.6M | 16.5M | **2.0x** |
| dither | 12.2M | 7.6M | **1.6x** |
| idct | 11.88M | 11.83M | 1.00x |

**The IDCT did not improve at all**, despite the DC-only shortcut. Its
`min` fell from 20,613 to 687, so the fast path does fire — just
almost never on that image. The shortcut is entirely data-dependent:
24% of blocks on a synthetic test image, ~0% on a detailed photograph.
A flat-row shortcut added afterwards catches only another 1-2% for the
same reason (the column pass spreads DC into every row, so a row is
flat only when the block's coefficients sit in column 0).

So the IDCT's cost is not skippable blocks, it is the butterfly
itself: **~7,394 instructions per 8x8 block**, where a well-scheduled
integer IDCT is nearer 900. That looks like register spilling -- ~20
live 32-bit values through the pass, and `-Os` spills rather than
unrolls. Hence the `ZIMG_OPT` knob in the Makefile; `make ZIMG_OPT=-O2`
costs ~2.9KB of text and is a hypothesis with a cheap experiment
attached, not a known win.

### Third profile: the -O2 experiment

`ZIMG_OPT=-O2`, against the second profile:

| phase | instructions | after -O2 | change |
|---|---:|---:|---:|
| entropy | 16.55M | 12.02M | **-27%** |
| idct | 11.83M | 11.20M | -5% |
| dither | 7.62M | 8.63M | **+13%** |
| read | 0.42M | 0.44M | +4% |
| total | 36.4M | 32.3M | **-11%** |

8.8s -> 7.7s. Two things came out of it, one of them against the
hypothesis that motivated the experiment:

**The spilling theory was wrong.** `-O2` moved the IDCT only 5%. If
~7,400 instructions per block were register spilling, a compiler with
more freedom would have recovered far more than that. The cost is
inherent to how the butterfly is written, so the remaining fix is
algorithmic -- an AAN scaled IDCT that folds dequantisation into the
quant table, ~5 multiplies per 1-D pass instead of ~12 and far fewer
live values -- not a flag.

**`-O2` is not uniformly better.** It made the dither loop 13% worse
at unchanged IPC, i.e. genuinely more instructions rather than more
stalling: the loop already carries its state in registers, so
unrolling buys nothing and costs instruction-cache footprint, which is
the term that dominates CPI here. `dither_row_fs()` therefore carries
`__attribute__((optimize("Os")))` so the two can differ without
splitting the file.

Net position: **16.2s -> 7.7s, 2.1x**, output bit-identical throughout.
The three remaining phases are now comparable in cost (entropy 37%,
idct 35%, dither 27%), which is the shape of diminishing returns. The
one lever left with real headroom is the AAN IDCT.

Two earlier ideas were dropped on this evidence. Replacing `luma()`'s
multiplies with shift-adds is worthless — at 29 cycles a `MUL` is
cheap next to these instruction counts. And PNG stays off: inflate is
worse per byte than JPEG's entropy stage, whatever this decode now
costs.

## Bilevel GIF: no dither at all

A GIF profile put dithering at **77% of bracketed decode time**, where
JPEG had it at 27% — the same 99,072 pixels costing 89.9M cycles
either way, which is a useful confirmation the profiler is sound.

For a **two-colour** GIF that work is not merely wasted, it is wrong.
Such a file is already a 1bpp image wearing a palette, and this
display is 1bpp, so the correct rendering is a direct mapping. Error
diffusion instead renders two flat tones as textures that were never
in the source. `decode_gif()` now detects this and thresholds, which
is both exact and free.

The guard is the interesting part. The test is **not** "are there two
colours" but "do the two colours fall on opposite sides of the
threshold". A palette of two dark greys — say luma 100 and 120 — would
collapse to solid black under a threshold, losing the picture
entirely, where dithering renders them as two distinguishable
textures. Anything that does not separate cleanly keeps the dither.

Interlacing does not disqualify it: a threshold is stateless, so row
order is irrelevant, and it takes precedence over the ordered
fallback.

Verified against a two-colour GIF (exact, 0 pixels of 76,800
differing from a direct threshold of the source) and a two-dark-greys
GIF (guard holds, image still dithered rather than collapsed).

### What was NOT done, and why

The same profile showed GIF's `read` phase at 27.3M cycles against
JPEG's 4.9M, which looked like sub-block call overhead worth fixing
with the staging buffer that helped JPEG so much. It is not.

Cost per `fs_read_chunk()` is ~440K cycles for 1KB in *both* cases —
JPEG 4.94M over ~11 reads, GIF 27.3M over ~64. Identical. GIF was
simply a bigger file (~60KB against 11KB) at the card's ~110KB/s,
which is the same figure the BMP measurements gave. The bytes have to
come off the card either way and no amount of buffering changes that.

## Bit order

The document is in the framebuffer's own packing: pixel *x* at bit
`x & 31` of word `x >> 5`, **least significant bit leftmost**, exactly
as `zbm.h` specifies. That is what `z_fb_hw_blit_mem()` reads, so a
decoded image reaches the screen in one blitter operation with no
reformatting.

This is the opposite order from font and icon data, which is MSB-first.
Getting it wrong produces an image mirrored within each 32-pixel block
— a distinctive enough symptom that `zbm.h` names it.

## Sharing the decoders

`zimg.c` depends only on a read callback and a caller-owned bitmap. It
knows nothing about windows and calls no stdio. Any app can link it by
adding `zimg.o` to `OBJS`; `draw` is the obvious candidate, since its
canvas is the same size and the same packing, and opening a JPEG into
it would need no conversion.

## Diagnostics

`view` uses no `printf`. It writes to the 16550 at `0xf0000000`
directly (`reg_uart0_*`, `zeitlos.h`), polling LSR bit 5 exactly as
`sw/os/uart.c` does. `printf` costs ~100KB on the newlib toolchain
this tree targets — more than this app and all its decoders combined.

One caveat, since the numbers above show it: **stdio still gets linked
in**, because `blit_copy_setup()` in `sw/common/zgfx.c` calls `printf`
and sits on the `z_fb_hw_blit_mem()` path. Avoiding it in `view.c`
alone does not avoid it in the binary. Removing that one call from
`zgfx.c` would drop ~14KB here (picolibc) or ~100KB (newlib) from
every app that blits and doesn't otherwise print, which is worth doing
but is a change to shared code and belongs on its own.

## Not implemented

- **Full-screen mode.** ESC is already swallowed by `handle_key()` so
  the binding is not claimed by anything else in the meantime. It
  needs wm support that does not exist: a `Z_WIN_FLAG_UNDECORATED` to
  expose wm's existing internal `no_titlebar` (currently set only for
  the dock, `wm.c:2181`), a `Z_WM_SET_FULLSCREEN` that stashes and
  restores the window rect, and a way to suppress the dock — which is
  a real window in wm's list, so it must leave the clip stack rather
  than merely be drawn over.
- **Zoom.** Would mean re-decoding, per the streaming design above.
- **Animation.** Later GIF frames need the previous frame kept as
  8-bit indices to composite against — a 640×480 index plane, 300KB.
- **Progressive JPEG.** Needs every coefficient of the image resident
  across scans, ~600KB for luma alone. Structurally impossible without
  dynamic memory, not a tuning problem.
- **No dock icon yet.** Adding one means a 32×32 PNG through
  `sw/data/icons/gen_dock_icon_data.py`, entries in
  `sw/apps/wm/dock_icons.{c,h}`, and a line in `dock_candidates[]` in
  `wm.c`. Deliberately left out of this change so that adding a viewer
  does not also mean rebuilding the window manager.

## Testing

Decoders are verified against reference images: BMP (1/4/8/24/32bpp),
GIF (plain, interlaced, and two-colour), JPEG (4:2:0, 4:4:4, and with
restart markers), PNM (P1–P6, ASCII and binary), and oversized sources
exercising the downscale path.

Two checks, because they catch different things:

- **Intensity.** Dithering preserves local average intensity, so 8×8
  block averages of the 1bpp output are compared against the greyscale
  source. All cases land within 1.5% mean absolute error. This catches
  a decoder that is wrong.
- **Bit-exactness.** Every optimisation is checked with `cmp` against
  the output of the previous revision. All four JPEG optimisations,
  the flat-row IDCT shortcut and the GIF bilevel path are byte-for-byte
  identical on unaffected inputs. This catches a decoder that has
  become *subtly* wrong, which the intensity check would pass.

The second one earned its place. The `dither_mode` refactor for the
bilevel GIF path left `decode_gif()` still setting only
`im->was_ordered`, so interlaced GIFs silently fell back to
Floyd–Steinberg — the exact streaking artefact the ordered path exists
to prevent. No crash, no error, and the intensity check would have
passed it. It surfaced only because `t_int.gif` stopped matching its
previous output.

Specific-case tests worth keeping:

- a two-colour GIF must come out as an **exact** threshold of the
  source (0 pixels of 76,800 differing)
- a two-*dark*-greys GIF must **not** take that path, or it collapses
  to solid black
