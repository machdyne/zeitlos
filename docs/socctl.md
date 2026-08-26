# SOC Control (`socctl`)

A small bank of **writable** global configuration bits at
`0x7000_0200`, always present on every board.

## Why it exists separately from the CSRs

`rtl/csrs.v` is the read-only sibling: it answers *"what does this
bitstream have"*, and its own header is explicit that it has no state
machine and no side effects on read. That inertness is precisely why it
can be trusted, and why it is the one block with no `ifdef` guard --
it cannot itself be one of the things that might be missing.

Putting writable state in there would undermine that. So configuration
software *sets* lives in this sibling block instead, which shares the
always-present property but has an explicit write path.

## Why not its own address nibble

There isn't a good one left. `rtl/sysctl.v`'s map is nearly full, and
the only free top nibble (`0x8`) is the virtual window apps execute in
-- a stale app pointer dereferenced in kernel context would land on
control registers, which is a bad failure mode to invent for the sake
of tidiness.

Nibble `0x7` was already subdivided, so this is a third tenant rather
than a new precedent:

| Window | Block | Purpose |
|---|---|---|
| `0x7000_00xx` | `rtl/csrs.v` | read-only: what this bitstream has |
| `0x7000_01xx` | `rtl/cache.v` | instruction cache control/stats |
| `0x7000_02xx` | `rtl/socctl.v` | writable global config |

## Registers

Word-addressed, matching every other simple slave in this codebase.

| Address | Register | Description |
|---|---|---|
| `0x7000_0200` | CTRL | bit 0: cursor shape. 0 = normal (X), 1 = busy (Z). **Resets to 1.** Bits 31:1 reserved, write 0. |
| `0x7000_0204` | MAGIC | fixed `0x5A43_5452` (`"ZCTR"`) |
| `0x7000_0208` | VIDEO | bits 1:0: virtual phosphor mode. Reads back as `{0x5643, 14'b0, mode}`. Reset value comes from the board's `GPU_*` defines. |

MAGIC exists for the same reason `csrs.v` has one: reading an address
nothing decodes does **not** fault on this bus, so a known constant is
the only way software can tell "this block is present" from "this is
whatever the bus happened to resolve to". Always check
`z_socctl_present()` before trusting anything else here.

C-side accessors are in `sw/common/zsoc.h`.

### A note on the window offset

`socctl` sits at `0x7000_02xx`, not at the start of its nibble, so
`sysctl.v` must pass it a **masked** address:

```verilog
.wb_adr_i({ 30'b0, wbm_adr_sel_word[1:0] }),
```

`wbm_adr_sel_word` is `0x80` for this block's first register, not `0`.
Passing it through unmasked means no `case` arm ever matches: writes
vanish silently and MAGIC reads back as zero, so `z_socctl_present()`
returns false and every feature gated on it quietly disappears. The
cache block at `0x7000_01xx` needs the same treatment for the same
reason; only `csrs.v` gets away with the raw value, because its window
starts at offset 0.

## The busy cursor

The mouse pointer is a hardware sprite (`rtl/gpu/gpu_cursor.v`)
composited at scanout, so software **cannot draw over it** -- CTRL bit
0 is the only way to change its shape.

- **X** -- normal, the system is ready.
- **Z** -- busy, wait.

Two shapes on the same 5x5 point grid: the X is both diagonals (9
points), the Z is full top and bottom bars plus the anti-diagonal (13
points). The Z is drawn solid rather than sparse because a 5x5 Z with
gaps in the bars doesn't read as a Z at all, and the extra comparators
are cheap next to being unrecognisable. Both shapes use the same
negative-offset guards, so an offset that would fall off the top or
left of the screen simply isn't drawn rather than wrapping to the
opposite edge.

### Reset state is busy, deliberately

CTRL resets to 1, so the cursor is a Z from the moment the display
comes up -- through the BIOS and kernel boot, before any software
touches this register.

That is the honest default. From power-on until the window manager says
otherwise the system genuinely is still coming up, and a ready-looking
pointer over a machine that isn't ready is a small lie. It also fails
in the right direction: on a board where `wm` never runs, or crashes
during startup, the cursor stays Z -- which is exactly what is true.

Defaulting to X would additionally hide the indicator entirely on the
fast path, where the core apps finish loading before `wm` first writes
the register.

## Who sets it

`sw/apps/wm/wm.c` owns the busy state, as a **bitmask of reasons**
rather than a bool or a counter:

```c
#define WM_BUSY_STARTUP   (1u << 0)   // core services not up yet
```

A bool breaks as soon as two things are busy at once -- whichever
finishes first clears it while the other is still going. A counter
fixes that but leaks forever on a single unbalanced call, and gives you
nothing to look at when it does. Named reasons are idempotent, clearing
is definitive, and a stuck busy state can say on the console exactly
which reason is stuck.

Adding a second reason is one `#define` plus paired
`wm_busy_set()`/`wm_busy_clear()` calls. `wm_busy_apply()` is the only
place that touches the register.

### `WM_BUSY_STARTUP` and the dock

`term` connects to `repl` over a port as soon as it starts, and that
connect has a timeout. Launched before `repl` has registered itself, it
comes up as a blank window with no indication why.

So `wm` sets `WM_BUSY_STARTUP` at startup and clears it once both
`net0` and `repl0` appear in the pid registry -- checked by name rather
than by fixed pid, because registration is what actually signals "up
and listening". While busy, `dock_launch()` refuses:

