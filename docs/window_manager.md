# Zeitlos Window Manager Developer Guide

## Overview

`sw/apps/wm` is the Zeitlos window manager. It owns the screen: it
draws window chrome, tracks focus and z-order, and lets the user drag
windows around by their titlebar. Apps talk to it over the messaging
system (see `docs/messaging.md`) using the protocol in
`sw/common/zwm.h`.

This was built against a phased plan, all three phases now done:

1. Window chrome, drag, focus.
2. Apps drawing their own content inside their allocated window rect
   (`hello_win`, `gpu3d`, `gpudemo` -- see "Drawing content" below).
3. Arbitrating shared GPU access between apps drawing concurrently,
   for the line rasterizer specifically -- see `docs/app_runtime.md`,
   "The GPU line rasterizer" for the full story (two real hazards,
   both closed by moving that access behind `zgfx.c`'s IRQ-masked
   `z_fb_hw_line()`), and "Known limitations" below for what's still
   open (the blitter has its own, different, not-yet-unified scoping).

See `docs/app_runtime.md` for the broader app-runtime picture this
all sits within (`zeitlos.h/c`, the syscall trampoline, direct
hardware register access, `zgfx.c`) -- this document stays focused on
the window manager's own protocol and `zwin.c`'s window-relative
drawing helpers.

## Starting it

