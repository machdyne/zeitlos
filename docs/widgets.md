# Widgets and dialogs

App-side UI building blocks: the widget toolkit (`sw/common/zwidget.h`),
the scrollbar, the file-list widget (`sw/common/zflist.h`) and the
modal dialogs (`sw/common/zdialog.h`).

None of this is a UI framework. There is no layout engine, no widget
tree, no focus chain and no event bubbling. Each piece is something an
app would otherwise have to write twice, factored out and nothing more.
An app positions everything itself, in content-relative coordinates it
computed itself, usually in a `layout()` function it calls again on
every resize — `sw/apps/draw` and `sw/apps/text` are both written this
way.

Everything here draws through the GPU: bodies and fills through the
blitter, frames and rules through the line rasterizer, text and icons
through the glyph blitter. The one deliberate exception is
`z_widget_t`'s 16x16 button icons, which are software per-pixel — see
`zwidget.h`'s header comment for why.

## Coordinates

Two conventions are in play, and mixing them up is the most common
mistake in this area.

- `z_win_draw_text()`, `z_win_fill_rect()` and every widget rect are
  **content-relative**: `(0,0)` is the top-left of the window's content
  area, just below the titlebar.
- `z_fb_hw_line()`, `z_fb_hw_fill_rect()`, `z_win_hw_box()` and
  `Z_WM_MOUSE` payloads are **absolute screen** coordinates.

`z_win_content_rect()` (`zwin.h`) converts, and is the single source of
truth for where a window's content area actually is. Use it rather than
recomputing the inset — two copies of that formula disagreeing has
already caused one real bug in this codebase.

`z_win_mouse_content_xy()` turns a `Z_WM_MOUSE` payload into
content-relative coordinates, which is what every widget's `_mouse()`
function below expects.

Both `z_win_draw_text()` and `z_win_fill_rect()` measure x and y from
the content area. That is what `zwin.h` has always documented, but until
recently `z_win_draw_text()` measured x from the *window's* left edge
instead — two pixels further left. Text drawn at `x = 0` therefore sat
directly against the frame, defeating the inset that exists to stop
exactly that, and a `zwidget` button's label was centred two pixels left
of the frame it was supposedly centred inside. If you have an app that
was nudging its own text right by two pixels to compensate, that
compensation is now the bug.

### The blitter and the rasterizer do not order against each other

Two independent engines write the same VRAM, and nothing sequences
them:

- `z_fb_hw_fill_rect()` and the glyph blits behind `z_win_draw_text()`
  use the **blitter**, which waits for itself to go idle and
  read-modify-writes whole 32-bit words.
- `z_fb_hw_line()` / `z_win_hw_box()` use the **line rasterizer**,
  which only waits for FIFO space and returns with the line still
  queued.

So a rasterizer pixel that hasn't landed when the blitter reads a word
is lost when the blitter writes that word back — and the reverse
ordering is equally possible. It only bites where rasterizer chrome and
blitter glyphs land in the **same 32-pixel word**, which makes it
position-dependent and therefore easy to misread as a geometry bug.

The save dialog's filename field hit it: with the dialog at x=88 its
left border sat at absolute x=96, a word boundary, and the first six
glyphs of the field shared that word. The outline came up partly
missing and filled in as characters were typed, complete at six. At
x=100 it would have been four characters, at x=176 just one.

The fix there was to draw the whole widget — interior, frame and caret
— with the blitter alone, so program order is the drawing order. **If
you see chrome that is intermittently or partially missing next to
text, and the amount depends on where the window is, this is what it
is.** Draw the chrome with `z_fb_hw_fill_rect()` instead of
`z_win_hw_box()`; a one-pixel-wide fill is a perfectly good line.

### Fill through the blitter

`z_win_fill_rect()` and `z_win_clear()` go through the GPU blitter.
They did not always: they called `z_fb_fill_rect()`, which is a
per-pixel software loop doing one clipped VRAM read-modify-write per
pixel. Nobody noticed while the only callers were clearing a line of
text at a time. `z_win_clear()` is the same function with the whole
window as its rectangle, and clearing a 288x216 dialog that way measured
as about **three seconds** of blank window before its contents appeared.
A smaller dialog took proportionally less, which is the signature of an
area-proportional loop and how it was tracked down.

