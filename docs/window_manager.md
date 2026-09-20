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

A fourth followed, and it is what the third phase's own closing note
asked for: **compositing by visible region**. There is one framebuffer
and no depth buffer, so the only thing keeping one window's pixels out
of another's is where each is permitted to draw -- and making that a
guarantee the window manager and the shared drawing layer give,
rather than something each app is trusted to arrange, is what
"Visible regions" and everything under it describe.

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
(`WM_TITLEBAR_H` pixels tall). A window created with
`Z_WIN_FLAG_RESIZABLE` also gets a resize grip: a few diagonal ticks in
the lower-right corner, drawn by `draw_window_box()` so they move with
the wireframe during a drag rather than vanishing until release the way
titlebar text does.

The focused window's **titlebar is inverted** -- one XOR fill over the
strip, applied by `draw_titlebar_content()` after its text and icons
are down, so title and background swap together.

It was a 1px ring around the window's whole perimeter, drawn just
*outside* its bounds. That could not survive visible regions: those
pixels belong to whatever is underneath, and are inside *that* window's
region legitimately, so it was erased the moment that window
repainted -- a clock ticking behind a focused window rubbed the ring
away a hand-sweep at a time.

An indicator drawn outside a window and a rule that a window may only
draw inside its own region cannot both hold. The region rule is what
stops windows painting over each other, so the indicator moved inside
the titlebar, which `wm` owns and no app can reach.

XOR rather than a reverse-video text path: the icons are bitmaps and
the title is hardware-blitted, so inverting once at the end costs a
single fill, needs no second code path for either, and cannot get out
of step with what was drawn. It stops one row short of the separator,
which would otherwise vanish on the focused window.

Two consequences worth knowing. Losing focus now changes **exactly one
thing**, so a focus change repairs titlebar strips rather than whole
windows -- a full repair is only needed when the z-order actually
changed and a window may have been uncovered. And the hardware cursor
had to become XOR too (`rtl/gpu/gpu_video.v`): it was OR'd with the
framebuffer, so it was always white and vanished into the inverted
bar.

Window titles are drawn by `draw_titlebar_content()`, hardware-blitted
from glyph memory.

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
back" the movement in slow motion after the fact. So nothing is
repaired while the button is held: only the dragged window's own
outline moves, and everything else keeps the pixels it already has.

**The outline is one XOR rectangle.** It used to be erase-then-redraw
(`draw_window_box()` with color 0, then color 1), and the erase wrote
black over every pixel the old outline crossed -- gouging a trail
through whatever was underneath that survived until the release
repair. XOR is its own inverse, so the call that takes the band off
its old position restores exactly what it was covering.
`xor_band_draw()`/`xor_band_erase()` (`wm.c`) keep the geometry of the
last draw rather than recomputing it, because a clamp that differed
between the draw and the erase would leave a pixel inverted for good;
and they force the chrome region off, because a clipped XOR is not its
own inverse either.

**XOR only cancels if nothing underneath changes between the two
halves of the pair.** A clock ticking or a terminal scrolling under
the band breaks that, and three separate attempts at repainting the
origin mid-gesture all left trails for exactly this reason. So the
first pixel of motion **freezes every window** -- see "Freezing the
desktop for a drag" below -- and `wm` waits, bounded, for the acks
before drawing the first band. The dragged window is frozen too: it
stays on the glass where it was, a still ghost, until release.

The consequence: while a window is being dragged, nothing on screen
changes except the band -- including the dragged window's own content,
which sits at the origin rather than travelling with the outline. This
is still the well-worn, intentionally simple "wireframe drag" pattern
(plenty of early window managers worked this way); what is new is that
the stillness it depends on is something `wm` now asks for and waits
for, rather than something it hopes for.

**The release repairs two rectangles, not the swept path.** `wm` used
to track the whole bounding box the window passed through
(`drag_min_x`/`drag_min_y`/`drag_max_x`/`drag_max_y`) and repair all of
it; those four now survive only in a diagnostic line. The gesture put
exactly two things on the glass -- the ghost, which sat still at the
origin, and the band, which erased itself at every step -- so the only
pixels that can be wrong are the ones the ghost was covering (the
**origin**, which belongs to whoever is behind it) and the ones the
window has just claimed (the **destination**). `repair_drag()` sends
every window its new region, fills the parts of the origin that no
window owns (`fill_unowned()`), repaints the chrome inside the origin,
and paints the dragged window's frame at the destination. A drag
across the desktop is not a reason to repaint the desktop.

## Input

Mouse position/buttons are read via `mouse_port()` (`wm.c`), which
picks whichever of the two USB HID ports currently reports itself as
a mouse -- there's no fixed port-to-device mapping anymore, and no
guarantee it's the same port from one boot to the next. See
`docs/user_input.md` for the dual-port hardware and register layout
this reads from; this section stays focused on what `wm` does with
the result. `wm` reads this once per main-loop iteration; there is no
mouse event queue, and deliberately so -- the cursor is level state in
a register, and coalescing is what apps want anyway (see `Z_WM_MOUSE`
below, which tells an app to act on the LAST sample it finds rather
than every one).

On a board that has one, a **software mouse register** is a third
source alongside the two ports, written by another process rather than
by a HID controller -- it overrides `mouse_port()`'s pick while it is
active. That is ULX3S-specific; see `docs/user_input.md`, "A pointer
with no hardware behind it".

What changed is when that loop runs. It used to wake every tick
(~732Hz) purely so it would notice the pointer moving; the HID
interrupt now wakes it on each report instead, so an idle desktop
stops waking `wm` almost entirely -- and the wait is indefinite unless
`wm` has a deadline of its own. The register read is unchanged -- only
the polling schedule went away. See `docs/user_input.md`, "Pointer
wakeups", and `docs/app_runtime.md`, "Who blocks, and how".

- **Click on a window** brings it to front and focuses it -- but only
  repaints if focus or z-order actually changed (clicking an
  already-focused, already-frontmost window is a no-op, not a
  repaint).
- **Click+drag on a titlebar** moves the window, clamped to stay
  fully on-screen. `wm` sends `Z_WM_WINDOW_MOVED` to the owning app
  once the button is released (not on every intermediate position),
  so apps aren't flooded with move messages mid-drag.
- **Click+drag on the resize grip** resizes the window -- see
  "Resizing" below.
- **Pointer events reach apps** via `Z_WM_MOUSE`, with capture -- see
  "Pointer delivery" below.
