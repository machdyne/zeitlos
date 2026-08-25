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
- `READBACK MISMATCH` -- the write isn't landing. Check the address
  masking above.
- No `net0 up` / `repl0 up` line -- that service never registered, so
  `WM_BUSY_STARTUP` is never cleared and the cursor stays Z forever.
  Check `ps` to see whether the process is even running.