If you are filling a rectangle in a window, use `z_win_fill_rect()`.
Reach for `z_fb_fill_rect()` only when you specifically need its
per-pixel clipping against an arbitrary region.

**`z_fb_hw_fill_rect()` clamps to the SCREEN, not to your window.**
Handing it a rectangle that runs past your own edge paints over another
app. Anything that fills has to clamp to `z_win_content_rect()` first.
`fill_content_rect()` in `draw.c` and `fill_content()` in `text.c` are
the same six lines for exactly this reason.

## Scrollbars

```c
z_scrollbar_t sb;

z_scrollbar_init(&sb, &win, Z_SB_VERT);      // or Z_SB_HORZ
z_scrollbar_set_geom(&sb, cw - Z_SB_THICK, 0, ch - Z_WIN_GRIP_INSET);
z_scrollbar_set_range(&sb, total_lines, visible_lines);
```

`total` and `page` are in whatever unit suits the app — `text` uses
wrapped display lines, a picture viewer would use pixels. `value` is the
first visible unit, always clamped to `[0, total - page]`.

Visually it is a thumb and nothing else: no step arrows, no trough
frame. When there is nothing to scroll it draws **nothing** rather than
a full-length thumb, which is both quieter and unambiguous. Drawing is
two hardware fills — clear the bar, paint the thumb — with no per-pixel
work, which is what makes it cheap enough to redraw on every scroll step
without thinking about it.

`Z_SB_THICK` is 12, but that is a click target rather than a visual
weight: the thumb is drawn 8px across, inside it.

### Anything that blanks must dodge the grip

The resize grip is chrome drawn by wm in the window's bottom-right
corner, and all but its outer two pixels fall **inside** the content
area. Any app that blanks its content down to the bottom-right erases
most of it and leaves a half-drawn corner. Both `sw/apps/text`'s
`repaint()` and `sw/apps/draw`'s `clear_panels()` split the affected
fill in two for this: full width above the grip's rows, then
`content_w - Z_WIN_GRIP_INSET` wide across them.

### Leave room for the frame, too

A scrollbar **blanks its whole rect** before painting the thumb. Placed
flush against a bordered widget it therefore erases that border: in the
file list it landed exactly on the frame's right-hand column and spanned
its full height, taking out the right edge and both right-hand corners
every time it drew — and since the frame is drawn first and the
scrollbar last, the scrollbar always won. `z_flist_set_geom()` insets it
one pixel inside the frame on all three sides.

### Leave room for the resize grip

On a **resizable** window a scrollbar down the right edge must stop
short of the corner grip, or it swallows it — clicks meant for the
corner land in the trough and page the document instead, and the window
can no longer be resized at all.

`Z_WIN_GRIP_INSET` (`zwm.h`) is how far the grip reaches in from the
content area's bottom-right corner, so a vertical scrollbar wants
`len = content_h - Z_WIN_GRIP_INSET`.

Keeping the scrollbar clear of the grip is only half of it: **anything
that blanks the content area has to stay clear of it too.** The grip is
chrome drawn by wm, and all but its outer two pixels fall inside the
content area, so an app clearing its full width down to the bottom
erases most of the grip and leaves a half-drawn corner. See
`repaint()` in `sw/apps/text/text.c` for the two-rectangle version. It is `Z_WM_RESIZE_GRIP - 2`
because the grip is measured from the window's outer corner while an app
lays out against its content rect, and the two differ by the border
inset. A fixed-size window has no grip and can use the full height.

### In the message loop

```c
if (z_scrollbar_has_pointer(&sb, cx, cy)) {
    if (z_scrollbar_mouse(&sb, cx, cy, buttons)) {
        top_line = (int)sb.value;
        repaint();
    }
    return;
}

// keep its edge detector fed even when the pointer is elsewhere,
// so a button released outside it still ends the drag
z_scrollbar_mouse(&sb, cx, cy, buttons);
```

Two things about that shape are load-bearing:

- **Check `z_scrollbar_has_pointer()` first, and return if it's true.**
  `z_scrollbar_mouse()` returns whether the value CHANGED, not whether
  the click was its. Pressing the thumb changes nothing, so an app that
  treated a returned `false` as "not mine" would start selecting text
  the moment the user grabbed it.
- **Call it unconditionally afterwards anyway.** It does its own
  button-edge detection, and a button released outside the bar would
  otherwise leave a drag running forever.

The thumb drags proportionally; clicking the trough either side of it
pages. A drag deliberately does not cancel when the pointer leaves the
bar — wm's pointer capture (`Z_WM_MOUSE` in `zwm.h`) keeps delivering
events out there so it can keep working.

There is no scroll wheel anywhere in this system, so the finest
mouse-only adjustment is a page. That is a real limitation and the
reason step arrows existed in the first draft; they were dropped because
three extra hit regions and a pair of drawn triangles is a lot of
surface for something the keyboard already does better.

## The file list

`z_flist_t` is a scrolling, selectable directory listing with a
folder/file icon per row, a synthetic `..` row, its own scrollbar and
directory navigation. It is the list part of the file dialogs, factored
out on the assumption that a standalone file browser app wants the same
thing.

```c
z_flist_init(&fl, &win);
z_flist_set_geom(&fl, x, y, w, h);     // scrollbar included in w
z_flist_chdir(&fl, "/");
```

Then feed it input and act on the result:

```c
int r = z_flist_mouse(&fl, cx, cy, buttons);   // or z_flist_key(&fl, keysym)

if (r == Z_FLIST_ACTIVATED && !z_flist_activated_dir(&fl)) {
    char path[80];
    z_flist_selected_path(&fl, path, sizeof(path));
    // ... open it
}
```

Activating a **directory** navigates into it, and the widget has already
done so by the time it returns — so `Z_FLIST_ACTIVATED` really means
"something happened", and `z_flist_activated_dir()` is how you tell
"show me inside" from "this is my answer". That flag is latched at
activation time rather than derived from the current selection, because
by the time you ask, the selection is an entry of the *new* directory.

It **allocates nothing**. An app's stack and heap come out of one 16KB
allocation (`Z_PROC_STACK_SIZE_DEFAULT`, `sw/os/kernel.h`), and a widget
that mallocs every time you open a folder is a slow leak waiting to
happen. Storage is fixed: `Z_FLIST_MAX` (128) entries of
`Z_FLIST_NAME_MAX` (24) bytes, about 3KB of `.bss` per instance, plus a
3KB staging buffer shared across every instance in the process. A
directory with more entries than that comes back truncated, and
`z_flist_truncated()` says so — worth surfacing somewhere in any UI
built on this, since the alternative is a file that is definitely on the
card and simply never appears.

### Directory detection

`fs_list()` never reported which entries were directories; FatFs knew
(`FILINFO.fattrib & AM_DIR`) and the answer was being thrown away. There
is now an optional `types` out-buffer on `z_fs_list_args_t` (`zfs.h`)
carrying one `Z_FS_TYPE_FILE` / `Z_FS_TYPE_DIR` byte per entry, and a
new app-facing wrapper:

```c
fs_list_into(path, buf, buf_cap, types, max_entries, &count, &truncated);
```

A separate buffer rather than a trailing `/` on directory names, because
the names in the listing are documented as usable directly with
`fs_size()` / `fs_mallocfile()` / `fs_unlink()`, and decorating them
would quietly break every existing caller. Passing `types == NULL`
behaves exactly as the syscall always has.

A non-NULL `types` **requires** an explicit `max_entries`: that is the
only thing telling the kernel how many bytes the buffer holds, and the
handler rejects the call outright rather than guessing. Getting this
wrong would be an overrun of app memory, so it fails loudly.

## Dialogs

There is no dialog server and no special kind of window. A dialog is an
ordinary window the app creates itself with `Z_WIN_FLAG_MODAL` set. Each
entry point blocks until the user answers, then destroys the window:

