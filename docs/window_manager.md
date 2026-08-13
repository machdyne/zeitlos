# Zeitlos Window Manager Developer Guide

## Overview

`sw/apps/wm` is the Zeitlos window manager. It owns the screen: it
draws window chrome, tracks focus and z-order, and lets the user drag
windows around by their titlebar. Apps talk to it over the messaging
system (see `docs/messaging.md`) using the protocol in
`sw/common/zwm.h`.

This is a skeleton, matching the phased plan it was built against:

1. **This phase**: window chrome, drag, focus. Apps don't draw their
   own content yet -- wm draws an empty box for every window.
2. **Next**: apps drawing inside their allocated window rect.
3. **After that**: arbitrating shared GPU access between apps drawing
   concurrently (see "Known limitations" below -- this is deliberately
   deferred, not solved here).

## Starting it

The WM is expected to run as a fixed, well-known pid -- see
`Z_PID_WM` in `zwm.h`. There's no dynamic role discovery yet (see
`docs/messaging.md`), so this is a hard assumption: start it right
after boot, before any client app, the same way the ping/pong demo
assumes pong is running as pid 1:

```
> run wm
```

Until a real client app exists, `wm` creates two windows for itself on
startup so there's something to look at and drag immediately.

## Window representation

Each window is drawn as a 5-line box: a 4-line rectangle border plus
one horizontal line separating the titlebar area from the body
(`WM_TITLEBAR_H` pixels tall). There's no resize handle and no
content area rendering yet -- just the outline.

The focused window gets a second, 1px-inset outline drawn on top of
the first (4 extra lines) as a simple "bolder border" focus
indicator, since there's no text rendering wired up yet to do
something like a filled/inverted titlebar.

Window titles are stored (`wm_window_t.title`) and logged to the UART
console when a window is created, but not yet drawn on screen --
rendering text needs font-blitting support that doesn't exist yet.

## Redraw strategy

There's no double buffering and no use of the GPU's blitter yet.
`wm` redraws using **targeted region repair**, not a full-screen
clear -- `repair_region(x, y, w, h)` in `wm.c` clears just that
rectangle of the framebuffer, redraws chrome for every window whose
rect overlaps it, and sends a content-redraw notification only to the
owners of those overlapping windows. Every event (create, destroy,
focus change, drag) calls it with whatever region that specific event
actually affected -- a focus change repairs just the two windows
involved, a destroy repairs just the destroyed window's old
footprint, and so on. Windows elsewhere on screen that aren't near the
change are never touched, so they don't flash.

This replaced an earlier full-screen `clear_screen()` + redraw/notify-
everyone approach, which made *any* change -- even a click that only
changed which window was focused -- make every window on screen
flash, however unrelated. That's a real usability problem, not just
wasted GPU work, so it was worth fixing properly rather than papering
over again.

**Dragging gets its own path, and it matters**: repairing per
mouse-move update (tried first, alongside the old full-clear approach)
queued up `Z_WM_REDRAW` messages faster than apps could drain them, so
content visibly lagged behind the already-finished drag, "playing
back" the movement in slow motion after the fact. So while a button is
held, only the dragged window's own border is erased and redrawn at
each step (`draw_window_box()` with color 0 then color 1) -- no
`repair_region()` call, no other windows touched, no messages sent.
`wm` tracks the full bounding box the window swept through during the
drag (`drag_min_x`/`drag_min_y`/`drag_max_x`/`drag_max_y`), and repairs
exactly that region once, on release -- covering the dragged window's
final content redraw and anything else that may have been visually
clobbered along the path, in one call.

The consequence: while a window is being dragged, its content (and
anything visually underneath the path its border sweeps through) is
frozen in place -- only the border outline moves live, and content
snaps to the new position once you let go. This is a well-worn,
intentionally simple "wireframe drag" pattern (plenty of early window
managers worked this way), and it also means erasing the border at
its old position can leave a visible gap if it happened to overlap
another window's border mid-drag -- purely cosmetic and self-corrects
at the release-time repair.

## Input

Mouse position/buttons are read directly from `reg_usb_cursor`
(bits 9:0 = x, 19:10 = y, 23:20 = button state), scaled down by 2 to
match the 512x384 framebuffer -- same convention `sw/apps/gpudemo`
uses. `wm` polls this once per main-loop iteration; there's no input
event queue yet.

