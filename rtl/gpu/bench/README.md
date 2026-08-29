# GPU and HID testbenches

Self-checking testbenches, written against `iverilog`. Originally all
for the hardware glyph blitter (`rtl/gpu/gpu_blit.v`'s `CTRL_GLYPH`
mode); `tb_video_mode.v`, `tb_game_mode.v` and `tb_gamepad.v` have
since joined them, so the directory name is now broader than its
history.

- `tb_glyph.v` -- draws two synthetic 4-row glyphs back-to-back and
  checks the resulting framebuffer rows are correct. Catches the
  glyph-fetch pipeline timing bug documented in
  `docs/gpu_blitter.md`, "Bugs found (and fixed)", #3.
- `tb_straddle.v` -- draws a single glyph row positioned so its cell
  straddles two 32-bit framebuffer words, with the surrounding bits
  pre-seeded to a known pattern, and checks the split lands correctly
  with no bleed into neighboring bits.
- `tb_line.v` -- draws 48 distinct synthetic characters left to right
  at real `z_font_5x8` pitch (5px wide, 5px pitch), covering every
  possible word-alignment offset at least once, and checks every
  character landed in exactly its own bit range with no
  cross-contamination between characters.
- `tb_arbiter_stress.v` -- instantiates the real `rtl/arbiter_vram.v` and
  `rtl/mem/vram.v` (not a simplified mock) alongside `gpu_blit_wb`,
  plus a synthetic second bus master that continuously performs its
  own read-modify-write cycles to a fixed "canary" word (mimicking
  `gpu_raster_wb`'s own access pattern) as aggressively as possible.
  Draws the same 48-character line from `tb_line.v` under this
  contention and checks both that every glyph still lands correctly
  and that the canary master's own writes are never corrupted or
  starved. This is the test to extend first if you suspect a
  cross-process/cross-master hazard (e.g. `term` typing while `wm`
  redraws chrome via the line rasterizer).

`tb_glyph.v` and `tb_straddle.v` model the framebuffer bus slave
directly off `rtl/mem/vram.v`'s actual ack timing (important: it acks
unconditionally on every cycle `cyc`/`stb` are held, not just once --
`gpu_blit.v` never drops `cyc`/`stb` between a row's read and the
write that follows it, and only `vram.v`'s specific always-active
design makes that pattern land correctly). `tb_line.v` and
`tb_arbiter_stress.v` use the real `vram.v` directly.

- `tb_game_mode.v` -- `rtl/gpu/gpu_video.v`'s game mode. Tests
  ADDRESSES rather than pixels, since the feature is an address
  transform: for a given viewport origin and mode, which framebuffer
  column and row does each physical pixel come from? That is exactly
  the `x`/`y` output pair. Covers the desktop 1:1 regression, 2x
  doubling verified exhaustively across a full line, clamp and
  toroidal wrap, frame-boundary adoption of a mid-frame write, and the
  frame counter. Runs ~40 full 800x525 frames, so it takes a few
  minutes rather than a few seconds. See `docs/game_mode.md`.
- `tb_gamepad.v` -- `rtl/usb_hid.v`'s gamepad register. Uses a STUB
  `usb_hid_host` (the real core needs an actual USB device bit-banging
  a low-speed link to produce a report at all, and none of the
  behaviour under test lives in that core). Covers bit order for every
  button individually, the clock-domain tearing fix, hot unplug with a
  direction held, and the interrupt on device type change. Fast.
  See `docs/gamepad.md`.

## Status

All originally-four currently pass against the fixed `rtl/gpu/gpu_blit.v`. They
did **not** reproduce the horizontal (~32-64px) corruption reported
near freshly-typed text in `term` after the row-shift fix landed --
see `docs/window_manager.md`, "Known limitations", the "Unresolved:
horizontal garbage..." entry, for the current state of that
investigation and what's been ruled out.

## Running

`iverilog`'s comment-nesting lint trips on the literal string
`font/*.mem` inside `rtl/mem/glyph.v`'s header comment (a false
positive -- there's no real nested comment). Point `iverilog` at a
patched copy, or pass your toolchain's equivalent lint-suppression
flag if it has one. From the repo root:

```
sed 's#font/\*\.mem#font/_STAR_.mem#' rtl/mem/glyph.v > /tmp/glyph_lintfix.v

# tb_glyph.v / tb_straddle.v -- gpu_blit.v + glyph_mem + a mock
# vram-timing-accurate slave built into the testbench itself
iverilog -g2005 -o /tmp/tb_glyph.out \
    rtl/gpu/bench/tb_glyph.v rtl/gpu/gpu_blit.v /tmp/glyph_lintfix.v
vvp /tmp/tb_glyph.out

iverilog -g2005 -o /tmp/tb_straddle.out \
    rtl/gpu/bench/tb_straddle.v rtl/gpu/gpu_blit.v /tmp/glyph_lintfix.v
vvp /tmp/tb_straddle.out

# tb_line.v -- same as above, longer run
iverilog -g2005 -o /tmp/tb_line.out \
    rtl/gpu/bench/tb_line.v rtl/gpu/gpu_blit.v /tmp/glyph_lintfix.v
vvp /tmp/tb_line.out

# tb_arbiter_stress.v -- needs the real arbiter + real vram model too.
# vram_wb's own module header uses an empty "#()" parameter list that
# some iverilog builds choke on -- strip it from a scratch copy first.
sed 's/module vram_wb #()/module vram_wb/' rtl/mem/vram.v > /tmp/vram_fix.v
iverilog -g2005 -o /tmp/tb_arbiter.out \
    rtl/gpu/bench/tb_arbiter_stress.v rtl/gpu/gpu_blit.v \
    /tmp/glyph_lintfix.v rtl/arbiter_vram.v /tmp/vram_fix.v
vvp /tmp/tb_arbiter.out

# tb_game_mode.v -- gpu_video.v alone. Its port list ends in a trailing
# comma, which yosys accepts and iverilog does not.
sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
    rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
iverilog -g2005 -o /tmp/tb_game.out \
    rtl/gpu/bench/tb_game_mode.v /tmp/gpu_video_fix.v
vvp /tmp/tb_game.out          # several minutes

# tb_gamepad.v -- usb_hid.v alone, with its own stub usb_hid_host
# built into the testbench. usb_hid.v needs three of the same kind of
# lint fixup: the empty #() parameter list, a trailing comma in the
# port list, and curs_x/curs_y declared both as output nets and as
# regs. All three are the file's existing style and yosys is happy
# with them.
iverilog -g2005 -DUSB_HID -o /tmp/tb_pad.out \
    rtl/gpu/bench/tb_gamepad.v rtl/usb_hid.v
vvp /tmp/tb_pad.out           # about a second
```

All four print `RESULT: PASS` or `RESULT: FAIL` plus the offending
values on failure.