```
wm: dock: busy, not launching 'term' yet
```

Both dock paths (mouse click and keyboard Enter) go through
`dock_launch()`, so the gating covers each.

## Virtual phosphor modes

The framebuffer is 1bpp, so a pixel is only ever set or clear. What
colour a set pixel *scans out as* is this register's job.

| Mode | Value | Appearance |
|---|---|---|
| `GPU_WHITE` | `00` | white on black (default) |
| `GPU_AMBER` | `01` | amber on black |
| `GPU_GREEN` | `10` | green on black |
| `GPU_PAPER` | `11` | black on white |

These were `ifdef GPU_AMBER` / `ifdef GPU_GREEN` inside
`rtl/gpu/gpu_video.v`: chosen at synthesis and changeable only by
re-flashing gateware. The defines still exist and still work, but they
now choose the **power-on default** only (`rtl/sysctl.v` derives
socctl's reset value from them and passes it in as
`VIDEO_MODE_RESET`), and software can change it at any time afterwards.
A board that used to synthesize green still comes up green.

The colour values themselves are unchanged, on both the VGA and DDMI
paths — including amber's asymmetric per-channel weightings and the
fact that the DDMI path drives `0x80` rather than `0xff` for white.
Those are what this hardware has always produced; adjusting them here
would have altered the look of every existing board while nominally
only adding a feature.

`GPU_PAPER` is the one new mode, and it is not a fourth colour. It is
the white path with the pixel sense inverted, which is why it is
exactly as legible as white rather than approximately so.

### Why it isn't a spare bit in CTRL

`z_cursor_set_busy()` writes CTRL as a whole word. Spare CTRL bits
would therefore be wiped to white on every busy/idle transition in
`wm` — a bug that would look like the display randomly resetting
itself. A read-modify-write in the C helper would fix that and would
be one non-atomic sequence away from doing it again the first time
anything else touched CTRL. A separate register has no such failure
mode.

### Why there is a second signature

MAGIC tells you socctl is present. It does **not** tell you this
register is: socctl shipped before VIDEO existed, and on such a
bitstream the read falls through socctl's `default` case and returns
`0` — bit-for-bit identical to a working block reporting white. So
VIDEO carries `0x5643` (`"VC"`, video colour) in its top half, and
`z_video_mode_present()` checks that rather than MAGIC. Same approach,
and the same reason for it, as `rtl/cache.v`'s `Z_ICACHE_MAGIC`.

### Frame-boundary updates

`gpu_video.v` synchronises the mode into its pixel clock domain with
two flops, then adopts the value **only when the frame counters wrap**.

Two flops alone are not enough for a multi-bit value: the bits can
resolve on different cycles, so a `01` → `10` write can be observed as
`00` or `11` in between. For one pixel clock that is invisible. The
reason to care is that a mid-frame change draws the top of the screen
in one mode and the bottom in another — on `GPU_PAPER`, which inverts,
that is a very visible tear.

So a mode change lands at the next frame boundary: at most 16.7ms,
below the threshold at which a person can tell it from instant.

### Blanking

In `GPU_PAPER` the *inactive* pixel state is 1. An ungated invert would
therefore drive the VGA DAC high through the front porch, sync pulse
and back porch. That is not cosmetic — a monitor reads sync amplitude
to lock, and a "white" blanking interval is how you get a display that
reports no signal at all. `pset` is gated by `is_visible` for exactly
this reason. The other three modes would tolerate the sloppiness; this
one does not.

### Using it

From the kernel shell:

```
> color
color: white
usage: color [white|amber|green|paper]
> color amber
color: amber
```

From Scheme (`repl`):

```scheme
(video-mode)          ; => "white"
(video-mode "green")  ; => "green"
(video-mode 3)        ; => "paper"
```

From C, in an app, via syscall (`sw/common/zeitlos.h`):

```c
uint32_t mode = z_video_mode_get();
bool ok = z_video_mode_set(Z_VIDEO_MODE_AMBER);
```

Or directly, from kernel code (`sw/common/zsoc.h`) — note the
deliberately different names, so it is always clear at the call site
which path is in use:

```c
if (z_video_mode_present()) z_video_set_mode(Z_VIDEO_MODE_PAPER);
```

## Troubleshooting

`wm_busy_apply()` reads the register back after writing and reports it,
because the cursor is the only visible effect and a silent failure
looks identical to "the feature doesn't work":

```
wm: net0 up
wm: repl0 up
wm: core services ready
wm: busy=0x0 cursor=X
```

- `no socctl in this bitstream` -- the gateware predates this block.
  This is an RTL change, so it needs `make flash`, not `make dev-flash`.
- `no video mode register in this bitstream` -- socctl is there but
  predates VIDEO. Also `make flash`. Reported separately from the line
  above because the fix is the same but the diagnosis isn't.
- `color` reports the mode you set but the screen doesn't change --
  the mode is adopted at a frame boundary, so check you aren't looking
  at a display that has lost lock for some other reason. The readback
  comes from the register, which is the SOC's intent, not proof that a
  monitor is showing it.
- `READBACK MISMATCH` -- the write isn't landing. Check the address
  masking above.
- No `net0 up` / `repl0 up` line -- that service never registered, so
  `WM_BUSY_STARTUP` is never cleared and the cursor stays Z forever.
  Check `ps` to see whether the process is even running.