- **Click on a window** brings it to front and focuses it -- but only
  repaints if focus or z-order actually changed (clicking an
  already-focused, already-frontmost window is a no-op, not a
  repaint).
- **Click+drag on a titlebar** moves the window, clamped to stay
  fully on-screen. `wm` sends `Z_WM_WINDOW_MOVED` to the owning app
  once the button is released (not on every intermediate position),
  so apps aren't flooded with move messages mid-drag.
- There's no resize yet, matching the original design goal.

## App protocol

See `sw/common/zwm.h` for the exact subject constants and payload
shapes. Summary:

| Direction | Subject | Payload | Purpose |
|---|---|---|---|
| app → wm | `Z_WM_CREATE_WINDOW` | `Z_MAP{title?, w?, h?}` | request a window |
| wm → app | `Z_WM_WINDOW_CREATED` | `Z_MAP{id, x, y, w, h}` | reply (same `tag` as the request) |
| app → wm | `Z_WM_DESTROY_WINDOW` | `Z_UINT32` (window id) | close a window |
| wm → app | `Z_WM_WINDOW_MOVED` | `Z_MAP{id, x, y, w, h}` | sent after a drag completes |

A client app's request/reply exchange looks like:

```c
z_obj_t args = z_obj_map(2);
z_map_set(&args, "title", z_obj_str("My App"));
z_map_set(&args, "w", z_obj_uint32(160));

z_msg_new_send(Z_PID_WM, Z_WM_CREATE_WINDOW, 0, args);

z_msg_t reply;
z_msg_wait(&reply, Z_WM_WINDOW_CREATED, 0);

int32_t win_id = z_map_find(&reply.obj, "id")->val.int32;
```

`w`/`h` are optional -- omitted keys fall back to
`Z_WM_DEFAULT_WIDTH`/`Z_WM_DEFAULT_HEIGHT`. `x`/`y` can't be
requested yet; `wm` picks placement itself (a simple cascade, see
`create_window()` in `wm.c`) since that was the easiest thing to
implement for this pass and nothing yet depends on apps controlling
initial placement.

The window manager never frees the `Z_MAP` reply object it builds and
sends -- same accepted-leak tradeoff as the ping/pong messaging demo
(see `docs/messaging.md`, "borrowed data has a lifetime"): freeing
immediately after `z_msg_send()` would race with the recipient still
reading it, and there's no ack mechanism yet.

## Content redraw protocol

Apps are responsible for drawing their own window content; the wm only
draws chrome. Since `repair_region()` (see "Redraw strategy" above)
clears whatever region it's given before redrawing chrome into it, it
wipes any content an app has drawn in that region along with the
chrome -- so every `repair_region()` call sends `Z_WM_REDRAW` to the
owner of every window whose rect overlaps that region, telling it to
redraw now. Windows outside the repaired region aren't notified,
because their content was never touched.