```c
char path[80];

if (z_dialog_save(&ctx, "/", "notes.txt", path, sizeof(path)))
    write_the_file(path);

int r = z_dialog_confirm(&ctx, "Unsaved changes",
    "Save changes before\ncontinuing?", Z_DIALOG_YES_NO_CANCEL);
```

`z_dialog_confirm()` returns `Z_DIALOG_YES` / `_NO` / `_CANCEL`. Cancel
is 0, so the common "did they agree?" test is just
`if (z_dialog_confirm(...))`, and every way of backing out — the Cancel
button, Escape, the titlebar close icon — lands on the same value
without the caller enumerating them.

Its message is split on `\n` only, up to `Z_DIALOG_MSG_LINES` lines.
Write your own line breaks; for the two or three sentences a confirm box
holds that gives better results than automatic wrapping, and it's less
code.

### The callback is not optional

This is the part that bites.

While a dialog is up, wm carries on sending your app messages about its
*other* window — `Z_WM_REDRAW` when something uncovers it, and so on.
Those cannot be dropped. wm blocks waiting for a redraw ack (see
`docs/window_manager.md`, "Content z-order"), and an app that stops
acking freezes the whole screen until `REDRAW_ACK_TIMEOUT` fires.

So `z_dialog_ctx_t` carries a callback, and everything not addressed to
the dialog goes to it:

```c
static z_dialog_ctx_t ctx;

ctx.parent  = &win;
ctx.on_msg  = forward_msg;   // the SAME function the main loop uses
ctx.user    = NULL;
```

In practice you factor one function out of your main loop and point both
at it. `sw/apps/text` does exactly that — `forward_msg()` handles
`Z_WM_REDRAW` (repaint, then `z_win_redraw_done()`),
`Z_WM_WINDOW_MOVED` and `Z_WM_WINDOW_RESIZED`, and is called both from
`main()`'s default case and through `ctx.on_msg`.

Routing works out like this:

| message | routed by |
| --- | --- |
| `Z_WM_REDRAW` | window id — `z_win_redraw_id()` unpacks it |
| `Z_WM_WINDOW_MOVED` / `_RESIZED` | the `id` key in the map |
| `Z_WM_CLOSE`, `Z_WM_TITLEBAR_ICON` | the id in the packed payload |
| `Z_WM_MOUSE` | coordinates |
| `Z_WM_KEY` | **always the dialog's** |

That last row is the whole reason dialogs are modal rather than merely
on top. `Z_WM_KEY` carries no window id and no coordinates, so there is
nothing to route by — an app with two windows open genuinely cannot tell
which one a keystroke was meant for. Modality removes the ambiguity by
construction: while a modal window is up, every key belongs to it.

### Creating a window while already running

`z_win_create()` and friends block on `z_msg_wait()`, which **discards**
every message that isn't the one it's waiting for. At startup that's
harmless — nothing else is in flight. For an app creating a *second*
window while running it is not: a `Z_WM_REDRAW` sitting in the queue at
that moment gets silently dropped, and wm is left waiting for an ack
that never comes.

`z_win_create_cb()` (`zwin.h`) is the fix — same call, but non-matching
messages go to a callback instead of the floor. `zdialog.c` uses it, and
so should anything else that creates a window after startup. The symptom
of getting this wrong is the screen freezing for a moment and
`wm: timed out waiting for pid N to ack a redraw` on the console, which
looks nothing like "somebody opened a dialog".

### Redraws while a dialog is open, and after it closes

Two hazards here have already bitten once each, both showing up as
`wm: timed out waiting for pid N to ack a redraw` and a multi-second
freeze:

- **While the dialog is being created.** Any `repair_region()` that
  overlaps the not-yet-created window must pass `exclude_idx` for it.
  Its owner is still inside `z_win_create_cb()` and has no window id to
  recognise a redraw request by, so it cannot ack. `wm.c`'s modal
  focus-change repair needs this just as much as the create-time repair
  does.
- **After the dialog is destroyed.** wm repairs the vacated region and
  blocks on the parent's ack, but the caller typically goes straight
  from `z_dialog_save()` into writing a file — an SD round trip long
  enough to blow the timeout. `dlg_run()` therefore services that
  redraw itself, in a bounded pump, before returning.