The WM registers itself by name (`"wm0"`, via the pid name registry --
see `docs/app_runtime.md`'s syscall table and `sw/os/pidreg.c/h`) at
startup, so client apps (`zwin.c`'s `z_win_create()`) can find it by
name rather than assuming a fixed pid. It still needs to actually be
running before any client app tries to create a window (they'll fail
to connect otherwise), and `zwin.c` still falls back to the fixed
`Z_PID_WM` constant (`zwm.h`) if the name lookup fails -- e.g. an old
`wm` build that predates the registry -- so starting it right after
boot, before any client app, remains the right convention even though
it's no longer a hard requirement the way it used to be:

```
> run wm
```

Until a real client app exists, `wm` creates two demo windows for
itself on startup so there's something to look at and drag
immediately, plus the dock (see "The dock" below) with launchers for
the two real apps that do exist so far (`term`, `gpu3d`).

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

Mouse position/buttons are read via `mouse_port()` (`wm.c`), which
picks whichever of the two USB HID ports currently reports itself as
a mouse -- there's no fixed port-to-device mapping anymore, and no
guarantee it's the same port from one boot to the next. See
`docs/user_input.md` for the dual-port hardware and register layout
this reads from; this section stays focused on what `wm` does with
the result. `wm` polls this once per main-loop iteration; there's no
mouse event queue.

- **Click on a window** brings it to front and focuses it -- but only
  repaints if focus or z-order actually changed (clicking an
  already-focused, already-frontmost window is a no-op, not a
  repaint).
- **Click+drag on a titlebar** moves the window, clamped to stay
  fully on-screen. `wm` sends `Z_WM_WINDOW_MOVED` to the owning app
  once the button is released (not on every intermediate position),
  so apps aren't flooded with move messages mid-drag.
- There's no resize yet, matching the original design goal.

Keyboard input is interrupt-driven, not polled -- see
`docs/user_input.md` for the full stack (kernel capture, keysym
translation) and `Z_WM_KEY` below for what reaches an app. Unlike the
mouse, keyboard events only ever go to the *focused* window's owner;
a window with no mouse available to click still gets focus once, on
creation, if nothing else is already focused (also covered in
`docs/user_input.md`) -- otherwise a keyboard-only session would have
no way to focus anything at all. See "Keyboard-only operation" below
for everything else that adds -- Alt+Tab, Alt+Arrow, and the dock's
own keyboard navigation, all of which now cover the same ground the
mouse does.

## Keyboard-only operation

Keyboard-only use is a first-class case, not an afterthought -- Zeitlos
has no requirement that a mouse be plugged in at all. Everything a
mouse can do to `wm` itself (focus a window, move it, launch an app
from the dock) has a keyboard equivalent, all handled directly in
`dispatch_keys()` (`wm.c`) and never forwarded to any app's own
`Z_WM_KEY` stream:

- **Alt+Tab** cycles focus to the next window, dock included (see
  "The dock" below for why the dock counts as a focusable item now,
  not just an always-on-top overlay) -- `next_focusable()` walks
  `windows[]` in fixed SLOT order (not z-order: the dock is kept
  frontmost in z-order at all times via `bring_to_front(dock_idx)`
  calls scattered through this file, which would otherwise dominate/
  distort a z-order-based cycle), wrapping around, and skips unused
  slots. The newly-focused window is brought to front exactly the way
  a mouse click on it already would be -- `alt_tab()` mirrors that
  code path rather than introducing a second one.
- **Alt+Arrow** moves the *focused* window by `WM_KEY_MOVE_STEP` (10)
  pixels in that direction, clamped to the screen the same way a mouse
  drag already is -- a no-op if the dock is what's focused (its
  position is fixed, see `create_dock()`). Implemented as an instant,
  single-step version of a mouse drag: `alt_move_focused()` updates
  the window's position directly, then hands the before/after
  bounding box to `repair_drag()` -- the exact same sweep-region
  repair a real drag-release already uses (see "Redraw strategy"
  above) -- rather than re-deriving that logic.
- **The dock is now focusable** (Alt+Tab reaches it like any other
  window) and, while it has focus, Left/Right/Up/Down move a selection
  cursor between icons (wrapping around) and Enter launches the
  selected one -- see `dock_handle_key()`. This reuses the exact same
  `dock_launch()` codepath a mouse click already goes through (see
  below), just reached a different way; both keyboard Enter and a
  mouse click on an icon are just two calls into the same function
  now, not two separate implementations. A small 1px-outset selection
  ring (`draw_dock_selection_ring()`, same visual language as a
  focused *window's* own outset border -- see `draw_window_box()`)
  shows which icon is selected, but only while the dock itself
  actually has keyboard focus -- it's not drawn at all otherwise, so
  it can't be mistaken for anything else on screen.
- **Dock launch feedback.** Whichever way an app gets launched from
  the dock -- mouse click or keyboard Enter -- its icon is drawn
  *inverted* (`dock_launching[]`, `draw_icon_bitmap_inverted()`: a
  solid-filled slot with the icon's own shape cut out of it, rather
  than lit up against a dark slot) starting BEFORE `z_proc_run()` is
  even called, not after it returns -- `z_proc_run()`
  (`sw/os/kernel.c`'s `k_proc_run()`) blocks wm's own process for as
  long as it takes to load the app's entire binary off the
  filesystem, which is the actual slow part of "launching" -- drawing
  the inverted icon only after that call returns would mean the
  invert only ever covered the (usually imperceptibly fast)
  remainder: the newly-started process's own C runtime init plus its
  first `z_win_create()` call, with no disk I/O left in it by that
  point. `dock_launch()`'s own framebuffer write lands in VRAM and
  stays there, visible on screen, even while wm sits blocked inside
  `z_proc_run()` right after -- the display scans out from VRAM
  independently of whichever process the CPU happens to be running.
  The inverted state is cleared either when the launched pid creates
  its first window (`handle_message()`'s `Z_WM_CREATE_WINDOW` case,
  matched by pid, not by which icon was clicked -- an app could in
  principle create its window from a different code path than the one
  that "feels" tied to the click, but pid is the only identity wm
  actually has to go on) or immediately if `z_proc_run()` itself
  fails synchronously (file missing, no free process slot -- no pid
  will ever exist to clear it the normal way). While an icon is
  inverted, launching it again (from either input method) is a no-op
  -- `dock_launch()`'s own `dock_launching[slot]` check -- so an
  impatient double-click or a held-down Enter can't spawn a
  slow-loading app twice before its first window ever shows up. A
  generous iteration-count timeout (`DOCK_LAUNCH_TIMEOUT_ITERS`,
  checked once per main-loop iteration) clears the inverted state
  anyway if a launched process starts but never creates a window at
  all (crashes early, isn't a GUI app, etc) -- otherwise a single bad
  launch would permanently strand that icon.

## App protocol

See `sw/common/zwm.h` for the exact subject constants and payload
shapes. Summary:

| Direction | Subject | Payload | Purpose |
|---|---|---|---|
| app → wm | `Z_WM_CREATE_WINDOW` | `Z_MAP{title?, w?, h?}` | request a window |
| wm → app | `Z_WM_WINDOW_CREATED` | `Z_MAP{id, x, y, w, h}` | reply (same `tag` as the request) |
| app → wm | `Z_WM_DESTROY_WINDOW` | `Z_UINT32` (window id) | close a window |
| wm → app | `Z_WM_WINDOW_MOVED` | `Z_MAP{id, x, y, w, h}` | sent after a drag completes |
| wm → app | `Z_WM_KEY` | packed `Z_UINT32` (`Z_WM_PACK_KEY`) | key press/release, focused window only -- see `docs/user_input.md` |

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

`repair_region()` takes an `exclude_idx` parameter for one specific
case: right after `create_window()`, the brand-new window's own chrome
needs drawing (and repair_region() is what does it) before its owner
gets its `Z_WM_WINDOW_CREATED` reply -- otherwise the owner's
`z_win_create()` can return, and its first drawing calls can run,
before the wm has drawn so much as a border for it. But that same new
window's owner can't be sent `Z_WM_REDRAW`/waited on for an ack the
normal way, since it's still blocked waiting for the very reply that
hasn't been sent yet -- its `z_msg_wait()` would silently discard the
`Z_WM_REDRAW` (it doesn't match what it's waiting for), and
`wait_for_redraw_done()` would stall for the full timeout on every
single window creation. `exclude_idx` skips just the notify+wait step
for that one window (chrome still gets drawn) while behaving normally
for every other window `repair_region()` touches.

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

## The dock

A small always-on-top launcher bar, anchored to the bottom left of
the screen, with one 32x32 icon per app (`sw/apps/wm/wm.c`,
`dock_apps[]`). Currently hardcoded to two apps, `term` and `gpu3d`;
adding a third is one more `{ "name", "label" }` entry in
`dock_apps[]`, nothing else. `name` is the bare filename `z_proc_run()`
(see `docs/app_runtime.md`) expects -- no path, no extension, same as
what you'd type after `run` at the kernel shell.

It's a real entry in `wm`'s own `windows[]`/`zorder`, owned by `wm`'s
own pid (`my_pid`, not the `Z_PID_WM` constant -- see "Starting it"
above) like the two demo windows created alongside it in `main()` --
not a special-cased overlay bolted on outside the normal window
system. That was a deliberate choice: reusing `repair_region()`'s
existing overlap-based dirty-region tracking means the dock gets
correctly redrawn whenever something overlaps it, and correctly
avoids touching/notifying anything it doesn't overlap, for free,
instead of needing its own parallel redraw path. Three things make it
behave differently from an ordinary window, all driven by small,
targeted flags/checks rather than forking the window struct or the
main loop:

- **No titlebar.** `wm_window_t.no_titlebar` (general-purpose, not
  dock-specific) skips the titlebar separator line in
  `draw_window_box()` and makes `hit_titlebar()` always return
  `false` for that window -- so clicking anywhere in the dock can
  never start a drag.
- **Always frontmost.** `create_dock()` is called last in `main()`
  (after the demo windows), so it starts out frontmost by
  construction (`create_window()` always appends to the front of
  `zorder`). Every other place `zorder` can change -- a window being
  raised by a click (`main()`'s click handling) or a new window being
  created (`handle_message()`'s `Z_WM_CREATE_WINDOW` case) -- follows
  up with `bring_to_front(dock_idx)`, so nothing can end up drawn on
  top of it. This is the only reason clicking inside the dock's rect
  reliably resolves to the dock in `hit_test()` (which walks `zorder`
  back-to-front and returns the first match) without needing a
  separate, earlier check ahead of the normal hit-test call.
- **Content is drawn synchronously, in-process.** `draw_dock()` is
  called directly from `repair_region()`'s per-window loop, right
  next to `draw_window_box()` -- not via `Z_WM_REDRAW` +
  `wait_for_redraw_done()` like every other window's content. That
  messaging round trip exists to let `repair_region()`'s
  back-to-front ordering guarantee hold (see "Content z-order" above)
  when content is drawn by a *different* process that might not be
  scheduled for a while; the dock's content is drawn by `wm` itself,
  in the same call, so there's nothing to wait for and no ordering
  gap to close. (Same reasoning already applied to the demo windows,
  which just never draw any content at all -- the dock is the first
  wm-owned window with real content of its own.)

Icon content is real per-app pixel art: 32x32 1bpp bitmaps (the
framebuffer itself is 1bpp, see `zgfx.h`), generated from source PNGs
by `sw/data/icons/gen_dock_icon_data.py` into `sw/apps/wm/dock_icons.c`/
`.h` (checked in, not generated at build time -- see that script's own
header comment for the full "add a new icon" steps and source-PNG
requirements: exactly 32x32, exactly two colors, no anti-aliasing).
`draw_dock()`'s `draw_icon_bitmap()` blits one via `z_fb_set_pixel()`
per bit -- not the hardware blitter (that's for glyph/font data
specifically, see below), and not a hot path (dock icons only redraw
when `repair_region()` finds the dock's rect overlapping something,
not continuously), so the straightforward per-pixel software path is
the right one here, same reasoning as `z_fb_set_pixel()`'s general
default-case status in `zgfx.h`.

Text -- the "Dock" window title, not currently drawn anywhere per
"Known limitations" below, so nothing dock-related actually uses text
right now -- would go through the hardware glyph blitter if it ever
does (`wm`'s Makefile builds with `-DZ_GFX_HW_BLIT`, like
`hello_win`/`term`) -- see "Hardware glyph blitting" below for how
that path works, and for why `wm` loading `z_font_5x8` exactly once,
itself, in `main()`, is now the *only* place any font is ever loaded
into hardware glyph memory board-wide.

Clicking a slot calls `z_proc_run()` (see `docs/app_runtime.md`'s
syscall table) and always spawns a fresh process, the same as running
`run <app>` twice from the shell would -- there's no tracking of
whether an app is "already running" to focus instead. Revisit if that
turns out to matter in practice.

One placement note: the dock sits at a fixed `y` close to
`WM_SCREEN_H` (see `DOCK_MARGIN`/`DOCK_ICON_SIZE` in `wm.c`), while
every other window still uses the plain top-left cascade described
under "App protocol" above (`x = 20 + (n % 8) * 24`, `y = 20 + (n % 8)
* 20`, capped at `y = 160` for the first 8 windows) -- so in practice
new windows don't reach far enough down the screen to land on the
dock. Nothing currently *enforces* that the way it does for the
screen's own edges (see "Placement cascade is intentionally minimal"
under "Known limitations"); it just happens to hold given the current
cascade formula and app window sizes.

## Drawing content

`sw/common/zgfx.c/h` provides direct-framebuffer pixel/text drawing
(`z_fb_set_pixel`, `z_fb_fill_rect`, `z_fb_draw_char`,
`z_fb_draw_text`), each taking an optional clip rect -- ordinary
memory writes to the 1bpp framebuffer, so two apps drawing into two
different, non-overlapping windows can't race on anything here, only
on memory, and disjoint memory writes from different processes are
inherently safe. See "Known limitations" for the case this doesn't
cover.

`zgfx.c/h` also provides `z_fb_hw_line()`/`z_fb_hw_box()`, driving the
shared GPU line rasterizer (`rtl/gpu/gpu_raster.v`) directly, and
`z_fb_hw_fill_rect()`, driving the GPU blitter's fill mode
(`rtl/gpu/gpu_blit.v` -- see `docs/gpu_blitter.md`) -- unlike the
memory-write functions above, both of these *are* shared, global
hardware state with no per-process isolation, and used to be (the
rasterizer) or still partly is (the blitter's glyph path, see below)
a real source of bugs. See `docs/app_runtime.md`, "The GPU line
rasterizer" and "The GPU blitter" for the full story on each: the
same two hazards each time (a shared clip/bounds state that whoever
drew most recently determines for everyone else, and a
register-writes-then-trigger sequence that isn't atomic), closed the
same way (IRQ-masked atomicity, coordinates clamped to the actual
screen bounds unconditionally, state reasserted fresh on every call
rather than assumed to still be correct). `wm.c` itself draws its
chrome through these now -- fills/clears via `z_fb_hw_fill_rect()`,
borders/titlebar separators via `z_fb_hw_line()`/`z_fb_hw_box()`,
both unclipped (`clip=NULL`) since it already knows its own
coordinates are valid; `gpu3d`/`gpudemo` draw through the window-aware
`z_win_hw_line()`/`z_win_hw_box()` wrappers described next. `wm.c`
switching its own `fill_rect()`/`clear_screen()` from a software VRAM
loop to `z_fb_hw_fill_rect()` wasn't just a speed win -- see
`docs/app_runtime.md`'s note on why a long, tight, uninterrupted
software loop was itself implicated in a real crash.

`sw/common/zwin.c/h` layers window-aware helpers on top:
`z_win_create()` (wraps the `Z_WM_CREATE_WINDOW` exchange),
`z_win_clear()`, `z_win_fill_rect()`, and `z_win_draw_text()` -- these
four always clip to the window's own content area (below the
titlebar, per `Z_WM_TITLEBAR_H` in `zwm.h`) and use content-relative
coordinates ((0,0) is just below the titlebar). `z_win_hw_line()`/
`z_win_hw_box()` do the same for the hardware rasterizer path, though
note the coordinate convention differs -- see their own doc comments
in `zwin.h`, since apps drawing through the rasterizer (a 3D
projection, say) typically already compute absolute screen
coordinates themselves, so these only take over clip-region/IRQ-mask
management, not coordinate translation. `z_win_content_rect()`
exposes the content-area rectangle these all compute internally, for
any app that needs to know its own drawable bounds for something else
(centering content, bouncing something off the edges) without
duplicating that formula itself -- which is exactly what caused a
real, shipped bug once already (`gpu3d`/`gpudemo` each kept their own
copy, and it silently fell out of sync with this file's own version
after a border-inset fix here). This is the sanctioned way for an app
to draw, and it's the mechanism referred to in "apps are trusted"
below: nothing stops an app from calling `zgfx.h` directly with no
clip, or writing to VRAM itself, but an app that only calls `z_win_*`
physically cannot draw outside its own window.

See `docs/app_runtime.md` for the full app-runtime picture this all
sits within (`zeitlos.h/c`, the syscall trampoline, process startup)
-- this document stays focused on the window manager's own protocol.

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
- **Glyph memory is shared, global hardware state -- one font's data
  at a time, board-wide, with no per-process isolation.** Every
  `Z_GFX_HW_BLIT` process used to call `z_gfx_hw_font_load()` itself
  at its own startup, which only worked by accident: nothing
  arbitrated whose call "won" if two processes using different fonts
  were both running, and there was no reload-before-draw discipline
  either, so a later process's `z_gfx_hw_font_load()` call for a
  *different* font could silently corrupt an already-running
  process's hardware-blitted text. Current convention, adopted
  specifically to close this rather than just narrow it: `wm` (see
  "The dock" above) is now the *only* process that ever calls
  `z_gfx_hw_font_load()` -- once, at its own startup, for
  `z_font_5x8` -- and every other app (`hello_win`, `term`) is
  expected to only ever draw with `z_font_5x8`, never loading a font
  of its own. This isn't enforced anywhere in code -- a
  `Z_GFX_HW_BLIT` app that calls `z_gfx_hw_font_load()` with a
  different font, or that's built to draw with one (`term`'s
  `FONT=z_font_6x12` build option still exists, see its own comment
  in `term.c`), will still corrupt glyph memory for everyone. It's a
  convention, not a guarantee, same category of tradeoff as the app
  trust model below.
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

## Window titlebar icons

Every window's titlebar (see "Window representation" above,
`Z_WM_TITLEBAR_H`) now shows its title text on the left (drawn with
`z_font_5x8` via `z_fb_draw_text()`, same hardware glyph blitter as
everything else in this section) and, optionally, a small close icon
on the right.

**Icons live in the same hardware glyph memory as font data, in a
separate reserved region** -- see `sw/common/zicon.h`. Font glyphs
occupy the *front* of glyph memory (offset 0, `z_gfx_hw_font_load()`,
unchanged from before); window icons occupy a small, fixed-size
region at the very *end* (`Z_ICON_MEM_OFFSET`, currently 256 bytes --
32 slots of 8x8 1bpp each). This split means adding or resizing
window icons can never disturb font glyph addressing, and the two
regions can only collide if font data itself grows to consume nearly
all of glyph memory, which none of the currently-defined fonts come
close to. `z_gfx_hw_icon_load()`/`z_fb_draw_icon()` (`zgfx.h`) are the
loading/drawing primitives -- same `CTRL_GLYPH` hardware blit mode
`z_fb_draw_char()` uses, just addressed into the icon region instead
of the font region. Like the font, **`wm` is the sole owner**: it
loads every window icon once, at its own startup
(`z_win_icons_load()`, `sw/apps/wm/win_icons.c`), right after loading
`z_font_5x8` -- same single-owner discipline "Hardware glyph
blitting" above already established for the font, extended to cover
icons too, for the same reason (nothing else ever writes to glyph
memory, so there's nothing to race over).

**Window icon bitmaps are hand-edited, not generated** (unlike dock
icons, `sw/apps/wm/dock_icons.c`, which come from
`sw/data/icons/gen_dock_icon_data.py` and a source PNG) --
`win_icons.c` uses binary literals (one `0b`-prefixed byte per glyph
row) specifically so an 8x8 icon can be eyeballed and tweaked
directly as bits, without round-tripping through an image editor and
a generator script for something this small. The one currently
defined, `Z_ICON_CLOSE` (`zicon.h`), is a hollow box -- deliberately
NOT an X, since the mouse cursor itself is drawn as an X
(`rtl/gpu/gpu_cursor.v`) and a same-shaped close icon under the
pointer would be genuinely hard to read, not just an aesthetic
mismatch. Adding a new window icon (minimize, open file, save file,
...) is: append an id to `z_icon_id_t` (`zicon.h`, before
`Z_ICON_ID_COUNT`), add its 8-byte bitmap to `win_icons.c`, and add
one `z_gfx_hw_icon_load()` call for it in `z_win_icons_load()`.

**Whether a window gets a close icon at all, and what clicking it
does, is opt-in per window** via a `Z_WIN_FLAG_*` bitmask (`zwm.h`)
passed to `Z_WM_CREATE_WINDOW`'s new `flags` key -- app-side entry
point is `z_win_create_flags()` (`zwin.h`), which `z_win_create()`/
`z_win_create_ex()` now both just call with `flags=0` (no icon), so
every caller written before this feature existed is unaffected.
- `Z_WIN_FLAG_CLOSE_ICON` -- draw the close icon at all. Without it
  (the default), no icon, and the titlebar can't be clicked closed.
- `Z_WIN_FLAG_CLOSE_KILLS_OWNER` -- meaningless without the flag
  above. **Set**: clicking the icon makes `wm` destroy the window
  AND kill the owning process outright (`z_proc_kill()`, a new
  syscall -- see below), no message round trip, no chance for the
  app to ignore it. Only correct for an app that owns exactly one
  window for its whole lifetime -- `term`, `hello_win`, `gpu3d`, and
  `gpudemo` all opt into this combination now. **Clear** (the
  default when `Z_WIN_FLAG_CLOSE_ICON` is set alone): `wm` instead
  sends the window's owner a new `Z_WM_CLOSE` message (a `Z_UINT32`
  window id, fire-and-forget, same convention as `Z_WM_REDRAW`/
  `Z_WM_KEY`) and does nothing else on its own -- the window stays
  open and interactive until/unless the owner itself calls
  `z_win_destroy()` on that specific id. This is the right choice
  for any app that can own MORE than one window at a time off a
  single pid: `repl`'s Scheme `win-create` (`docs/scheme_api.md`,
  `sw/apps/repl/zapi.c`) is exactly that case (a single `repl`
  process can have several Scheme-created windows open
  simultaneously, tracked in `zapi_windows[]`) -- setting
  `Z_WIN_FLAG_CLOSE_KILLS_OWNER` there would take down `repl` itself,
  and every other window it owns, the instant any ONE of them was
  clicked closed. `repl.c`'s main loop now handles `Z_WM_CLOSE` by
  calling `zapi_win_close()` (`zapi.h`/`zapi.c`), which just destroys
  the one matching `zapi_windows[]` entry -- same bookkeeping the
  Scheme-facing `(win-destroy id)` already does, just reachable
  directly from the message loop instead of only from Scheme code.

**Click handling**: `wm.c`'s `hit_close_icon()` is checked BEFORE the
general titlebar-drag/focus-change handling in the main loop's click
dispatch, so clicking the icon closes the window instead of starting
a drag or changing focus first. `close_icon_rect()` computes the
icon's on-screen rect once and is shared between the hit test and the
actual draw (`draw_titlebar_content()`), the same "compute once,
share everywhere" reasoning `z_win_content_rect()`'s own comment
gives for the identical class of bug.

**Titlebar text/icon are drawn separately from the border**, in
`draw_titlebar_content()`, called from `repair_region()` right after
`draw_window_box()` -- NOT from the wireframe-drag block in `main()`,
which calls `draw_window_box()` directly (color=0 then color=1) on
every intermediate drag step to move just the border cheaply, while
content stays frozen by design (see "Redraw strategy" above). Title
text and the close icon are exactly that kind of content, not
border -- folding them into the per-drag-step border redraw would
have defeated the entire point of the wireframe-only move for zero
visual benefit, since neither moves relative to the border anyway.

**New syscall: `Z_SYS_PROC_KILL`** (`syscalls.def`,
`k_proc_kill_syscall()` in `kernel.c`, `z_proc_kill()` in
`zeitlos.h/.c`) -- lets any userland process kill another by pid, the
same way the existing `Z_SYS_PROC_RUN` (added earlier for the dock,
see "The dock" above) let `wm` start one. No ownership/permission
check, same "apps are fully trusted" model as everything else here
(see "App trust model" below) -- wm uses this on a window's
`owner_pid` when `Z_WIN_FLAG_CLOSE_KILLS_OWNER` is set and its close
icon is clicked. The underlying kernel-internal `k_proc_kill()`
(used directly by `sh.c`'s `kill` shell command) is unchanged; this
just exposes the same mechanism as a syscall.

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
- **~~The focused-window bold border can still get drawn over~~ --
  fixed, then superseded, then given real breathing room.** Was:
  `zwin.c`'s content clip inset only 1px from the window's outer
  edge, far enough to clear the *regular* (unfocused) border but not
  wm's additional bold focus-border (`draw_window_box()`'s extra
  1px-inset outline, drawn only when a window is focused) -- content
  reaching the content area's own edge would draw directly over it
  whenever the window happened to be focused. `gpu3d` (drawing a
  cube whose rotation naturally reaches its own content area's edges)
  is what finally exposed this -- `hello_win` never hit it since it
  leaves a 4px margin. First fixed by insetting `z_win_content_rect()`
  2px on left/right/bottom, clearing both borders -- but that meant
  hardware-blitted text (`term`, 5px-wide glyphs) sat directly against
  the *outer* border with almost no breathing room, since the extra
  pixel came out of content, not chrome. Superseded: the focus-border
  itself now draws 1px *outside* the window's own frame instead of
  1px inside it (`draw_window_box()`), so it never overlaps content
  at all regardless of focus state -- `repair_region()`'s own
  dirty-region computation grew a matching 1px margin (clamped to the
  screen's own bounds, since the hardware rasterizer's coordinate
  registers are unsigned and a negative x/y would otherwise wrap to a
  huge value instead of clipping) so the now-external ring gets
  properly cleared/redrawn on every focus change, not just drawn.
  With the focus-border no longer a factor, `z_win_content_rect()`'s
  inset first dropped back to 1px on every content-bearing edge --
  which turned out to be a second real-hardware regression of its
  own, just a subtler one: 1px is exactly enough to not share a pixel
  with the border, but zero *blank* pixels between content and border
  reads as text sitting directly against (or overlapping) the frame,
  which is exactly what it looked like once `term` was rebuilt against
  it. Settled on 2px on every side (left/right/bottom/top, top
  relative to the titlebar separator line) as a genuine margin rather
  than a border-avoidance side effect this time -- see
  `zwin.c`'s own `z_win_content_rect()` comment for the full
  reasoning, and `term.c`'s own window-size formula
  (`VT_COLS*font.w + 4`, `VT_ROWS*font.h + 16`) for the matching
  arithmetic.
- **~~No GPU arbitration for the line rasterizer/blitter~~ -- fixed.**
  Was: direct framebuffer writes (what `zgfx`/`zwin` used for text)
  sidestepped this for content drawn that way, but any app wanting to
  use the line rasterizer for its own content hit the original
  problem (two processes racing on the rasterizer's single clip
  register, plus a second hazard: the register-writes-then-trigger
  sequence isn't atomic either, so a preempted-mid-sequence call could
  end up interleaved with another process's own sequence). Fixed by
  moving rasterizer access behind `zgfx.c`'s `z_fb_hw_line()`/
  `z_fb_hw_box()`: IRQ-masked for the writes-then-trigger sequence
  (not the FIFO-wait beforehand, which can legitimately take a while
  and shouldn't stall the scheduler for other processes while it
  does), and clip state reasserted fresh on every single call rather
  than assumed to still be correct from a previous one. `wm.c`,
  `gpu3d`, and `gpudemo` all draw through this now instead of each
  keeping its own copy of this logic. See `docs/app_runtime.md`, "the
  GPU line rasterizer" for the full writeup.

  The blitter (used for both fills and hardware glyph blitting, see
  below) is a *separate* piece of shared hardware state from the
  rasterizer, with the same two hazards in its own right (confirmed
  directly in `rtl/gpu/gpu_blit.v`, not just by inference from the
  rasterizer's own bugs). Both paths now have the same fix applied:
  the **fill path** via `zgfx.c`'s `z_fb_hw_fill_rect()`, and the
  **glyph path** (`z_fb_draw_char()`/`z_fb_draw_char2()`, `Z_GFX_HW_BLIT`
  builds only) via the same IRQ-masking treatment around its own
  7-register writes-then-trigger sequence (`gpu_blit_dst_x`/`_dst_y`/
  `_glyph_addr`/`_glyph_w`/`_glyph_h`/`_fg_color`/`_bg_color`, then
  `ctrl`) -- see `docs/app_runtime.md`, "the GPU blitter", and
  `docs/gpu_blitter.md` for the full writeup on each. The glyph path
  had been relying entirely on convention instead (see "Hardware
  glyph blitting" below for the loader side of that convention, which
  is unrelated and still in place) -- real, confirmed real-hardware
  symptom before this fix: garbled pixels near text, worse with more
  than one glyph-drawing process actually running at once (e.g. two
  `term` windows, or `term` alongside `hello_win`), since a timer IRQ
  landing mid-setup could let a second process's own glyph-blit setup
  interleave before the first's `ctrl=START` write ever fired, mixing
  one process's coordinates with another's glyph/color.
  Worth unifying if/when the glyph path gets touched again for
  another reason -- not done proactively here, since it wasn't what
  was asked.
- **Horizontal garbage (~32-64px) near freshly-typed text in `term`
  -- likely root cause found, mitigation removed in favor of a real
  fix.** Reported on real hardware after the glyph-fetch pipeline fix
  (`docs/gpu_blitter.md`, "Bugs found (and fixed)" #3, which did fix a
  separate, confirmed vertical row-shift/contamination bug) -- garbage
  appeared within roughly 1-2 framebuffer words to the right of what's
  being typed, sometimes duplicating recently-typed characters,
  sometimes solid blocks; bounded (doesn't reach the window edge), and
  specifically tied to active typing. Ruled out at the RTL level, each
  via a dedicated testbench (see `rtl/gpu/bench/`), not just review:
  `gpu_blit.v`'s word-straddle split math (`tb_straddle.v`); a full
  48-character line at real 5px-pitch spacing, covering every possible
  word-alignment offset (`tb_line.v`); and cross-master corruption or
  ack-misrouting through the real `rtl/arbiter.v` + `rtl/mem/vram.v`
  under aggressive, continuous contention from a second synthetic bus
  master mimicking `gpu_raster_wb`'s own access pattern
  (`tb_arbiter_stress.v`) -- all pass cleanly against the fixed RTL.
  `sw/common/zvt100.c`'s parser and `sw/apps/term/term.c`'s own
  dirty-tracking/cursor-overlay logic were also reviewed without
  finding a bug. That pointed at something the RTL-only testbenches
  above structurally can't exercise: a **software-level, cross-process
  race**, found on review of `sw/common/zgfx.c`'s glyph/fill-mode
  callers -- see `gpu_blit_acquire()`'s own (long) comment there for
  the full writeup. Short version: every caller about to start a new
  blitter operation used to poll "is the hardware idle?" with IRQs
  still enabled, then separately mask IRQs before writing its own
  registers and the START trigger. The blitter (unlike the line
  rasterizer, which has a FIFO) has no queue -- a START trigger written
  while another operation is still in flight lands while the state
  machine isn't in `ST_IDLE` and is silently dropped, no error, no
  effect. A timer IRQ landing in that narrow "observed idle but not
  yet masked" gap can switch to a different process (`wm`'s own
  `fill_rect()`-based screen repairs, or another `term`/`hello_win`
  instance's own glyph blits -- both share this exact peripheral and
  busy bit) which wins the race and starts its own operation; when the
  original process resumes, its own trigger silently no-ops, and the
  framebuffer cell it meant to update is left showing whatever was
  there before -- stale content, exactly matching "duplicating
  recently-typed characters" or "solid blocks" (an old reverse-video
  cursor cell). It fits "specific to active typing" too: `term`'s
  `render()` issues many `z_fb_draw_char2()` calls in a tight sequence
  per dirty row, multiplying how often the gap gets exercised.
  `gpu_blit_acquire()` closes the gap by folding the busy-check into
  the SAME masked section as the trigger, with a re-check-and-retry if
  another process won the race in between -- applied to
  `z_fb_hw_fill_rect()`, `z_fb_draw_char()`, `z_fb_draw_char2()`, and
  the new `z_fb_draw_icon()` (all of which share the one peripheral).
  **This has not been confirmed on real hardware** -- there was no way
  to reproduce or verify it in this environment -- so treat it as a
  strong, well-reasoned candidate rather than a proven fix.

  **Update: confirmed insufficient on its own.** After
  `gpu_blit_acquire()` shipped (with `resweep_right_of_cursor()`
  removed on the theory it was no longer needed), the artifact was
  still observed on real hardware -- both in `term`'s typing
  (the original report) and, new, in `wm`'s own titlebar text (a
  single-process, no-contention draw, which rules out a
  cross-*process* race as the sole explanation for at least that
  occurrence). So either `gpu_blit_acquire()`'s race isn't the (whole)
  cause, or there's a second, still-unidentified bug -- current
  suspicion, not yet confirmed via simulation, is something in
  `rtl/gpu/gpu_blit.v`'s own state machine (see this file's own
  `TERM_RESWEEP_MITIGATION` note just below). `gpu_blit_acquire()`
  itself is still believed correct and worth keeping regardless (it
  closes a real race independent of whether it explains this
  particular symptom) -- it just isn't sufficient by itself.
  `resweep_right_of_cursor()` has accordingly been reinstated in
  `term.c`, now behind a build-time opt-out
  (`TERM_RESWEEP_MITIGATION`, defined to `1` by default) rather than
  unconditionally removed or unconditionally present -- see that
  macro's own comment in `term.c` for how to disable it once a real
  fix is confirmed (`make term CFLAGS+=-DTERM_RESWEEP_MITIGATION=0`,
  or edit the `#define`). Leaving it enabled by default costs a
  handful of redundant (already-fast) glyph blits per keystroke and
  can only ever redraw *correct* content, never destroy any -- see the
  mitigation's own comment for why that's true by construction --
  so there's no real downside to leaving it on while the actual root
  cause is still being tracked down.
- **A subtler framebuffer race for overlapping/adjacent windows.**
  `z_fb_set_pixel()`'s read-modify-write of a framebuffer word
  (`VRAM[word_index] |= mask`) isn't atomic. Two non-overlapping
  windows can still share a framebuffer word if they're within 32
  pixels of each other horizontally (words are 32 pixels wide and
  don't align to window boundaries), and two apps writing to that
  shared word concurrently could clobber each other's bit if
  preempted mid-update. Not addressed yet -- would need either
  word-aligned window placement or a locking/masking scheme around
  framebuffer writes. Narrower in practice than it used to be: `wm.c`
  itself no longer touches the framebuffer this way at all (its own
  chrome now draws entirely through the hardware paths above), so
  this only applies to apps still using the software `z_fb_*`
  functions for their own content (`hello_win`'s text, by default --
  `Z_GFX_HW_BLIT` builds route glyphs through the blitter instead,
  which has its own, different concurrent-access gap noted above).
- **An intermittent crash chased across several rounds, current
  status: not yet fully confirmed fixed.** `wm.c` would sometimes
  hang or hard-reset (confirmed via the board's `cpu_trap` LED -- a
  genuine CPU trap, not just an unresponsive process) after dragging
  windows around for a while, release-triggered specifically. Ruled
  out, in order: `maskirq()` (removed entirely in a test build, bug
  persisted), the CPU's MUL/DIV extension (a separate, concurrent
  change being tested -- removed, bug persisted), and an
  out-of-range GPU coordinate hanging the rasterizer's FIFO (a real,
  separate bug fixed regardless -- see `z_fb_hw_line()`'s coordinate
  clamp in `docs/app_runtime.md` -- but not what was causing this).
  Targeted diagnostics eventually placed the hang specifically inside
  `fill_rect()`'s own software loop, with no clear dependency on the
  rect's size (a smaller, later call would sometimes hang while a
  larger, earlier one in the same sequence had just completed fully)
  -- consistent with something timing/interrupt-probabilistic rather
  than a fixed threshold. Current best explanation: `fill_rect()` was
  a tight, uninterrupted loop spanning many timer-tick periods for
  anything but a small rect, and this project has hit this exact
  failure class once before (see `boot_picorv32.S`'s `irq_stack`
  comment) -- the C-level interrupt handler's own dedicated stack,
  sized adequately for a timer tick alone, but not necessarily for a
  timer tick and a UART interrupt both landing in the same handler
  invocation, which heavy printf output (both the diagnostic
  instrumentation used to chase this, and plausibly any sufficiently
  chatty app in normal use) makes far more likely. Addressed two
  ways: the IRQ stack was doubled again (512->1024 words -- the
  second such increase; the first, 256->512, addressed a related
  symptom during earlier TFTP debugging, see `docs/networking.md`),
  and `fill_rect()` itself was switched to `z_fb_hw_fill_rect()` (the
  GPU blitter), removing the long software loop that both created the
  original symptom and made it easy to trigger. Neither half of this
  has been confirmed as *the* fix by dedicated, isolated testing (the
  two changes landed together) -- testing in normal use is ongoing as
  of this writing, with no recurrence seen so far.
- **No process-death cleanup.** If an app that owns a window is
  killed, `wm` has no way to find out and will leave its window (and
  window-table slot) around forever. This needs either a kernel
  notification mechanism or a way for `wm` to poll whether a pid is
  still alive -- neither exists yet (see `docs/messaging.md`).
- **Titles are still not drawn** in the chrome itself (font support
  now exists via `zgfx`, but `wm.c` draws chrome purely via the line
  rasterizer and hasn't been updated to render the title text yet).
- **No app-requested placement or resize.**
- **No "already running -- focus instead of relaunching" tracking**
  for dock icons (noted in "The dock" above, deliberately deferred --
  doesn't change the click-handling or redraw plumbing when addressed
  later). The dock also doesn't currently avoid other windows landing
  on top of it via the cascade the way it avoids being *drawn*
  underneath them (see "The dock"'s own placement note) -- true so
  far only because current app window sizes and the cascade formula
  happen not to reach that far down the screen, not because anything
  enforces it.
- **Single-font, single-loader convention isn't enforced.** "Hardware
  glyph blitting" above covers this in full -- `wm` is the only
  process meant to call `z_gfx_hw_font_load()`, and every app is
  meant to only ever draw with `z_font_5x8` as a result, but nothing
  stops a `Z_GFX_HW_BLIT` build from violating either half (e.g.
  `term`'s existing `FONT=z_font_6x12` build option). A real fix would
  need either per-window glyph regions or a "load before every draw
  you actually own" discipline that survives multiple fonts in play
  at once -- neither attempted here, since the immediate goal (close
  the wm/dock race, not support multiple simultaneous fonts) didn't
  need it.
- **Placement cascade is intentionally minimal** -- it just offsets
  each new window slightly from the last; no collision avoidance,
  centering, or multi-monitor concerns (not applicable here) were
  considered. It also doesn't check the new window's own size against
  the screen bounds at all: a large enough window (a first version of
  `gpu3d`'s own window, 320x320, is what surfaced this) can land
  partially or entirely off the bottom/right of the 640x480 screen
  depending on where the cascade happens to be, with no clamping.
  Worked around so far by keeping `gpu3d`/`gpudemo`'s own window sizes
  modest rather than fixing placement itself.