`Z_WM_REDRAW`'s payload is a **packed `Z_UINT32`** (see
`Z_WM_PACK_XY`/`Z_WM_UNPACK_*` in `zwm.h`), not a `Z_MAP` like the
other wm messages -- deliberately, and this isn't a minor detail. It
still fires once per completed drag/creation/destruction/focus-change
per affected window, and a fresh `Z_MAP` per broadcast (several heap
allocations, including one per key string) is more than this needs,
given the wm never frees the message objects it sends (see
docs/messaging.md's borrowed-payload lifetime rules). The packed
scalar carries just `(id, x, y)` -- width/height are omitted since
there's no resize support yet, so an app already has them from
`Z_WM_WINDOW_CREATED` and they don't change. `z_win_apply_redraw()` in
`zwin.c` unpacks it.

`Z_WM_WINDOW_CREATED` and `Z_WM_WINDOW_MOVED` are low-frequency (once
per window, once per completed drag) and keep the `Z_MAP` shape --
`z_win_parse_rect()` handles those. An app's message loop needs to
tell these two shapes apart by subject; see `sw/apps/hello_win` for
the pattern.

An app should redraw in response to `Z_WM_REDRAW`/`Z_WM_WINDOW_MOVED`,
and also whenever its own internal state changes independent of the
wm (a clock ticking, data arriving, etc.) -- the wm has no way to know
about the latter, so apps drive those redraws themselves. If an app
does this on a timer (like `hello_win`'s counter), don't just check
messages once before and once after a long delay -- poll in small
chunks throughout it (see `TICK_ITERATIONS`/`POLL_CHUNK` in
`hello_win.c`), or a `Z_WM_REDRAW` arriving mid-wait sits unprocessed
until the whole delay elapses, which is very noticeable if the delay
is more than a fraction of a second.

### Content z-order

Chrome is drawn directly by `wm`, in z-order, so it's always correct.
Content is drawn by each app's own process, asynchronously, whenever
that process happens to get scheduled -- which has nothing to do with
window stacking order. Without something to enforce ordering, a
window behind another one could easily have its content redraw
*after* the front window's, and since there's no depth buffer (just
last-write-wins on the framebuffer), its content would show through on
top of the window that's supposed to be in front of it.

`Z_WM_REDRAW_DONE` (app -> wm, `zwm.h`) fixes this: `repair_region()`
walks its overlapping windows strictly back-to-front, and after
sending `Z_WM_REDRAW` to each one, blocks (`wait_for_redraw_done()` in
`wm.c`) until that specific app acks with `Z_WM_REDRAW_DONE` before
moving on to the next (more frontmost) window. `z_win_redraw_done()`
in `zwin.c` is the app-side call -- `hello_win` sends it right after
finishing the redraw that `Z_WM_REDRAW` triggered, not for redraws it
initiates on its own (the wm isn't waiting on those).

This blocks `wm`'s whole main loop -- no mouse handling, no other
apps' requests serviced by the normal poll -- until the *specific* app
being waited on acks or a timeout (`REDRAW_ACK_TIMEOUT` in `wm.c`,
not a precise time unit) elapses. `wait_for_redraw_done()` does still
call `handle_message()` for anything that isn't the ack it's waiting
for, so other apps' requests aren't silently dropped the way they
would be with `z_msg_wait()` -- but they are *processed reentrantly*,
from inside `repair_region()`, which has a real edge case: if handling
one of those other requests itself calls `repair_region()` again (e.g.
another app's `Z_WM_CREATE_WINDOW` arrives mid-wait), the outer
`repair_region()`'s loop over `zorder`/`zorder_count` can end up
iterating over a table the inner call just mutated. Not crash-unsafe
(the tables are fixed-size, so there's no out-of-bounds access), but
the outer loop could act on a stale index. Rare in practice, not fixed
here -- see "Known limitations".

A slow-to-redraw app therefore stalls the whole UI until its timeout
expires, which is a real cost for a fairly small amount of protocol.
The alternative -- computing each window's actually-visible region
(its rect minus whatever's covering it) and having apps clip to just
that -- would let redraws happen in any order safely, but needs real
multi-rectangle clipping (a window's visible area can be an irregular
shape once more than one window overlaps it), which felt like more
complexity than this phase warranted. Revisit if the stall becomes a
real problem, or once resizing/proper occlusion support is worth
building anyway.

## Drawing content

`sw/common/zgfx.c/h` provides direct-framebuffer pixel/text drawing
(`z_fb_set_pixel`, `z_fb_fill_rect`, `z_fb_draw_char`,
`z_fb_draw_text`), each taking an optional clip rect. These write
directly to the 1bpp framebuffer via ordinary memory writes -- not
through the shared GPU line rasterizer -- which matters for the
GPU-arbitration question flagged below: two apps drawing into two
different, non-overlapping windows can't race on shared hardware
state (like the rasterizer's single clip register) this way, only on
memory, and disjoint memory writes from different processes are
inherently safe. See "Known limitations" for the case this doesn't
cover. (`z_fb_draw_char`/`z_fb_draw_text` can optionally be backed by
the hardware glyph blitter instead of software pixel writes -- see
"Hardware glyph blitting" below -- but the arbitration reasoning above
is specifically about the plain-memory-write path; the blitter is
shared hardware state like the rasterizer is, see that section for
how it's currently scoped to avoid the same problem.)

`sw/common/zwin.c/h` layers window-aware helpers on top:
`z_win_create()` (wraps the `Z_WM_CREATE_WINDOW` exchange),
`z_win_clear()`, and `z_win_draw_text()` -- the latter two always
clip to the window's own content area (below the titlebar, per
`Z_WM_TITLEBAR_H` in `zwm.h`) and use content-relative coordinates
((0,0) is just below the titlebar). This is the sanctioned way for an
app to draw, and it's the mechanism referred to in "apps are trusted"
below: nothing stops an app from calling `zgfx.h` directly with no
clip, or writing to VRAM itself, but an app that only calls `z_win_*`
physically cannot draw outside its own window.

## Hardware glyph blitting

`z_fb_draw_char`/`z_fb_draw_text` have two implementations, selected
at compile time by defining `Z_GFX_HW_BLIT` (`-DZ_GFX_HW_BLIT` in an
app's `CFLAGS`) -- the original software renderer (default, always
available, always correct) or a hardware-accelerated path driving the
GPU blitter (`rtl/gpu/gpu_blit.v`) and a new dedicated glyph memory
(`rtl/mem/glyph.v`). Both implementations live in `zgfx.c` under
`#ifdef Z_GFX_HW_BLIT`; the interface (`zgfx.h`) is identical either
way, so callers (`zwin.c`, apps) don't need to know which is active.

**Why**: software drawing costs roughly one function call + clip
check + shift/mask + read-modify-write per *pixel*. The blitter writes
a whole 32-pixel *word* per Wishbone transaction (a few cycles each).
That's on the order of 100x fewer cycles per pixel for anything
word-parallel, which is most of what text rendering needs (background
fills especially).

**How it fits together**:
- `rtl/mem/glyph.v` is a small (4096-byte, room for both current fonts
  with headroom for more) dual-port BRAM. Port A is a normal
  Wishbone slave at `0x3000_0000` (`GLYPH_MEM_BASE` in `zeitlos.h`) --
  software writes font glyph data here. Port B is a direct,
  non-Wishbone synchronous read port wired straight to the blitter in
  `sysctl.v` -- glyph reads never contend with anything else on the
  bus.
- `gpu_blit_wb` gained a third mode (`CTRL_GLYPH`, alongside the
  existing fill/copy) that blits one glyph: reads its row bytes from
  glyph memory, and for each row does a solid-cell read-modify-write
  into the framebuffer (foreground color where a glyph bit is set,
  background color where it isn't -- proper terminal-cell semantics,
  not a transparent overlay like the software renderer). One trigger
  per character; software loops over a string.
- `z_gfx_hw_font_load(font)` (new in `zgfx.h`, always declared and
  callable) pushes a `z_font_t`'s glyph data into glyph memory. Call
  it once, before the first hardware-accelerated draw with that font.
  It's a documented no-op when built without `Z_GFX_HW_BLIT`, so
  callers don't need their own `#ifdef`.
- The hardware blit is **unclipped by design** -- it doesn't do the
  general partial-word clipping the fill/copy path does. `z_fb_draw_char`
  checks whether the glyph is fully on-screen and (if a clip rect was
  given) fully inside it; if not, it falls back to the same
  per-pixel software path used when `Z_GFX_HW_BLIT` isn't defined at
  all. In practice this only affects glyphs that would land partially
  outside a window's edge, which shouldn't be common if windows are
  sized to fit whole character cells.
- **Bit order matters and was a real source of bugs earlier in this
  project** (see the font orientation notes above) -- worth being
  extra careful here. Framebuffer words have increasing x = increasing
  bit position; font bytes are MSB-first (bit 7 = leftmost pixel).
  `gpu_blit.v`'s glyph path explicitly bit-reverses each glyph byte
  before use (`g_byte_rev` in the RTL) to reconcile this. If hardware
  text ever renders mirrored, this is the first place to check.
- A found-and-fixed bug along the way: the *existing* fill/copy
  clipped-fill path referenced `read_data` for partial-word masking
  without ever populating it (the clipped-fill branch skipped straight
  to `ST_WRITE`, never `ST_READ`) -- meaning any clipped rectangle
  fill that didn't land on exact 32-pixel word boundaries (i.e. almost
  any real window-sized rectangle) would have masked against
  stale/undefined data. Fixed as part of this work, independent of the
  glyph blitting itself.

**This is genuinely untested** -- I don't have access to your FPGA
toolchain or hardware, so none of `glyph.v`, the `gpu_blit.v` glyph
extension, or the `sysctl.v` wiring has been simulated or run. A
staged bring-up is strongly recommended rather than trusting all of it
at once:
1. Build with the fill/copy bugfix alone (no `MEM_GLYPH`/glyph mode
   changes exercised yet) and confirm existing rectangle fills
   (`z_win_clear()`, etc.) still look correct -- this validates the
   `ST_READ` routing fix didn't regress the working paths.
2. Bring up `glyph.v` in isolation: write a few known bytes via its
   Wishbone port, read them back, confirm round-trip correctness,
   before wiring it to the blitter at all.
3. Trigger a glyph blit with a synthetic, easy-to-recognize pattern
   (e.g. a solid square, or a diagonal-line test glyph) rather than
   real font data first, and confirm it lands at the right pixel
   position with the right dimensions before trusting orientation.
4. Only then load real font data (`z_gfx_hw_font_load()`) and confirm
   actual character shapes render correctly and right-side up.
5. `MEM_GLYPH` must be defined (RTL) for any bitstream that software
   built with `Z_GFX_HW_BLIT` will run on. `gpu_blit_wb`'s
   `glyph_addr_o`/`glyph_data_i` ports are always connected in
   `sysctl.v` regardless of `MEM_GLYPH` -- without it, `glyph_data_i`
   is tied to a constant `0` rather than real glyph memory (so the
   build stays clean either way), meaning glyph-mode blits would just
   read all-zero glyph data if triggered without `MEM_GLYPH` actually
   built. Software and RTL feature flags still need to agree for
   glyph rendering to do anything meaningful.

**Assumptions worth double-checking against your actual conventions**,
since I made judgment calls without being able to verify them: the
`0x3000_0000` base address for glyph memory (chosen because it was
the only free top-nibble slot in `sysctl.v`'s existing decode scheme
next to `0x2` VRAM -- there was no established convention to follow);
and the register layout at offsets 7-11 in `gpu_blit_wb` (picked
because 0-6 were already taken, no other constraint). The RTL feature
flag is `MEM_GLYPH`, matching the existing `MEM_SRAM`/`MEM_SDRAM`/
`MEM_QQSPI`/`MEM_VRAM`/`MEM_ROM` naming convention already used in
`sysctl.v` for other memories.


The font (`sw/common/zfont.h/zfont_data.c`) supports multiple bitmap
fonts through a common `z_font_t` struct (width, height, codepoint
range, glyph data) -- `zgfx`/`zwin`'s drawing calls take a font
pointer rather than assuming one. Currently defined: `z_font_8x16`
(from `sw/data/font/font16.mem`) and `z_font_6x12` (from
`sw/data/font/font6x12.mem`, added for dense text -- 6x12 is small
enough to fit an 80x25 terminal grid in a reasonably-sized window). A
6x6 font is anticipated for even denser use cases; adding one is just
a new `.mem` source plus a regeneration, no code changes needed
elsewhere, given the struct is already generic over glyph dimensions.

`zfont_data.c` is generated by `sw/data/font/gen_font_data.py`, not
hand-written -- regenerate it (`python3 sw/data/font/gen_font_data.py`
from the repo root) rather than editing it directly if a `.mem` source
changes or a new font is added.

**Orientation is source-specific, not a fixed convention** -- worth
knowing if you add a third font. `font16.mem` (flat format) lists
rows top-to-bottom already; `font6x12.mem` (commented format) lists
them bottom-to-top and needs the reversal the generator applies. Both
sources list bits left-to-right (no reversal needed there). If a new
font renders upside down, reversed, or both, check row order and bit
order for that specific source independently -- don't assume they
match an existing font's convention. `gen_font_data.py`'s two parsers
(`parse_flat`/`parse_commented`) each apply whichever fix their source
needs; verify against an asymmetric glyph (a letter like 'F', 'L', or
'P' -- symmetric ones like 'X' or 'O' won't tell you anything) before
trusting a new source.

See `sw/apps/hello_win` for a complete minimal example: create a
window, draw text into it, redraw on `Z_WM_REDRAW`/`Z_WM_WINDOW_MOVED`
and on its own periodic tick.

## App trust model

Apps in Zeitlos are fully trusted -- there's no memory protection
between processes (see docs/messaging.md's discussion of the flat
physical memory model), so nothing at the OS level actually prevents
an app from drawing outside its window. The `zwin.h` API above is the
*compliance mechanism*: it's the easy, natural way to draw, and it
enforces the window boundary for you, but it's a convention backed by
a convenient API, not a hard guarantee.

## Known limitations / future work

- **`repair_region()` reentrancy during an ack wait.** Covered in
  detail under "Content z-order" above: `wait_for_redraw_done()`
  processes other apps' requests (including ones that call
  `repair_region()` again) while blocked waiting for one app's ack,
  so an outer `repair_region()` call's loop over `zorder` can end up
  iterating a table an inner, nested call just changed. Not memory-
  unsafe, but could act on a stale window index in that rare case.
- **A slow or unresponsive app stalls the whole wm** while
  `wait_for_redraw_done()` waits for its ack, up to
  `REDRAW_ACK_TIMEOUT`. See "Content z-order" above for why this
  tradeoff was made (it's what makes content z-order correct) and
  what a fuller fix would need.
- **A residual race between wm-triggered and app-driven redraws.** An
  app that redraws on its own schedule (not just in response to
  `Z_WM_REDRAW`/`Z_WM_WINDOW_MOVED`, like `hello_win`'s counter tick)
  can lose a race with the wm clearing its window's region: if the
  app's own redraw fires in the gap between the wm's `repair_region()`
  clearing that area and the app actually receiving/processing the
  resulting `Z_WM_REDRAW`, it draws at a stale cached position, and
  that stale copy is never cleaned up (nothing else knows to clear
  it). `hello_win` mitigates this by polling its mailbox in small
  chunks throughout its tick delay (`TICK_ITERATIONS`/`POLL_CHUNK` in
  `hello_win.c`) rather than only at the very start and end of it --
  this was also what fixed a much more obvious symptom, multi-second
  latency between a window moving and its content catching up, since
  a message arriving mid-wait used to sit unprocessed until the whole
  delay elapsed. The race itself is only narrowed, not eliminated: it
  still shrinks to roughly one poll-chunk's worth of time rather than
  the whole tick interval. A complete fix needs an explicit ack
  protocol between wm and app. If you see duplicate/stale text after
  moving a window, or a delay before content catches up, this is why
  -- try a smaller `POLL_CHUNK` first.
- **The focused-window bold border can still get drawn over.**
  `zwin.c`'s content clip insets 1px from the window's outer edge, far
  enough to never overlap the *regular* (unfocused) border -- but the
  focused-window indicator (`draw_window_box()`'s extra 1px-inset
  outline in `wm.c`) sits exactly on that same inset boundary, so
  content that reaches all the way to its own edge could still paint
  over it. `hello_win` never hits this in practice (it leaves a 4px
  margin), but nothing enforces that margin. A 2px inset would close
  this too, at the cost of slightly less usable content area; not
  done since it wasn't the reported problem.
- **No GPU arbitration for the line rasterizer/blitter.** Direct
  framebuffer writes (what `zgfx`/`zwin` use for text) sidestep this
  for content drawn that way, since they don't touch shared hardware
  registers -- but if an app ever wants to use the line rasterizer or
  blitter for its own content (not just `zgfx` pixel writes), the
  original problem (two processes racing on the rasterizer's single
  clip register) is back. Still unsolved; app-level locking was the
  suggested direction, not yet implemented.
- **A subtler framebuffer race for overlapping/adjacent windows.**
  `z_fb_set_pixel()`'s read-modify-write of a framebuffer word
  (`VRAM[word_index] |= mask`) isn't atomic. Two non-overlapping
  windows can still share a framebuffer word if they're within 32
  pixels of each other horizontally (words are 32 pixels wide and
  don't align to window boundaries), and two apps writing to that
  shared word concurrently could clobber each other's bit if
  preempted mid-update. Not addressed yet -- would need either
  word-aligned window placement or a locking/masking scheme around
  framebuffer writes.
- **No process-death cleanup.** If an app that owns a window is
  killed, `wm` has no way to find out and will leave its window (and
  window-table slot) around forever. This needs either a kernel
  notification mechanism or a way for `wm` to poll whether a pid is
  still alive -- neither exists yet (see `docs/messaging.md`).
- **Titles are still not drawn** in the chrome itself (font support
  now exists via `zgfx`, but `wm.c` draws chrome purely via the line
  rasterizer and hasn't been updated to render the title text yet).
- **No app-requested placement or resize.**
- **Placement cascade is intentionally minimal** -- it just offsets
  each new window slightly from the last; no collision avoidance,
  centering, or multi-monitor concerns (not applicable here) were
  considered.