Related, in wm rather than here: a click on an already-frontmost window
used to claim a z-order change it hadn't made, because `bring_to_front()`
reports movement and the dock is pushed back to the front immediately
afterwards, cancelling it out. That triggered a full `repair_region()` —
two repaints and two ack round trips — on **every click**. It is now
tested by comparing the z-order before and after. Harmless when apps
owned one window each; very noticeable with a dialog open.

### Storage

The dialogs allocate nothing either. State is one file-static struct in
`zdialog.c`, shared across all three, which is sound because exactly one
dialog can be open at a time — every entry point blocks until its own is
dismissed, so a second cannot start while a first is running. That is
what "modal" means, and it lets the 3KB file list be shared rather than
duplicated per dialog type. A per-call struct on the stack would put
that same 3KB on a 16KB stack, underneath whatever the app was already
doing.

### What the dialogs deliberately don't do

`z_dialog_save()` does not check whether the file already exists or ask
about overwriting. Only the caller knows whether overwriting is a
problem in its case, so that is its `z_dialog_confirm()` to show.

The file dialogs are a fixed size and not resizable. Making them
resizable means handling `Z_WM_WINDOW_RESIZED` and relaying out
mid-loop, for a window the user looks at for a few seconds.

## Files, in an app that has a document

`sw/apps/text` and `sw/apps/draw` both do new/open/save the same way,
and the pattern is worth copying rather than reinventing:

- **`Z_WIN_FLAG_NEW_ICON | _OPEN_ICON | _SAVE_ICON`** on the window,
  and a `Z_WM_TITLEBAR_ICON` case in the message loop dispatching to
  `do_new()` / `do_open()` / `do_save()`.
- **`Z_WIN_FLAG_CLOSE_ICON` WITHOUT `_CLOSE_KILLS_OWNER`.** An app that
  shows dialogs owns more than one window, and the killing form takes
  every window of a pid down the instant any one is clicked closed. It
  also has to be the non-killing form so closing with unsaved work can
  ask first. `draw` had to change here: it was single-window and used
  the killing form correctly until it grew dialogs.
- **`confirm_discard()`** before anything destructive, returning false
  if the user cancelled. If the user says yes-save and the save then
  fails or is itself cancelled, the whole operation is off — silently
  discarding after a failed save is the worst reading of "yes, save
  it".
- **Read with `fs_open_read()` + `fs_read_chunk()` into the app's own
  buffer**, not `fs_mallocfile()`. That allocates a second copy of the
  whole file out of a heap that is 16KB shared with the stack
  (`Z_PROC_STACK_SIZE_DEFAULT`), so for `draw`'s 37.5KB canvas it could
  never succeed, and for a 20KB text document it would fail for no
  reason the user could guess.
- **A `set_modified()` that also updates the titlebar**, via
  `z_win_set_title()` — both apps show the filename with a leading `*`
  while dirty.

`draw` saves raw bitmap data: the canvas array verbatim, 480 rows of 20
words, 38400 bytes, no header. That is exactly what the blitter already
reads and is one line of Python away from a PNG, but it carries no
record of its own shape — if `CANVAS_W`/`CANVAS_H` ever change, every
existing file becomes unreadable garbage and the only guard is a length
check. A header is the obvious next step if the canvas size stops being
fixed.

## Launch arguments

There is no `argv`. `z_proc_run()` takes a program name and nothing
else, so a launcher that wants to say "open *this* file" has nowhere to
put the filename.

Rather than reserve space in every process table entry for something
used once at startup, **wm holds a single pending argument**:

```c
z_launch_arg_set(path);      // launcher, immediately before...
z_proc_run(app);             // ...starting the program

char arg[96];                // new app, early in main()
if (z_launch_arg_take(arg, sizeof(arg))) open_it(arg);
```

One slot is enough because only one app is ever mid-launch at a time —
wm's dock already assumes this — it is sized in one place
(`Z_WM_ARG_MAX`), and growing it for longer paths costs one constant
rather than sixteen process entries.

Two properties make it safe:

- **Claiming is destructive.** wm marks the slot empty as soon as it
  answers, so a second app starting later gets nothing rather than
  re-opening the previous app's file.
- **It expires** (`Z_WM_ARG_TIMEOUT`, about four seconds). Without
  that, an argument set for an app that never asks — an older build, or
  one that dies during startup — would sit in the slot and be collected
  by whatever the user launched next, which is a confusing way for the
  wrong file to open.

`z_launch_arg_take()` blocks on wm's reply through `z_msg_wait()`,
which discards anything else that arrives meanwhile. **Call it early,
before creating a window** — at that point nothing else is in flight,
which is exactly why it is safe there and would not be later.

An app that expects no argument should still claim one if it might be
launched by the browser, so a stale value can't be left pending.

## File types

`sw/common/ztype.h` maps extensions to apps — `TXT`/`ASC`/`MD` to
`text`, `ZBM` to `draw`. One table, in `sw/common` rather than inside
the browser, because a shell `open` command or a future desktop asks
the same question and none of them should carry their own list. Adding
a type is one line.

Matching is by extension, case-insensitively, and nothing else. There
is no content sniffing: FAT short names give every file a
3-character extension, and reading the head of every file in a
directory just to draw a list would be a lot of card traffic for the
benefit.

**A file with no extension is the one exception, and it is checked.**
`z_ftype_is_executable()` reads the first four bytes and requires the
`ZEXE` magic, because the loader will not check for us — `zexec.h`
treats a file *without* the magic as the legacy raw executable format
and loads and jumps into it regardless. Without that check,
double-clicking a `README` would execute it.


## System information

`z_proc_list()` and `z_mem_stats()` (`sw/common/zproc.h`) return the
process table and heap figures as data. `reg_csr_features`
(`sw/common/zsoc.h`) is readable directly from an app. `sw/apps/info`
uses all three; see `docs/info_app.md`.

Two things changed there that affect everyone:

- **`z_proc_info_t` gained a `name`**, filled from the pid registry.
  Only processes that called `z_pid_register()` have one; the rest
  report `""` and a caller should fall back to the pid.
- **`Z_PROC_FLAG_ACTIVE`/`_DIE`/`_BLOCKED` moved** from
  `sw/os/kernel.h` to `sw/common/zproc.h`. An app receiving those flags
  needs to interpret them, and `kernel.h` isn't includable from app
  code. `kernel.h` includes `zproc.h` now, so there's one copy.

`z_proc_info_t.cpu_ticks` is real per-process CPU time: the kernel
charges each KTIMER tick to whichever process it interrupted. A
percentage is the difference between two samples over the elapsed
ticks between them — see `docs/info_app.md` for the pitfalls (sampling
resolution, first sight of a pid, pid reuse).

`Z_PROC_FLAG_BLOCKED` remains useful as "is this waiting or working",
and is what the scheduler itself acts on.

## Drawing a live display cheaply

`sw/apps/info` refreshes once a second and is meant to be left open, so
it's the worked example for keeping that cheap:

- Block between samples with `z_proc_wait()`, never spin. A status
  display that busy-waits distorts the load it reports.
- Full repaint only on `Z_WM_REDRAW`; the periodic update redraws only
  what changed.
- Avoid `printf` in the draw path — newlib's formatter is large and
  slow. A handful of small integer-to-string helpers covers most needs.
- Bars and charts through `z_fb_hw_fill_rect()`, frames included. Using
  the line rasterizer for a frame and the blitter for its fill puts two
  engines with no ordering between them on the same VRAM words; see
  `field_draw()` in `sw/common/zdialog.c`.


## Keyboard focus

Keyboard-only operation is a first-class case (see
`docs/window_manager.md`), so a set of buttons reachable only with a
pointer is a dead end. `z_widget_set_t` carries a `focused` index, and:

```c
z_widget_focus_next(&set, backward);   // Tab / Shift+Tab
z_widget_key_activate(&set);           // Enter / Space
z_widget_focus_set(&set, idx);         // or clear with -1
```

The focused widget draws a ring one pixel **outside** its own frame —
the same visual language wm uses for the focused window, and outside
rather than inside so it never eats into the label's space.