- **The scroll wheel reaches apps** via `Z_WM_WHEEL`, a signed count of
  notches sent to the same window `Z_WM_MOUSE` would go to, and never
  coalesced -- every notch counts (`docs/user_input.md`, "Scroll
  wheel").

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

## Moving and resizing

They no longer work the same way, and the difference is worth saying
first: **a resize blanks the window's interior when it starts**
(`clear_window_interior()` in `wm.c`), **a move does not**.

Without that blanking the app's content stays painted where it was
while the frame changes shape around it, so the window visibly comes
apart mid-gesture. A move has no such problem to solve: the content is
not left behind at a size that no longer fits, it is left behind whole
at the origin, and that is exactly the still ghost the XOR band is
drawn over (see "Redraw strategy" above). Blanking it would destroy
the one thing the band relies on being able to put back.

**Moving** draws the XOR band at each step and repairs the origin and
the destination on release. It used to repair the whole swept bounding
box, and before that four strips around the final footprint with the
footprint itself skipped, on the reasoning that the border there was
already correct. Two things made that wrong, and both showed as
corruption after a *small* move: titlebar content is not part of
`draw_window_box()`, so the old title text and icons survived inside
the new footprint where the strips never reached; and only the dragged
window's owner was asked to repaint, leaving any other window
overlapping the footprint stale. A large move happened to look fine
because the old text landed in a swept strip. Both are covered by the
release repair now, which repaints the moved window's chrome at the
destination and asks whoever gained pixels at the origin to repaint --
see `repair_drag()`.

**Resizing** now moves the real frame rather than drawing a separate
preview: XOR the frame away, adopt the candidate size, XOR it back —
the same three steps the move drag uses. `w`/`h` follow the candidate
live, which is safe because pointer dispatch and hit testing are both
suspended while `resizing` is set; the app is still told once, on
release, via `Z_WM_WINDOW_RESIZED`.

This replaced an L-shaped wireframe over the prospective bottom and
right edges. The L was meant to say "these are the two edges that
move", but with the real frame still sitting at the old size the
result was two disconnected shapes with no visible relationship. A
single closed rectangle that grows and shrinks under the cursor reads
immediately. `resize_orig_w`/`_h` record the starting size, since
`w`/`h` no longer hold it by the time the release-time repair needs it.

## Titlebar icons

A window can ask for extra titlebar icons alongside the close button by
setting flags on `Z_WM_CREATE_WINDOW`: `Z_WIN_FLAG_NEW_ICON`,
`_SAVE_ICON`, `_OPEN_ICON` and `_FONT_ICON` (`zwm.h`). `sw/apps/text`
uses new + save + close.

They are laid out **right-to-left** from the right edge, in the fixed
order close, save, open, new, font — so the common combination reads
left to right as new / open / save / close, the order a File menu would
list them in. That ordering is anchored at the
right for two reasons: close keeps the exact position it always had, so
every app that predates this feature draws identically; and an app
asking for new+save+close gets them reading "new save close" left to
right without having to specify any order at all.

`titlebar_icons()` in `wm.c` computes the placement once and is walked
by both the drawing and the hit testing, so what you can see and what
you can click are the same rects by construction. Icons that don't fit a
narrow window are dropped rightmost-first — if only one button fits, it
should be the one that gets you out. The title text clips to whatever
the leftmost placed icon leaves.

Clicking one does nothing by itself. `wm` has no idea what "save" means
for your app, so it sends `Z_WM_TITLEBAR_ICON` to the owner and stops
there — the same fire-and-forget shape the non-killing form of
`Z_WM_CLOSE` already had. Close deliberately keeps `Z_WM_CLOSE` rather
than becoming a fifth icon kind, so no existing app has to learn a new
message.

The bitmaps live in `sw/apps/wm/win_icons.c`, hand-edited as binary
literals. Adding one is three steps, unchanged from before: append an id
to `z_icon_id_t` (`zicon.h`), add the 8-byte bitmap, add a
`z_gfx_hw_icon_load()` call in `z_win_icons_load()`.

## Moving a window

A drag is a wireframe operation: only the band follows the cursor, and
content is frozen until the button comes up. "Frozen" is now literal --
every window is told to stop drawing for the duration (see "Freezing
the desktop for a drag") -- and three things follow from that, each of
which was wrong at some point.

**The interior is no longer blanked when the drag starts.** It used to
be, before any movement (`clear_window_interior()`, still used by the
resize path), on the reasoning that the window's insides otherwise stay
drawn wherever they were, visibly detached from the frame that is now
somewhere else. That reasoning belongs to a drag in which the *content*
was expected to be stale: it was drawn under a region that no longer
described it, and nothing was stopping it being redrawn wrong. With the
window frozen, its content at the origin is not stale, it is a correct
picture of where the window still logically is -- a ghost, not debris --
and it is what the XOR band needs underneath it in order to cancel.
Blanking it would replace a still image with a black rectangle and
still have to repaint everything at release.

Neither form ever fired on a plain titlebar click: a click that merely
focuses or raises a window has no reason to discard its content and
make the owner redraw. That is why `drag_moved` exists, and why the
freeze happens on the first pixel of motion rather than on the press.

**The repair on release covers the window's own footprint**, not just
the strips around it. `repair_drag()` used to repair four strips and
deliberately skip the final rect, reasoning that its border was already
correct. That missed two things:

- Titlebar *content* is not part of `draw_window_box()`. The wireframe
  drag redraws the box at each step but never the title text or icons,
  so after a small move the old text and icons were still sitting
  inside the new footprint, which the strips by definition never touch.
- Alt+Arrow doesn't erase the old frame at all, so the old **border**
  survived inside the new footprint too.

Both are covered by painting the moved window's whole chrome at the
destination, which `repair_drag()` does last so it lands on top of the
origin repair.

**The origin is repaired, not the sweep.** A single `repair_region()`
over the swept bounding box replaced the four strips, and the sweep in
turn has been replaced by the origin rectangle -- with the desktop
frozen, the band is the only thing that ever touched the pixels in
between, and it took itself back off. `fill_unowned()` fills the parts
of the origin no window owns, `repair_chrome_in_rect()` repaints the
frames of the ones that do, and the apps behind are asked to repaint by
their regions widening, not by a rectangle.

**Alt+Arrow is the same path, one step long.** `alt_move_focused()`
freezes just the window it is about to move (and waits for that one
ack), moves it, and calls `repair_drag()`. The freeze is what stops the
moved window blacking out the part of the origin the destination does
not cover while the windows behind are still repainting it.

## Modal windows

`Z_WIN_FLAG_MODAL` makes a window modal **with respect to its owner's
other windows**. While it exists, `wm` will not focus, raise, drag,
resize or deliver pointer events to any other window owned by the same
process; clicking one raises and focuses the modal window instead, so a
click aimed at a blocked window at least shows you what is blocking it
rather than appearing to do nothing.

Windows belonging to other processes are completely unaffected. This is
per-app modality, not a screen-wide grab — a modal dialog that froze the
whole machine would be a much bigger hammer than anything here needs,
and would make a wedged app unrecoverable.

The reason it exists is `Z_WM_KEY`, which carries no window id. An app
with two windows open cannot tell which one a keystroke was meant for.
Modality resolves that by construction, and is what makes
`sw/common/zdialog.h`'s blocking dialogs possible at all — see
`docs/widgets.md`.

Three smaller behaviours fall out of it, each of which is wrong by its
absence rather than merely untidy:

- A newly created modal window **takes focus immediately**, which is the
  one exception to `wm`'s usual don't-steal-focus rule. A dialog you have
  to click before you can type in it is a worse version of no dialog.
- When a modal window is destroyed, focus goes to the frontmost
  surviving window of the **same owner**, not to nothing. Otherwise
  dismissing a dialog leaves its owner unfocused and receiving no keys
  at all until clicked, which reads as the app having hung.
- Alt+Tab **skips** windows blocked by their owner's modal. Tabbing onto
  one would hand it the keyboard and reintroduce the exact ambiguity
  modality removes. The modal window itself stays in the cycle, so the
  app is still reachable.

If a process somehow ends up with more than one modal window, the
frontmost in z-order wins. That isn't a supported arrangement, just a
defined outcome rather than an undefined one.

## Repairs that nobody can see

`repair_region()` clears a rectangle, redraws the chrome of every
window overlapping it, and asks each of those windows' owners to
repaint their content. It used to do that last part synchronously,
waiting for an ack before moving on to the next window; it does not
any more, and "Content z-order" below says why the wait stopped buying
anything once regions existed.

Two of those requests turned out to be pure waste even so, and both
were visible as a flash when opening a dialog:

**A region the excluded window covers entirely.** A window being
created or brought to the front is repaired using its *own* rect, so
the region always fits inside it. Only that region was cleared;
everything outside is untouched, and the window's chrome fills the
region completely. Anything behind it would have its content hidden
the instant it was drawn. `window_covers_region()` detects this and
skips the notify-and-wait for every window **behind** the excluded one
— windows in front are drawn afterwards and still need theirs.

**A focus change.** Losing focus alters exactly one thing about a
window: how wm draws its titlebar. wm draws that itself, so repairing
the full rect asked the owner for a complete content redraw it had no
reason to do. Narrowing the repair to `Z_WM_TITLEBAR_H` was the first
fix; a focus change now goes through `repair_focus_chrome()` instead,
which calls `paint_window_chrome()` on the two windows involved and
nothing else -- no rectangle is cleared, no owner is notified at all.
`paint_window_chrome()` blanks only the chrome's own pixels
(`draw_window_chrome_bg()`: the titlebar interior, the separator row
and the one-pixel margins), so a frame can be repainted without
touching a pixel of the content inside it. That is what makes it safe
to repaint chrome without asking anyone to repaint content.

**A window retitling itself.** `Z_WM_SET_TITLE` had already narrowed
the *region* to the titlebar strip, but still passed `exclude_idx =
-1` — so the owner that just asked for the retitle got a full content
redraw request back. It would service that later, from its main loop,
after having already repainted for its own reasons. In `read` that was
a visible second render of the whole document on every file open; in
`text` it's a repaint on the first keystroke after every save. Now
`exclude_idx = idx`. Windows in *front* are still notified — they
overlap the strip and are drawn after it.

That one is only safe because the z-order is *not* changing.
`alt_tab()` still repairs in full, because `bring_to_front()` there can
uncover content that really does need redrawing.

Together these removed both full refreshes that used to happen before
a dialog appeared.

## Draining the GPU before a repair

`z_fb_hw_line()` returns with the line still queued in the rasterizer's
FIFO — that's what makes it fast, and the pixels landing a moment later
is normally invisible.

It stops being invisible when a window goes away. An app drawing right
up to the moment it closes still has lines in that queue, and they
drain *after* wm repairs the region, painting over the repair. The
symptom is a window that closes and leaves a scribble behind, which
reads as wm failing to clean up rather than a queue outliving its
owner.

Two changes fix it:

- `destroy_window()` calls **`z_fb_hw_sync()`** (`zgfx.h`) before
  repairing, which waits for the rasterizer FIFO to empty and the
  blitter to go idle.
- `handle_close_click()` **kills the owner before destroying the
  window**, not after. It used to be the other way round, so that a
  dead process wouldn't briefly still have a window on screen — which
  is cosmetic. The ordering it produced was not: with the owner still
  alive, it could draw into the region between the repair and the kill.

## Launch arguments

wm holds one pending launch argument (`Z_WM_SET_ARG` / `Z_WM_GET_ARG` /
`Z_WM_ARG`), because there is no `argv` and `z_proc_run()` carries only
a program name. The launcher sets it, starts the process, and the new
process claims it on startup.

One slot, not one per process: only one app is ever mid-launch at a
time, and reserving `Z_WM_ARG_MAX` bytes in all sixteen `z_proc`
entries for something used once would be waste that also has to grow
every time the path limit does.

Claiming is destructive and time-limited — see `Z_WM_ARG_TIMEOUT` and
`docs/widgets.md` for why an unclaimed argument must not linger. The
reply is a `Z_STR` pointing straight at wm's own buffer, so the bytes
are deliberately *not* cleared when the flag is; the payload is
borrowed until the recipient reads it.

## Retitling

`Z_WM_SET_TITLE` changes an existing window's titlebar text —
`z_win_set_title()` (`zwin.h`) is the app-side call. Until now a title
was fixed at creation, so an app whose title reflects a document had no
way to say so; `sw/apps/text` shows the current filename and whether it
has unsaved changes.

Only the owner may retitle its own window. Nothing else in this protocol
is authenticated either, but there is no reason to add the first way for
one app to relabel another's.

`wm` repairs only the **titlebar strip**, not the whole window. Repairing
the window would work and would be one line shorter, but it costs the
owner a full content redraw and an ack wait every time — for an editor
that is the first keystroke after every save.

## Fonts in glyph memory

Glyph memory (`rtl/mem/glyph.v`, 4096 bytes, byte-addressed through a
12-bit address) holds **more than one font at a time**, at fixed
offsets declared in `glyph_layout[]` (`sw/common/zgfx.c`):

| font | glyphs | bytes | offset |
| --- | --- | --- | --- |
| `z_font_5x8` | 96 x 8 rows | 768 | 0 |
| `z_font_6x12` | 96 x 12 rows | 1152 | 768 |
| *(free)* | | | 1920 |
| window icons | 32 slots x 8 | 256 | 3840 |

The offsets being **fixed and compiled from one source file** is the
whole design, not tidiness. `zgfx.c` is linked separately into every
process, so any allocation state kept at runtime would be per-process:
`wm` would load a font, record where it put it, and no other process
would have any idea — every app would silently fall back to software
text rendering, which is exactly what the hardware path exists to
avoid. A shared table means every process derives the same offset
without anyone communicating anything.

`wm` remains the only process that ever *writes* glyph memory, and now
loads both fonts at startup. Everyone else reads the offset out of the
table and blits.

Fonts are matched by **address** (`font == &z_font_5x8`), which is safe
within a process even though the address differs between them — each
process compares against its own copy, and all of them agree on the
offset, which is the only thing the hardware sees.

A font that isn't in the table still works: `glyph_offset_of()` reports
it as absent and both `z_fb_draw_char()` and `z_fb_draw_char2()` render
it in software. Blitting from an offset holding a different font's data
would draw confident nonsense, which is worse than being slow.

To add one: append an entry with the next free offset, check the total
still clears `Z_ICON_MEM_OFFSET`, and add a `z_gfx_hw_font_load()` call
in `wm`'s `main()`. Note the blitter only handles glyphs up to 8 pixels
wide (`gpu_blit.v`), so `z_font_8x16` is the widest that could ever go
here.

### The software fallback

`z_fb_draw_char()`/`z_fb_draw_char2()` render in software when a glyph
would need clipping, or when its font isn't resident. It is worth
knowing exactly why that exists and how much it costs, because the
obvious reaction — fix it in hardware and delete the fallback — is not
worth doing.

**Why it existed.** Glyph mode in `rtl/gpu/gpu_blit.v` used to be
genuinely unclipped: `CTRL_CLIP` was documented there as "ignored in
glyph mode", with no screen bounds check either — which is why the
`fits` test checks the screen as well as the clip rect, since an
out-of-range destination can hang the blitter's state machine.

**That is no longer true.** Glyph mode honours the scissor now:
vertically by skipping rows outside it, horizontally by masking the
cell. The *cell* is masked, not just the glyph bits — glyph mode paints
a solid cell, so clipping only the bits would let the background erase
pixels outside the region, which for a partially occluded terminal
means erasing the window in front of it in the shape of its own text.
`rtl/gpu/bench/tb_glyph_clip.v` tests exactly that.

So the fallback is no longer needed for clipping. It is still the path
for a font that is not resident in glyph memory, and still the path on
a bitstream that predates the scissor. The measurements below describe
how rarely it fired even when clipping was its main job.

**How often it fires.** Measured against the real geometry of every
clipped call site: **zero** for a full text-editor repaint at any
window size or either font (6273 glyphs, none clipped), zero for the
file list at normal widths, zero for a titlebar whose title fits. It
only fires on *truncated* text, and then only for the one glyph
straddling the boundary.

`z_fb_draw_text()` used to make this much worse than it needed to be:
it called `draw_char()` for every remaining character even after
running past the clip, so each one took the software path and then had
all `w*h` of its pixel writes rejected individually. Skipping glyphs
that start past `clip->x1` cut the worst measured case (a 24-character
title in a 64px titlebar) from 21 software glyphs to 1, and a narrow
file list from 55 to 11.

**What is left** is at most one partial glyph per truncated line — 40
pixel writes at 5x8, 72 at 6x12. Adding clip support to the glyph state
machine would mean masking `g_bits_lo`/`g_hi` against a clip window and
skipping out-of-range rows, in a state machine that already has fifteen
states, to save that. Not worth it.

**The case that would be slow** is a font that isn't resident, where
every glyph goes software. Today nothing does that: `term` defaults to
`z_font_5x8` and `repl`'s `(text ...)` uses `z_font_6x12`, both
resident. Building `term` with `FONT=z_font_5x7` or `z_font_8x16`
would be correct but slow, and the fix is to add that font to
`glyph_layout[]`, not to change the hardware. There are 1920 free bytes
between the fonts and the icon region, which fits `z_font_8x16` (1536)
or `z_font_5x7` (672), but not both.

### Font switching

`Z_WIN_FLAG_FONT_ICON` puts a font button in the titlebar and clicking
it sends `Z_WM_TITLEBAR_ICON` with `Z_WM_TBICON_FONT`. What that means
is entirely the app's business — `wm` does not change anything itself.
`sw/apps/text` toggles between the two resident fonts, which changes
its column count and therefore rewraps the whole document.

## Widgets

`sw/common/zwidget.c` is a small toolkit for the furniture an app keeps
in its window: push buttons, toggles, radio groups, pattern swatches,
sliders. It is deliberately **not** a UI framework -- no layout engine,
no widget tree, no focus chain. A widget set is a flat array the app
declares and positions itself, in content-relative coordinates. The
first two consumers (`draw`'s tool column and pattern palette) both want
fixed grids they compute arithmetically, and a layout engine would be
more code than the thing it replaces.

What it does provide is the part that is annoying to write twice: hit
testing, pressed/selected state, radio-group exclusivity, dirty tracking
so only changed widgets repaint, and drawing that goes through the GPU.

Alongside the widget set there are scrollbars (`z_scrollbar_t`, both
orientations, in the same file) and a scrolling file-list widget
(`sw/common/zflist.h`), which is what the file dialogs are built on and
what a standalone file browser app would reuse. **See
`docs/widgets.md`** for how to use all of these, and for the dialogs in
`sw/common/zdialog.h` — including the one non-obvious requirement, that
an app opening a dialog must keep servicing its own window's redraws
while the dialog is up.

Widget bodies, frames, fills and slider tracks use
`z_fb_hw_fill_rect()`, `z_fb_hw_fill_pattern()` and `z_win_hw_box()`.
Text labels go through the hardware glyph path. **Icons do not** -- they
are drawn per pixel in software, and that is not an oversight. The
glyph blitter can only draw what is in glyph memory, that memory holds
one font at a time, and by board-wide convention `wm` is the only
process that ever writes to it (see "Hardware glyph blitting" below). An
app blitting its own icons would have to load them there, breaking that
convention and reintroducing exactly the cross-process race it exists to
prevent. Icons are 16x16 and redrawn only when a widget's state changes,
so software is genuinely fine -- the same tradeoff `wm` already makes
for the dock's own 32x32 icons.

`icon-next.png` is in the same directory and goes through the same
generator, but is not an app icon -- it is the dock's own NEXT button.
It is there rather than hand-written into `wm.c` so that the one place
32x32 dock artwork is authored stays the one place.

`z_fb_hw_fill_pattern()` (`zgfx.h`) is new alongside this: the
blitter's `BLIT_PATTERN` register was always a full 32-bit word, and
`z_fb_hw_fill_rect()` simply hardcoded it to all-0s or all-1s. Patterns
are 8 bytes, one per row, MSB-first -- the same convention as font
glyphs and `zicon.h` icons -- and are anchored to the screen's 8-pixel
grid so adjacent fills tile seamlessly. Cost is one blitter operation
per *run of rows sharing a pattern byte*, so a solid fill still costs
one.

## Pointer delivery

Until `Z_WM_MOUSE` existed, `wm` polled the pointer and kept it
entirely to itself -- only `Z_WM_KEY` ever reached an app. An app that
needed the mouse had to read the USB HID registers directly, which is
what the pre-window `sw/apps/draw` did and precisely why it could not
live in a window.

`Z_WM_MOUSE` is a packed `Z_UINT32` (`x`, `y`, buttons, and an "inside
my content rect" bit), for the same no-heap-allocation reason as
`Z_WM_REDRAW` and `Z_WM_KEY`: it fires at pointer-movement rates, and
nothing frees message payloads (see `docs/messaging.md`).

**Who receives it**, in order:

1. If a **capture** is active, that window, wherever the cursor now is.
   A capture starts when a button goes down inside a window's content
   area and ends when it comes up.
2. Otherwise the **focused** window, but only while the cursor is over
   it *and* the hit test agrees it is frontmost there. Requiring both
   is what stops a window receiving phantom events through another
   window stacked on top of it.

Nothing is delivered while `wm` is running its own drag or resize
gesture -- the pointer belongs to `wm` for the duration.

Capture is not a refinement; without it every drag-style interaction
(a paint stroke, a slider, a rubber-banded rectangle) silently cuts off
at the window edge.

`wm` **coalesces** these: one is sent only when position or buttons
actually differ from the last one sent to that window, so a resting
mouse costs nothing. It does not rate-limit beyond that, so an app
should drain its whole queue and act on the *last* mouse message it
finds. Processing every one in turn makes a slow redraw path fall
progressively further behind the real cursor -- the stroke visibly lags
and then catches up in a rush.

Coordinates are absolute screen coordinates, matching `z_win_hw_line()`
rather than the window-relative convention `z_win_draw_text()` uses.
`z_win_mouse_content_xy()` (`zwin.h`) converts.

## Resizing

A window created with `Z_WIN_FLAG_RESIZABLE` can be resized by dragging
the grip in its lower-right corner. Without the flag the corner is
ordinary frame and the hit test never fires, so every app predating the
flag is completely unaffected.

The gesture is a **wireframe**, like dragging already is, and for the
same reason: a full clear/redraw/content-notify per step queues redraws
faster than apps can drain them. Only the prospective **bottom and
right edges** are drawn -- a 2px bold L, not a full box. Dragging a
window already highlights its entire frame, and reusing that here would
claim the whole window is moving when in fact the top-left corner is
pinned and only those two edges follow the cursor. Showing exactly the
edges that move is both more honest and easier to aim.

The window's real `w`/`h` are not touched until the button is released.
On release `wm` commits the size, sends `Z_WM_WINDOW_RESIZED`, then
repairs the union of the old footprint, the new one, and the largest
extent the wireframe reached in between. Unlike the drag path there is
no point excluding the window's own final rect -- its content area
genuinely changed size, so all of it needs redrawing.

**Minimum size.** Every resizable window is clamped to
`Z_WM_MIN_WIDTH`/`Z_WM_MIN_HEIGHT`. That floor is not cosmetic: below
it the content area's height goes negative and underflows to an
enormous unsigned value downstream. `Z_WIN_FLAG_MIN_IS_CREATE` raises
the floor to whatever size the window was *created* at, for an app with
fixed-size furniture it cannot shrink below -- `sw/apps/draw`'s tool
column and pattern palette are exactly that. The app knows that size
(it is why it asked for it); `wm` has no way to work it out. The global
floor still applies on top, so a window created at 20x20 is not
resizable down to 20x20.

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
  single-step version of a mouse drag: `alt_move_focused()` freezes
  just that window, updates its position directly, and calls
  `repair_drag()` -- the exact same origin-and-destination repair a
  real drag-release uses (see "Redraw strategy" above) -- rather than
  re-deriving that logic.
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
| wm → app | `Z_WM_MOUSE` | packed `Z_UINT32` (`Z_WM_PACK_MOUSE`) | pointer position and buttons, to the window that owns the pointer; coalesced -- act on the last one queued |
| wm → app | `Z_WM_WHEEL` | `Z_UINT32`, signed notches (`Z_WM_WHEEL_NOTCHES`) | scroll wheel, positive = up; same target as `Z_WM_MOUSE`; not coalesced -- act on every one |
| app → wm | `Z_WM_SET_TITLE` | `Z_MAP{id, title}` | retitle an existing window -- repairs the titlebar strip only |
| wm → app | `Z_WM_TITLEBAR_ICON` | packed `Z_UINT32` (`Z_WM_PACK_TBICON`) | a new/save/open/font titlebar icon was clicked |
| wm → app | `Z_WM_SET_CLIP` | `Z_BLOB` of `z_wm_cliprect_t[]`, led by a control rectangle naming the window | the part of this window not covered by the windows in front of it, or a command about its clip |
| app → wm | `Z_WM_CLIP_DONE` | `Z_UINT32` (window id, plus `Z_WM_CLIP_DONE_DREW`) | acknowledges a `Z_WM_SET_CLIP` |

**Every windowed app must handle `Z_WM_SET_CLIP`.** Three lines:

```c
case Z_WM_SET_CLIP:
    z_win_apply_clip(&win, &msg.obj);   // &msg->obj in some loops
    break;
```

An app that handles the message not at all draws **nothing**, which is
the opposite of what it used to be: until a window has been told a
region it is treated as invisible rather than as unclipped. See
"Visible regions" for why that swapped round.

The ack `z_win_apply_clip()` sends is still not optional, though the
reason changed. `wm` no longer waits for it on a narrowing region; it
waits for it at the start of a drag, when it needs to know the screen
has actually gone still before drawing the first rubber band, and it
reads `Z_WM_CLIP_DONE_DREW` out of it to find out which windows tried
to paint while they were frozen. An app that applies regions without
acking makes the first band of every drag wait out the freeze timeout
and can leave its own window stale after one.

`z_win_apply_clip()` returns **false** for a region belonging to some
*other* window of the same process -- the control rectangle at the
front of the payload says which window it is for. An app that owns a
dialog as well as its main window must offer the message to each in
turn rather than letting the first one swallow it.

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
scalar carries just `(id, x, y)` -- there is no room for width/height
in it. That is why `Z_WM_WINDOW_RESIZED` exists as a separate `Z_MAP`
message and why `wm` sends it *before* the redraw that follows a resize:
an app that saw the redraw first would repaint at its old size into a
window that is no longer that size. `z_win_apply_redraw()` in `zwin.c`
unpacks the packed form.

`Z_WM_WINDOW_CREATED`, `Z_WM_WINDOW_MOVED` and `Z_WM_WINDOW_RESIZED`
are low-frequency (once per window, once per completed drag or resize)
and keep the `Z_MAP` shape -- `z_win_parse_rect()` handles all three,
and `z_win_apply_resized()` is just that under a name that says what it
is for at the call site. An app that handles resizing must handle
**both** `Z_WM_WINDOW_RESIZED` (to learn its new size) and the
`Z_WM_REDRAW` that follows (to know when to repaint at it). An app's message loop needs to
tell these two shapes apart by subject; see `sw/apps/hello_win` for
the pattern.

An app should redraw in response to `Z_WM_REDRAW`/`Z_WM_WINDOW_MOVED`,
and also whenever its own internal state changes independent of the
wm (a clock ticking, data arriving, etc.) -- the wm has no way to know
about the latter, so apps drive those redraws themselves. The one time
it should not is while `z_win_frozen()` is true -- see "Freezing the
desktop for a drag". If an app does this on a timer (like
`hello_win`'s counter), don't just check
messages once before and once after a long delay -- poll in small
chunks throughout it (see `TICK_ITERATIONS`/`POLL_CHUNK` in
`hello_win.c`), or a `Z_WM_REDRAW` arriving mid-wait sits unprocessed
until the whole delay elapses, which is very noticeable if the delay
is more than a fraction of a second.

### Damage: what the redraw actually invalidated

`Z_WM_PACK_XY` uses bits 0..27, so there were spare high bits, and one
of them is now `Z_WM_REDRAW_DAMAGE`. With it set, the redraw means
**only the pixels this window's region just gained** -- not "everything
you can see".

The distinction earns its bit on a raise. Clicking a partly covered
window uncovers a strip of it; without damage, the only thing `wm` can
say is "repaint", and the app repaints all of itself -- which on this
machine is not free. With three terminals behind a click that was
around 0.8 s of wall time spent putting pixels back exactly where they
already were, visible as a black curtain sweeping the window, and every
stripe of it re-encoded by the remote desktop on the way out.

`z_win_damage_rects()` (`zwin.h`) hands an app the rectangles. It is
worth reading its contract in full, because the three return values are
genuinely different situations:

| | |
|---|---|
| `-1` | everything the window can see. A cleared rectangle, a full-screen repaint, a move, a resize, or the first region the window was ever given. |
| `0` | nothing. The region changed in a way that took pixels away and gave none back; every pixel the window still owns is already on the glass, and the app may return without painting. |
| `>0` | that many rectangles, in screen coordinates. Repainting their intersection is enough. |

**Ignoring it entirely is still correct.** The shared layer confines
every `z_win_*` draw to the same rectangles for the duration of the
redraw, so an app that repaints itself whole puts exactly the same
pixels on the glass -- it just does the work of drawing the ones that
were already right. Only an app that keeps its own model of what is on
screen (`term`'s shadow) has to care, and only to save the work. An app
whose content is not addressable by rectangle -- `gpu3d` re-renders a
whole frame, and painting only the newly exposed strip would leave the
previous frame standing in the rest of the window -- says so with
`z_win_damage_ignore()` and repaints in full.

Two rules keep a damage list from being wrong rather than merely
narrow, and both were learned by getting them wrong:

- **An empty damage list means empty, not unrestricted.** Zero
  rectangles would be read by `zgfx` as "no clipping" -- the same trap
  `Z_WM_SET_CLIP` documents -- so a redraw whose damage worked out
  empty used to be answered with a full, unclipped repaint. That is
  the curtain again, arriving by a different road. `z_win_t.paint_set`
  is what distinguishes the two.
- **A full redraw cannot be narrowed by a damage that arrives after
  it.** Apps drain their whole queue before painting, and `wm` can
  legitimately send both in one breath: destroying a window repairs the
  rectangle it left (everything) and then widens the regions underneath
  it (a damage). `z_win_t.paint_full` latches the wider of the two.

The "what the window gained" arithmetic needs a "before", and that is
`clip_was` (`zwin.h`): the region as of the last *paint*, not the last
`Z_WM_SET_CLIP`. It is intersected with each new region rather than
overwritten, which matters for two cases that overwriting got wrong --
a drag that uncovers a hole in several steps (the damage has to be the
union of everything newly visible, not just the last strip), and a
window lowered and then raised again with no repaint in between (whose
"before" must not still be the region it had before it was lowered, or
the damage works out empty and the window comes back blank).

### Content z-order

Chrome is drawn directly by `wm`, in z-order, so it's always correct.
Content is drawn by each app's own process, asynchronously, whenever
that process happens to get scheduled -- which has nothing to do with
window stacking order. Without something to enforce ordering, a
window behind another one could easily have its content redraw
*after* the front window's, and since there's no depth buffer (just
last-write-wins on the framebuffer), its content would show through on
top of the window that's supposed to be in front of it.

`Z_WM_REDRAW_DONE` (app -> wm, `zwm.h`) was the first answer:
`repair_region()` walked its overlapping windows strictly back-to-front
and, after sending `Z_WM_REDRAW` to each one, blocked
(`wait_for_redraw_done()` in `wm.c`) until that specific app acked
before moving on to the next, more frontmost window.

**Visible regions replaced it, and the replacement is stronger.**
Ordering by ack makes a late repaint land in the right *order*;
clipping makes it land in the right *place*, whenever it lands. A rear
window's drawing cannot reach the front window's pixels at all, because
those pixels are not in its region -- so there is nothing left for the
ordering to protect against, and `repair_region()` now sends its
redraws fire-and-forget. `wait_for_redraw_done()` is still in `wm.c`,
unreferenced, along with the reasoning above.

Dropping the wait was not just tidiness. It froze the pointer for the
sum of every overlapping app's full repaint, which on a focus click
with several terminals behind it was seconds of black, and it did that
while holding `wm` inside a message handler -- see "Known limitations"
for the reentrancy that implies. Regions removed the only reason to pay
for it.

`z_win_redraw_done()` (`zwin.c`) is still the app-side call, and it
matters more than it used to rather than less. `wm` is no longer
waiting on the message, but the call itself is what tells the shared
layer that everything asked of this window is now on the glass: it
commits `clip_was`, which is the baseline the **next** redraw's damage
is measured against (see "Damage" above). An app that repaints and
never calls it computes its next damage against a stale picture of what
it has painted. Call it after servicing a `Z_WM_REDRAW`, not for
redraws the app initiates on its own.

### Visible regions

The ack ordering above kept *repairs* correct, but it only ever
covered drawing an app does **because wm asked it to**. An app that
redraws on its own -- a clock ticking, a terminal receiving output --
was clipped only to its own content rect, not to the part of that rect
actually on screen. So a clock running behind a text editor drew its
hands straight through the editor.

Each window now has a **visible region**: its own rectangle minus every
window in front of it. `wm` computes it (`region_compute()` in `wm.c`)
and sends it as `Z_WM_SET_CLIP`; the app calls `z_win_apply_clip()`
(`zwin.c`), which hands it to `z_gfx_set_visible()` (`zgfx.c`), which
confines every drawing primitive to it.

It is a **list** of rectangles, not one. A window covered in its middle
is visible as a ring, and a bounding box around that is a *superset* --
it would permit exactly the drawing this prevents.
`sw/apps/wm/tests/test_region.c` checks the computation pixel by pixel
and asserts that a bounding box gets it wrong.

**An empty list is not "no clipping" -- and neither is having been
told nothing.** Zero rectangles would be read by `zgfx` as
unrestricted, so a fully occluded window is sent **one empty
rectangle**, never zero of them.

The state *before* wm has said anything went the other way round, and
the reversal is the single most consequential line in this protocol.
It used to mean "draw normally", on the reasoning that a window would
otherwise be blank until its first region message. It now means draw
**nothing** (`win_use_clip()`, `zwin.c`): a window is born invisible.
Unrestricted was the wrong default because a window's first region can
still be in flight while it paints, and an unrestricted first paint
lands on every window above it -- which is precisely what regions exist
to stop, arriving in the one gap the protocol had left open.

The blankness that argument was guarding against does not happen,
because the two halves are delivered together: wm sends the region
first and a `Z_WM_REDRAW` immediately behind it (`send_clip_ex()`), so
a window that could not paint before its first region is asked to paint
the moment it has one. Read those two paragraphs together or the first
one reads like a bug.

**Nothing waits any more.** A shrinking region means a window has moved
in front, and the app must stop drawing into those pixels before that
window is drawn -- so wm used to send and wait for `Z_WM_CLIP_DONE`,
while a widening region, where drawing less than permitted for a moment
is stale rather than wrong, was fire-and-forget.

The premise was right and the wait was still wrong, for a reason that
only showed once it was in. Every wait blocks wm inside a message
handler (see "Content z-order"), and regions need sending on creation,
on every raise and on every drag -- so where there had been one such
point there were now several, and every app that happened to be busy
starting up became a timeout. The launch that timed out was the one
that appeared not to start.

And the wait buys nothing now. What `repair_region()`'s ordering
protected against was a rear window's late repaint landing on top of a
front window's; clipping makes that impossible regardless of timing,
because those pixels are not in the rear window's region at all. The
worst case without the wait is an app drawing against a stale region
for the few milliseconds until it reads its queue -- and the window
that just covered it is repainted immediately afterwards anyway, so
even that self-heals.

A window's **first** region is still special, just not as a narrowing.
It has never been told it owns anything, so there is nothing to take
away. Waiting for it also deadlocked, because the app is still inside
`z_win_create()` and its first repaint, neither of which reads the
message queue.

**Only a window that GAINED pixels gets a redraw.** The area a window
just uncovered still holds whatever was on top of it, and the region
message does not ask for a repaint, so `send_clip_ex()` sends one --
with `Z_WM_REDRAW_DAMAGE` set, naming exactly the pixels that were
gained.

Comparing total area is not enough to find them: the common drag has
the window behind losing a strip on one side and gaining one on the
other, with the area unchanged, and the gained strip left black. So
`region_gains()` subtracts the old region from the new one and asks
whether anything is left.

The other direction is the expensive one, and it is why this is
"gained" rather than "changed". A window whose region merely **shrank**
has nothing new to paint -- every pixel it still owns is already on the
glass, and the pixels it lost belong to whoever covered them and are
that window's to repaint. Asking it to repaint anyway was the curtain:
three terminals behind one click, each redrawing itself entirely for no
visible change. The one case that argued for telling it anyway -- a
self-animating app like `gpu3d`, which learns of a narrowing only from
`Z_WM_SET_CLIP` -- turns out not to need it either: what it keeps in
the still-visible part is exactly what was there before.

**Copies can be clipped now, within limits.** `z_fb_hw_scroll()` and
`z_fb_hw_blit_vram()` used to be refused outright whenever a region
was set at all, because the blitter's copy source aligns to the
destination *word*, so a scissor that moves the first written word
feeds the engine the wrong data. That is still true of the hardware --
but the right question is about the *destination*, not about whether a
region exists: `copy_region_allows_rect()` (`zgfx.c`) allows the copy
when the destination lies inside a single visible rectangle, because a
scissor that cannot cut cannot get anything wrong. A window that is
merely focused, or covered nowhere near where it is scrolling, keeps
its hardware scroll; a genuinely partially occluded one still repaints
instead. Several rectangles are still refused, since proving the union
contains the destination needs coverage rather than containment.

The memory-to-VRAM path obeys differently. `z_fb_hw_blit_mem()` walks
the visible region and intersects the destination with each rectangle,
moving the source origin by the same offsets, one pass per rectangle --
so the canvas apps keep their blit whatever is on top of them. See
`docs/gpu_blitter.md`, "Copy and the scissor", which carries the full
arithmetic.

**The region says which window it is for.** A process owns more than
one window as soon as it opens a dialog (`sw/common/zdialog.c`), and
messages arrive in one queue per **process**, not per window. So every
`Z_WM_SET_CLIP` payload begins with a control rectangle --
`x0 == Z_WM_CLIP_CTL`, `y0 == Z_WM_CLIP_WINDOW`, the window id in `y1`
-- which `z_win_apply_clip()` consumes and never installs.
`Z_WM_CLIP_CTL` is `INT16_MIN`, which can never be a screen
coordinate, and the rectangle is degenerate, so a receiver that does
not understand it reads it as empty, which is the harmless reading.

Without it the receiver cannot tell whose region it just got, and
applying the newest one to whichever `z_win_t` is at hand is wrong
exactly half the time. It showed as a dialog with no buttons: the
region meant for the dialog went to the parent, the dialog was never
told one, and a window that has not been told a region draws nothing at
all.

`clip_payload()` (`wm.c`) writes the control rectangle rather than
each call site doing it, so the freeze path cannot forget it.

`repair_region()` takes an `exclude_idx` parameter for one specific
case: right after `create_window()`, the brand-new window's own chrome
needs drawing (and repair_region() is what does it) before its owner
gets its `Z_WM_WINDOW_CREATED` reply -- otherwise the owner's
`z_win_create()` can return, and its first drawing calls can run,
before the wm has drawn so much as a border for it. But that same new
window's owner can't be sent `Z_WM_REDRAW` the normal way, since it's
still blocked waiting for the very reply that hasn't been sent yet --
its `z_msg_wait()` would silently discard the `Z_WM_REDRAW` (it doesn't
match what it's waiting for). `exclude_idx` skips just the notify step
for that one window (chrome still gets drawn) while behaving normally
for every other window `repair_region()` touches.

### Where this came from

The ack-ordered scheme above ended with a note about what it was not
doing yet, and it is worth quoting, because this section is the answer
to it:

> A slow-to-redraw app therefore stalls the whole UI until its timeout
> expires, which is a real cost for a fairly small amount of protocol.
> The alternative -- computing each window's actually-visible region
> (its rect minus whatever's covering it) and having apps clip to just
> that -- would let redraws happen in any order safely, but needs real
> multi-rectangle clipping (a window's visible area can be an irregular
> shape once more than one window overlaps it), which felt like more
> complexity than this phase warranted. Revisit if the stall becomes a
> real problem, or once resizing/proper occlusion support is worth
> building anyway.

The stall did become a real problem -- with several terminals on screen
a focus click cost seconds of frozen pointer -- and the multi-rectangle
clipping it named is what `region_compute()`, `z_gfx_set_visible()` and
`z_wm_cliprect_t[]` are. Redraws do now happen in any order safely, and
the waits are gone for exactly the reason predicted: they were paying
for an ordering guarantee that clipping provides for nothing.

What the note could not predict is the second half of the bill. Once a
window can only paint inside its region, three things that used to be
somebody's implicit responsibility have to be said out loud: what a
redraw actually invalidated ("Damage" above), whether a window is
allowed to paint at all right now ("Freezing the desktop for a drag"
below), and which of a process's windows a region is for. Each of those
is a protocol addition rather than a refinement, and each was found by
something on screen being wrong rather than by design.

### Freezing the desktop for a drag

A titlebar drag is a wireframe over a still image, and "still" has to
be enforced rather than assumed -- the XOR band only cancels if nothing
underneath it changes between the draw and the erase (see "Redraw
strategy").

`Z_WM_CLIP_FREEZE` is how wm asks. It is a control rectangle, like the
window id above, and it means **stop drawing, keep the glass**: the
window's clip becomes the empty rectangle *without* the repaint a
narrowing region would provoke, so whatever it is showing stays exactly
where it is. There is no thaw command -- the next real region thaws it,
because a region is state and the newest one is always the right one.

`freeze_all()` sends it to every window and waits, bounded by
`FREEZE_ACK_TIMEOUT_MS` (300 ms), for the acks before the first band is
drawn. That is the one wait left in this protocol, and it is the only
place a wait still buys something: it is the difference between a clean
band and a trail.

**An app that does not cooperate is still correct.** Its draws reach
nothing, because its clip is empty. The problem is narrower and
sharper: an app that keeps its own model of what is on screen --
`term`'s shadow of the cells, `clock`'s record of where it last drew
its hands -- updates that model as it "paints", the paint goes nowhere,
and from then on the model disagrees with the glass. `clock` erases its
old hands by drawing them in the background colour, so a frozen tick
loses the old hands permanently.

So `z_win_apply_clip()` remembers that a draw was attempted while
frozen and reports it in the ack for the *thawing* region, as
`Z_WM_CLIP_DONE_DREW`. wm answers that with a `Z_WM_REDRAW` even though
the window's region did not change, and the app-side bookkeeping drops
`clip_was` so that redraw is a full one. The safety net works; it costs
a whole-window repaint per affected app per drag, which with three
terminals running render loops is the curtain users see on release.

The cure is for an app whose render is periodic to test
`z_win_frozen()` (`zwin.h`) at the top of it and return without
touching either the glass or its own model. What it wanted to draw
stays pending -- a dirty flag still set, a hand not yet moved -- and
the first tick after the thaw draws exactly that, which is a handful of
cells rather than a window. `term` and `clock` both do this; the DREW
ack remains for everything that does not.

**The dragged window is frozen too, and stays where it was.** Nothing
is lifted off the glass during the gesture: its content sits at the
origin as a ghost, and the windows behind it are not told they have
gained anything until the button comes up, when `repair_drag()` repairs
the origin and they do.

The alternative is to resolve that at the *start* instead -- freeze the
dragged window, blank its origin, give the windows behind their regions
as though it were gone, wait (bounded) for them to paint, and only then
freeze the desktop and draw the band. It is implemented, behind
`DRAG_ORIGIN_REPAINT` in `wm.c`, and off by default: it shows what is
behind from the first pixel of motion, at the cost of the band
appearing one repaint round later. `region_skip_idx` is what leaves a
window out of every region computation for that, as if it were not on
screen.

## The dock

A small always-on-top launcher bar, anchored to the bottom left of
the screen, with one 32x32 icon per app (`sw/apps/wm/wm.c`,
`dock_candidates[]`). Adding another is one more
`{ "name", z_icon_<name>_data }` entry there, plus a 32x32 PNG in
`sw/data/icons/` and a run of `gen_dock_icon_data.py` -- see that
script's own usage comment. Nothing else.

The list is deliberately not reproduced here. It went stale the moment
apps were added -- it used to say "currently `term`, `gpu3d` and
`draw`" long after there were a dozen -- and `dock_candidates[]` is two
lines of scrolling away.

### Paging

The dock is one row and stays one row -- vertical space is the scarcest
thing on a 480px screen, so it does not grow a second one. Icons are 36
pixels apart (`DOCK_ICON_SIZE` 32 plus `DOCK_ICON_GAP` 4), which puts
**seventeen** across a 640px screen margin to margin.

Up to seventeen resolved apps, nothing below applies: the dock is sized
to its contents and grown from the left, every icon visible, no NEXT
button and one page. Most machines will never reach that, and they
should not pay for a mechanism they do not need.

Past seventeen it pages:

```
+------------------------------------------------------+
| [a][b][c][d][e][f][g][h][i][j][k][l][m][n][o][p]  [>] |
+------------------------------------------------------+
```

- The frame spans the **full screen width** and the NEXT icon is
  **right-aligned** inside it, so NEXT is in the same place on every
  page. A NEXT that trailed the last icon of each page would sit
  somewhere different on a short final page, which is exactly the
  behaviour that makes a button annoying to hit.
- Sixteen apps per page (`DOCK_PAGE_SIZE`), since NEXT occupies the
  seventeenth position. Crossing the threshold therefore costs one
  visible app: 17 shown, then 16 plus a button.
- **NEXT wraps.** That is what makes one button enough. A NEXT that
  stopped at the last page would need a PREV beside it, which is a
  second slot spent and a second thing to explain; wrapping means the
  same icon always does something and the way back is simply forward.
  That holds while the page count is small -- two is instant, four is
  three clicks -- which is the range this is for.
- **Empty space is the page indicator.** Page 0 is always full, so the
  unused positions on a short page can only mean a later one. No
  counter, no label, no chrome.
- **Alt+[** and **Alt+]** step pages directly, for anyone who does not
  want to walk the cycle. Global rather than dock-focused: the point is
  to reach an app on another page without first focusing the dock.
- Keyboard navigation treats NEXT as one more icon in the cycle --
  Left/Right lands on it and Enter activates it -- so the keyboard and
  the mouse offer exactly the same targets.

Capacity is now bounded by `dock_candidates[]` rather than by geometry,
so the list may grow freely.

**The arithmetic lives in `sw/apps/wm/dock_layout.h`, not in `wm.c`.**
`draw_dock()`, `dock_click()` and `dock_handle_key()` all have to agree
on where an icon is and which app is under it; while that was one
multiplication each, three copies was survivable, and with a page
offset and an off-stride NEXT it is three chances for a click to launch
the icon next to the one that was pressed. Those functions are pure --
they take "how many apps" and "which page" as arguments rather than
reading wm's globals -- which is also what makes
`sw/apps/wm/tests/test_dock.c` able to check a forty-app dock on a
machine that has six. `make -C sw/apps/wm test` runs it, and needs no
toolchain and no board.

A candidate is only an *offer*: `dock_build()` keeps the ones that
actually resolve via `z_exec_exists()` at startup, so an app that isn't
installed is skipped rather than becoming a button that does nothing.
`draw` lives on the sdcard rather than in the flash core-app archive,
so it appears only when a card carrying it is present.

**Present is not the same as runnable**, and the dock checks both. A
candidate may name a `Z_FEATURE_*`/`Z_FEATURE2_*` bit
(`dock_app_t.feature`/`feature2`), and `dock_build()` drops it when
`z_soc_has_feature()` says this bitstream does not have that hardware
-- `track`, `midi` and `play` need `Z_FEATURE_AUDIO`, `mmod` needs
`Z_FEATURE2_GPIO`. The alternative is a button that starts an app only
for it to exit a second later, which looks like a broken app rather
than a board built without a peripheral. The same board can be
reflashed with a bitstream that has it, and the icon comes back.

`name` is the bare filename `z_proc_run()` (see `docs/app_runtime.md`)
expects -- no path, no extension, same as what you'd type after `run` at
the kernel shell.

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
  called directly from `paint_window_chrome()`, right next to
  `draw_window_box()` -- not via a `Z_WM_REDRAW` to another process
  like every other window's content. That messaging round trip exists
  because content drawn by a *different* process might not be
  scheduled for a while; the dock's content is drawn by `wm` itself,
  in the same call, so there is nothing to wait for and no ordering
  gap to close. (Same reasoning already applied to the demo windows,
  which just never draw any content at all -- the dock is the first
  wm-owned window with real content of its own.) It is also why
  `send_clip()` skips every window whose owner is `wm`: the dock is
  clipped to its visible region directly, by the same
  `window_visible_region()` the regions are computed from, rather than
  being told one over the wire.
- **It clears its own background.** Everything else `draw_dock()` does
  only *adds* ink: `z_fb_hw_box()` draws the slot outlines, and
  `draw_icon_bitmap()` sets an icon's 1-bits and leaves the 0-bits
  alone. So every pixel of the dock that is neither an outline nor an
  icon's own ink -- the gaps between slots, the padding, the dark
  inside of every icon -- was whatever the last thing to paint there
  left behind, and a repaint could not take it back.

  A window dragged onto the dock is exactly that. While it sits there
  its frame legitimately owns those pixels; when it leaves, the dock
  repaint puts the outlines and the icons back and leaves the departed
  window's two border columns standing inside the dock, one pixel wide,
  top to bottom -- 55 stray pixels in one measured case, all at the
  window's own x edges, and precisely in the zones nothing cleared (the
  two rows `draw_window_chrome_bg()` does clear came back correct).

  So the dock fills its interior first, clipped to its own visible
  region so a window legitimately covering it is not overpainted. That
  is the rule the rest of the compositor already follows: every pixel
  has exactly one owner, and the owner repaints it whole. The border
  itself belongs to `draw_window_box()` and is left alone.

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

**Every one of those calls loads this window's own region into `zgfx`
before it draws**, rather than trusting whatever region was in force
from the last thing that drew. The region is stored per window
(`z_win_t.clip`), not per process, and that distinction is not
theoretical: an app owns more than one window the moment it opens a
dialog, the two have different visible regions, and a
process-global region meant whichever arrived last won. The symptom was
a dialog whose button outlines and list-box frame sat outside its
parent's region and were clipped away.

`z_win_content_rect()` carries the same load, because it is the
chokepoint an app goes through before drawing with raw `z_fb_*` calls.
Putting it there rather than asking every app to remember is the whole
reason the guarantee holds for apps that were written before regions
existed.

### What an app owes the compositor

Four calls, all in `zwin.h`, and the first two are the ones that cost
nothing to get right and are expensive to get wrong:

| | |
|---|---|
| `z_win_apply_clip()` | on `Z_WM_SET_CLIP`. Returns false if the region belongs to another of this process's windows -- offer it to the next one rather than swallowing it. |
| `z_win_redraw_done()` | after servicing a `Z_WM_REDRAW`. Commits what is now on the glass, which is what the next damage is measured against. |
| `z_win_damage_rects()` | optional. What this redraw actually invalidated; ignoring it is correct, just slower. `z_win_damage_ignore()` gives the restriction back for a repaint that cannot be expressed as rectangles. |
| `z_win_frozen()` | optional but strongly advised for any periodic render. True while a drag is in progress: draw nothing, change nothing, keep it pending. |

`z_win_paint_begin()`/`z_win_paint_next_rect()`/`z_win_paint_end()`
are for an app repainting a large area under a multi-rectangle region:
they walk the region one hardware-scissor rectangle at a time, and
between `next_rect()` returning 1 and the following call, glyph blits
skip their per-cell scissor setup. `term` uses them for a full-screen
repaint; nothing else needs to. They must be paired and must not be
nested.

See `docs/app_runtime.md` for the full app-runtime picture this all
sits within (`zeitlos.h/c`, the syscall trampoline, process startup)
-- this document stays focused on the window manager's own protocol.

## Two coordinate spaces, and the trap between them

**`z_win_hw_line()` and `z_win_hw_box()` take absolute screen
coordinates. Everything else an app draws with is content-relative.**

| call | coordinates |
|---|---|
| `z_win_fill_rect()`, `z_win_draw_text()`, `z_win_clear()` | content-relative |
| `zwidget.c` (`z_widget_t.x`/`.y`) | content-relative |
| `z_win_hw_line()`, `z_win_hw_box()` | **absolute screen** |

The reason is in the API's own history: apps using the hardware
rasterizer typically already compute absolute coordinates (a 3D
projection, a clock hand), so those two calls take over clip-region
and IRQ-mask management but not coordinate translation.

**This is a footgun and it has cost real time.** The two families sit
next to each other in `zwin.h` and take identical-looking arguments,
so mixing them is easy — and it does not fail loudly. Frames land at
the window's screen position instead of its content origin: off by
(2, `Z_WM_TITLEBAR_H`+2) with the window at the top-left of the
screen, and off by the whole window position anywhere else. `sw/apps/logic`
had every frame, rule and lamp outline drawn this way, and it was
misdiagnosed twice as a spacing problem before anyone looked at a
render.

If a panel draws frames in content coordinates, wrap them once:

```c
static void abs_box(int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(&win, &clip);
	z_win_hw_box(&win, clip.x0 + x0, clip.y0 + y0,
		clip.x0 + x1, clip.y0 + y1, color);
}
```

and call nothing else. `sw/apps/mmod/panel.c` and `sw/apps/logic/logic.c`
both do.

## Rendering a panel on the build machine

`sw/common/tests/zrender.h` draws an app's panel on a host and writes
a PBM, so it can be **looked at** before it reaches a screen.

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
   sw/apps/<app>/tests/render.c \
   sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
   sw/common/zobj.c sw/common/zeitlos.c
/tmp/render /tmp/panel.pbm
```

An app's `tests/render.c` is around sixty lines: include `zrender.h`,
include the panel source, set up the state you want to see, call
`layout()` and `repaint()`, then `z_render_write()`.

### Why this rather than more assertions

`sw/apps/logic`'s panel shipped wrong three times, and its arithmetic
test passed every time:

1. Window coordinates where content coordinates were needed, so the
   bottom 15 rows were off the window.
2. Widgets inside the window but across the frames they were meant to
   be inside.
3. The coordinate-space bug above — which was the actual cause of 1
   and 2.

After each one the assertions were extended, and each time the next
mistake was of a kind the new assertion did not cover. That is the
shape of the technique, not bad luck: **a geometry assertion can only
check a relationship somebody thought to write down.** A render checks
every relationship at once, including the ones nobody anticipated —
text wider than its well, a frame over a readout, two things two
pixels apart that are legal and look wrong. The third bug was found in
one look.

It does not replace `tests/test_layout.c`, which runs unattended and
fails loudly on the relationships that *are* known. The renderer is
the step before shipping.

### What it does and does not run

`zwin.c`, `zwidget.c` and the app's panel are the **real sources** —
which matters, because the content-rect inset and the widget geometry
are where those bugs lived.

`zgfx.c` is **not linked**. Its box, line and fill primitives all
program the GPU rasterizer and blitter, which on a build machine write
to unmapped MMIO and draw nothing. So `zrender.h` supplies software
implementations of the dozen `z_fb_*` entry points `zwin.c` and
`zwidget.c` call.

That split is deliberate: pixel plotting is not where layout bugs
live. A wrongly *drawn* line is obvious the moment you look; a wrongly
*placed* one is not. It does mean a render **cannot catch a bug in
`zgfx.c` itself**, or one that depends on the blitter's exact
clipping.

### Practical notes

- Populate the panel with the **longest text each field will ever
  hold**. An idle panel of dashes hides exactly the overflow this
  exists to find.
- The image is exactly the content rect, so anything drawn outside it
  is missing rather than shown — which is itself the signal.
- Scale up. At 1x a 1bpp panel is too small on a modern display to
  judge, and judging it is the point.
- `maskirq()` (`sw/common/zeitlos.h`) has a non-RISC-V branch so the
  window layer compiles on a host at all.
- Linux and x86-64 only; it needs `MAP_FIXED_NOREPLACE` at a low
  address, and exits 77 elsewhere so CI skips rather than fails.

Existing examples: `sw/apps/mmod/tests/render.c`,
`sw/apps/logic/tests/render.c`.

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
- The hardware blit **clips now**, and the cell is clipped along with
  the glyph bits -- glyph mode paints a solid cell, so masking only
  the bits would let the background erase pixels outside the region.
  `z_fb_draw_char` still checks whether the glyph is fully on-screen
  and falls back to the per-pixel software path if not, because an
  out-of-range destination can hang the blitter regardless of
  clipping, and because a font that is not resident in glyph memory
  has to go through software anyway. What no longer forces the
  fallback is a glyph landing partly outside a window's edge or
  outside its visible region.
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

- **A partially occluded window still cannot scroll in hardware** --
  though it no longer loses the scroll merely for having a region.
  `z_fb_hw_scroll()` and `z_fb_hw_blit_vram()` were refused whenever a
  visible region was set at all; `copy_region_allows_rect()` now allows
  the copy when the destination lies inside a single visible rectangle,
  so `term` keeps its hardware scroll while it is focused or covered
  somewhere else, and repaints instead only when the region genuinely
  cuts it. Several rectangles are still refused, because proving the
  union contains the destination needs coverage rather than
  containment. The underlying hardware limit is unchanged: the
  blitter's copy source aligns to the destination *word*, so a scissor
  that moves the first written word feeds it the wrong data, and
  rounding the region down to a word boundary would let the copy write
  up to 31 pixels over the window in front -- the bug regions exist to
  prevent. The real fix is still in `gpu_blit.v`'s shifter priming; see
  `docs/gpu_blitter.md`, "Copy and the scissor".

- **A visible region is capped at 8 rectangles.** Past that the region
  *shrinks* rather than dropping rectangles: a region larger than the
  truth is unsafe (an app draws over its neighbour) while one smaller
  is merely stale (pixels do not refresh until the next repaint).
  `sw/apps/wm/tests/test_region.c` asserts overflow produces zero
  unsafe pixels.

- **Acks are matched by pid, not by request.** `Z_WM_REDRAW_DONE`
  carries a window id but nothing matches on it, so a process with two
  windows answers for both with the same message. That was load-bearing
  while `repair_region()` waited, and it is not any more: the only
  waits left are the drag freeze (`freeze_all()`, which wants one ack
  per window and does match `Z_WM_CLIP_DONE` by window) and the
  disabled `DRAG_ORIGIN_REPAINT` path. If per-request matching is ever
  wanted, the fix is a request tag in `Z_WM_REDRAW` and its `DONE`, not
  more careful sequencing.

- **Reentrancy while waiting for an ack.** `repair_region()` no longer
  waits, so the case where an outer call's loop over `zorder` could be
  iterated against a table an inner, nested call had just changed is
  gone. The shape has not vanished, only moved and shrunk:
  `freeze_all()` and `wait_clip_ack_one()` both service other messages
  through `handle_message()` while they wait, so a `Z_WM_CREATE_WINDOW`
  arriving in that 300 ms window is handled from inside the drag-start
  path. Not memory-unsafe (the tables are fixed size), and it is one
  bounded window at the start of a gesture rather than one per repaired
  window.
- **A slow or unresponsive app no longer stalls the whole wm.** It used
  to, for up to `REDRAW_ACK_TIMEOUT` per repair. What is left is the
  drag freeze: an app that does not ack costs the first rubber band up
  to `FREEZE_ACK_TIMEOUT_MS` (300 ms), once per drag, and the drag then
  proceeds with that window not guaranteed still -- which shows as a
  trail behind the band rather than as a frozen pointer.
- **A resize does not freeze the desktop.** The move path does
  (`freeze_all()` at the first pixel of motion), the resize path does
  not: it blanks the window's own interior and XOR-draws the frame at
  the candidate size, which pairs correctly over its own blanked
  rectangle but rests on the same assumption the move drag stopped
  making -- that nothing else repaints under the band. In practice the
  band stays close to a window that is not moving, so there is much
  less of it over other windows' pixels; the honest description is that
  this has not been made symmetric yet, not that it does not need to
  be.
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

  Regions bound the damage this can do rather than removing it: the
  stale copy lands inside the app's own visible region, so it can no
  longer appear on top of the window in front. During a drag it cannot
  land at all, because the window is frozen -- and the `DREW` ack is
  exactly the report that an app tried.
- **~~The focused-window bold border can still get drawn over~~ --
  fixed, superseded, given breathing room, and finally removed
  entirely.** The whole entry below is now history: there is no focus
  border. It became untenable with visible regions -- a ring drawn
  outside a window's bounds sits on pixels belonging to whatever is
  underneath, and that window erases it the moment it repaints. Focus
  is the inverted titlebar now (see "Window representation"), which is
  inside the chrome `wm` owns and cannot be reached by any app. The
  1px margin `repair_region()` grew to service the ring is now dead
  weight and can go. Kept for the reasoning: Was:
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
  ack-misrouting through the real `rtl/arbiter_vram.v` + `rtl/mem/vram.v`
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
  window-table slot) around forever. There is still no kernel
  notification, but the second half of that sentence has stopped being
  true: `z_proc_list()` (`Z_SYS_PROC_LIST`) reports every live process
  slot with its flags and allocates nothing, so a provider CAN ask.
  `sw/apps/repl` does exactly that to reclaim connections whose
  terminal was closed from its titlebar -- see `docs/ports.md`,
  "Nobody tells a provider that a client died", including the two
  things that shape gets wrong if copied carelessly. The same scan
  would work here.
- **Titles are still not drawn** in the chrome itself (font support
  now exists via `zgfx`, but `wm.c` draws chrome purely via the line
  rasterizer and hasn't been updated to render the title text yet).
- **No app-requested resize.** The user can resize a window that asked
  for `Z_WIN_FLAG_RESIZABLE`, but an app cannot ask to be resized, nor
  change its own minimum after creation.
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


## An app taking the screen

wm's game mode is a **camera**: a 320x240 viewport over an unchanged
desktop, which is exactly why windows keep working in it. An app's game
mode is a **takeover** -- it draws over the framebuffer the desktop
lives in.

They share one register, so wm cannot tell them apart by looking. It was
never told, so it carried on: a click changed focus, raised a window,
and painted it into pixels the game was showing. That was the bug.

### How they are told apart now

Two functions rather than a runtime test.

- `z_game_view_set_enabled()` in `zsoc.h` -- the register write, and
  nothing else. This is what **wm** calls.
- `z_game_set_enabled()` in `zeitlos.h` -- same name and signature as
  the accessor it replaces, so every existing caller became correct
  without a source change or a Makefile change. It writes the register
  and sends wm `Z_WM_GAME_GRAB`.

The notification could not live in `zsoc.h`: that header is included by
kernel-compiled code and pulls in nothing but `stdint` and `stdbool`, so
making a register accessor depend on the message layer would invert it.
It went in `zeitlos.c` because every app already links that object --
which is what made "no changes to the games" achievable rather than
aspirational.

`msg->from` is stamped by the kernel and ignored if a caller sets it, so
a grab cannot be claimed on another process's behalf.

### What a grab suspends, and what it does not

**Suspended:** the click that manages windows -- focus changes, raising,
dragging, the dock. That block is the one that paints over the game.

**Not suspended:** input. Keys and mouse both go to the grab owner
instead of to whatever the cursor is over, because an app owning the
screen is the only thing the user can see, so focus is meaningless. This
is not optional -- every game in `sw/apps` leaves game mode with Escape,
so a grab that silenced the keyboard would be a trap with no way out.

**Not suspended:** wm's own hotkeys. It is the only process that sees
every keystroke and that has to stay true whoever owns the screen.

### Alt+Esc during a grab revokes it

The camera is meaningless when an app owns the framebuffer, so Alt+Esc
hands the screen back, repaints the desktop, and sends the owner
`Z_WM_GAME_REVOKED`. An app that ignores that message is no worse off
than before; one that handles it can put itself back in a window.

It is also the only way out if the app has stopped listening.

### The failure that matters

**An app that owned the screen and has gone** -- crashed, closed, killed
-- must not leave wm suspended with a black desktop and no way back.
`destroy_window()` revokes the grab on that path. Every other failure
here is cosmetic; this one is unrecoverable without a reboot.


### An app that was already a window

`sw/apps/space3d` is the cheapest possible case and worth recording as
the pattern. Its window was already 320x240 -- exactly a game-mode page
-- and it establishes its drawing rectangle in one function,
`update_geometry()`. So full screen is not a different renderer; it is
the same one drawing into a different rectangle, and F2 costs a branch
there plus an enter/leave pair.

Two things it does that are not obvious:

**The erase list is dropped on a mode change.** It holds lines in the
OLD rectangle's coordinates, and replaying it would rub out pixels
somewhere else entirely. `drain_messages()` already drops it on a
redraw for the same reason.

**The redraw path does not ask for the content rect while full screen.**
Doing so would reload wm's clip region and confine the next frame to
wherever the window happens to sit on the desktop behind.

**And the `z_win_*` draw calls are bypassed.** This is the one that
actually bit: space3d draws through `z_win_hw_line()`, which clips to
the window's visible region. In full screen the window is still sitting
at its desktop position, so everything outside it -- most visibly the
top-left of the screen -- was never drawn at all. The app already does
its own Cohen-Sutherland clipping (that is the whole argument at the top
of `space3d.c`), so in game mode it draws straight to the framebuffer
and loses nothing; the clip the `z_win_*` layer would have applied was a
backstop either way.

`z_win_draw_text()` is worse, because it also takes **window-relative**
coordinates -- which in game mode are relative to nothing. That one
needs converting, not just bypassing.

Anything else adopting full screen should audit its draw calls for the
same thing. A `z_win_*` call is right in a window and wrong on a page,
and the failure is silent: the drawing happens, just not where anyone
can see it.

It also does not send `Z_WM_REPAINT` when leaving, unlike the older
apps: releasing the grab makes wm repaint by itself. Those calls are now
redundant everywhere, and harmless.