`z_widget_key_activate()` does exactly what a click does, by design:
the same widget gives a keyboard user and a mouse user the same result,
rather than two implementations that drift apart. A click also moves
focus, so Tab afterwards resumes from where the user is looking.

Default is `-1`, so an app that never calls these behaves exactly as
before they existed.

**Two constraints that are easy to get wrong:**

- The ring is drawn on *every* redraw — in ink when focused, in
  background when not. Drawing it only when focused leaves the old
  ring on screen when focus moves on, because repainting the losing
  widget touches its body and frame but not the pixels outside them.
  Rings then accumulate on everything that has ever been focused and
  focus appears never to leave.
- Because the ring sits one pixel *outside* the widget, **a set that
  uses focus needs at least 2px between widgets.** In a grid where
  they abut — `sw/apps/draw`'s tool column, 20px cells with no gap —
  erasing a ring would rub out the neighbour's frame. That set never
  calls the focus functions, and `focus_used` keeps the ring machinery
  entirely dormant until something does.

Tab is the consistent binding across the system: `settings`, `files`
and every dialog. Arrows are bound *additionally* where they're
unambiguous — a radio group with nothing else on the window — and
deliberately not where they'd fight a list.

### Labels on a lit widget

`z_widget_draw()` uses `z_fb_draw_text2()` for labels, not
`z_win_draw_text()`. `z_fb_draw_char()` hardcodes its background to 0
and exposes only the foreground, so a label on a **lit** body — where
the ink has to be 0 to show against a filled background — asked for
fg=0 with bg=0, and every glyph cell came out solid background. The
label didn't dim or misalign; it vanished, leaving a block.

That stayed hidden because a push button is lit only while held, so
the label blinking out read as a press effect. A selected radio member
is lit permanently, and then it's just broken.

**The tab order usually spans more than the widget set.**
`sw/apps/files` is the worked example: its order is list → Open → New
Folder → Delete → list, and the list isn't a `z_widget_t` at all. That
ordering lives in the app, which is the only place that knows about
both. Note arrows deliberately do *not* move between buttons there —
they belong to the list, and Tab is the only way across.

## Clipboard

System-wide, hosted by wm for the same reason the launch argument is:
it has to outlive the app that produced it, and wm is the one process
guaranteed to be running whenever this matters.

```c
z_clip_set(&buf[start], end - start);   // len of -1 means "to the NUL"

char clip[Z_WM_CLIP_MAX];
int n = z_clip_get(clip, sizeof(clip));
```

Reads are **non-destructive**, unlike the launch argument — pasting
twice pastes the same thing twice — and there is no expiry. A
clipboard that quietly emptied itself would be worse than useless.

`Z_WM_CLIP_MAX` is 4KB, one static buffer in wm's `.bss`, so it's a
fixed cost paid once rather than per process. That's about a page of
dense text, which is what this is for: moving a paragraph between
windows, not moving a file. Over-long copies are **truncated at the
set**, not refused — losing the tail is a better failure than the
paste doing nothing, and the app can see what came back.

`z_clip_set()` copies into a staging buffer before sending, because
the payload is borrowed by wm until it reads the message and the
caller's buffer is typically a slice of a document about to be edited.
That buffer is another `Z_WM_CLIP_MAX` of `.bss` — but it lives in
`zwin.c` referenced only from `z_clip_set()`, so `--gc-sections` drops
it entirely in an app that never copies. Only apps that use the
clipboard pay for it.

`z_clip_get()` blocks on wm's reply through `z_msg_wait()`, which
discards anything else that arrives meanwhile. Call it from a key or
click handler having just drained the queue — the same constraint
`z_launch_arg_take()` documents.

### Text conventions

Ctrl+C / Ctrl+X / Ctrl+V, with Shift turning any caret movement into a
selection. `sw/apps/text` is the reference implementation.

**`term` is the exception and uses Ctrl+Shift+C/V.** Ctrl+C in a
terminal is `^C` to the remote end — the single most-used key in a
shell — and rebinding it would be indefensible. See
`docs/terminal.md`, which also covers where terminal line editing
lives (not in `term`).
