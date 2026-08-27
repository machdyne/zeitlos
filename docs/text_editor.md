# text

A plain, fast, word-wrapping notepad. `sw/apps/text`.

```
> run wm
> run text
```

Deliberately not a programmer's editor. `sw/apps/repl` already embeds
`te` (`docs/editor.md`), which is modal, vi-flavoured and built around
lines as the unit of everything. This one is for notes, letters and
stories — so it wraps, it has no modes, and every key that isn't a
command inserts itself.

It is also the first app to use the file dialogs and the scrollbar
widget, so it doubles as the worked example for `docs/widgets.md`.

## Using it

Titlebar buttons, left to right: **new**, **open**, **save**, **close**.
The title shows the current filename, prefixed with `*` when there are
unsaved changes.

| key | does |
| --- | --- |
| any printable character | inserts it |
| Enter | new paragraph |
| Tab | four spaces — see "Tabs" below |
| Backspace / Delete | delete before / after the caret |
| arrows | move the caret; up and down keep the column |
| Home / End | start / end of the display line |
| PageUp / PageDown | a screen at a time |
| Ctrl+N / Ctrl+O / Ctrl+S | new / open / save |
| Ctrl+Q | close |
| Shift + any movement key | extend the selection |
| click and drag | select |
| Shift+click | extend the selection to here |
| Ctrl+A | select all |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste |
| right-click | copy the selection |

Clicking places the caret. Clicking past the end of a line, or below the
last one, clamps to the nearest real position rather than being ignored
— clicking in the empty space under a short document puts the caret at
the end of it, which is where you were pointing.

Anything that would discard unsaved work — new, open, close — asks
first, and offers to save. If that save is then cancelled or fails, the
whole operation is off; silently discarding after a failed save would be
the worst possible reading of "yes, save it".

The window is resizable. A resize rewraps the document rather than
scrolling it. The scrollbar stops short of the bottom-right corner so
the resize grip stays clickable — see `Z_WIN_GRIP_INSET` in
`docs/widgets.md`.

## The document

One flat `char` array, `TEXT_MAX` = 32KB, with insert and delete done by
`memmove`.

Not a gap buffer, and that is a choice rather than an oversight. At 32KB
the worst-case move is the whole buffer — a few milliseconds on this
CPU, and only when typing at the very start of a full document. A gap
buffer would remove that and add a second representation of "where the
text is" that every function in the file would have to understand. If
typing ever feels heavy on real hardware, this is the first thing to
change, and the change is confined to `buf_insert()` / `buf_delete()`.

The buffer is not NUL-terminated as a rule; `len` is the length and the
text may contain any byte. Anything handing a string to a drawing call
copies out a bounded piece first.

Files are read with `fs_open_read()` and `fs_read_chunk()` straight into
that buffer, **not** with `fs_mallocfile()`. The latter would allocate a
second copy of the whole file out of a heap that is 16KB shared with the
stack (`Z_PROC_STACK_SIZE_DEFAULT`, `sw/os/kernel.h`), so a 20KB
document would fail to open for no reason the user could ever guess.
CRLF is normalized to LF on the way in.

## Wrapping

`line_off[]` holds the buffer offset each display line starts at.
`MAX_LINES` is 4096 — unreachable for 32KB of prose, which wraps to a
few hundred, and only approached by a file of nothing but newlines. At
the cap the document stops being *displayed* past that point; it is not
truncated on disk and editing above the cap still works. Cost is 8KB of
`.bss`.

`uint16_t` offsets are exactly wide enough for a 32768-byte buffer. **If
`TEXT_MAX` ever goes to 64K this must become `uint32_t`** — an offset of
65536 would wrap to 0 and the failure would look like the document
silently folding in half.

The property everything else rests on is that **wrapping is
paragraph-local**: where a line breaks depends only on the text since
the last `\n`. So an edit can never change how anything before its own
paragraph is laid out, and every edit rewraps from the start of its own
paragraph rather than from the top of the document. That is what keeps
typing cheap in a long file — `para_line_at()` finds the start,
`wrap_from()` rebuilds from there. Only a resize rewraps everything,
because a column-count change genuinely can move every line.

Lines include their terminating `\n`, and a wrapped line includes the
space it broke after, so every byte of the document belongs to exactly
one display line and nothing has to special-case "is the character
before me a newline".

A wrapped line also **absorbs the whole run of spaces at its break**, so
a line can never begin with one. Without that, two ordinary cases
produce a line holding nothing but spaces, which renders as a blank line
in the middle of a paragraph: a word that exactly fills the width
followed by a space, and a double space after a full stop that happens
to straddle the break. The absorbed spaces make a line longer in bytes
than the column count, which is why `line_col_max()` exists — trailing
spaces are invisible, `draw_row()` clamps what it paints, and the caret
clamps to the same limit so it can't follow them out over the scrollbar.

A word longer than the whole line is broken mid-word. Letting it run off
the edge would be prettier and less useful: a URL or a line of pasted
code should still be readable.

## Drawing

Every glyph goes through the hardware glyph blitter, via
`z_win_draw_text()` (`-DZ_GFX_HW_BLIT`). Text is drawn a display line at
a time, and an edit repaints only from the edited line down — reflowing
a paragraph can move every line after it, but never one before. A cursor
move repaints just the caret's old row and its new one.

`repaint()` also blanks the strip between the text and the scrollbar and
any partial row at the bottom, because `wm` clears before most redraws
but **not** after a move (`repair_drag()` in `wm.c` deliberately
excludes the window's own final footprint), so anything not actively
rewritten keeps its pre-move contents. Same reasoning as `draw`'s
`clear_panels()`.

The caret is a 1px vertical rule between characters, not a filled block
over one. A block caret hides the character it sits on and is also just
wrong about where the text will go, for something that lives *between*
two characters.

## Tabs

Tab inserts four spaces rather than a tab character. `wrap_one()` counts
columns, and a real tab has no fixed width in that count, so honouring
one would mean teaching both the wrapper and the renderer about tab
stops. A tab can therefore only appear in text loaded from disk, and is
drawn as a single space so it at least occupies its one column.

## Font

Two fonts, toggled by the titlebar font button: `z_font_5x8` and
`z_font_6x12`. Both are resident in glyph memory at fixed offsets and
loaded once by `wm`, so both draw through the hardware blitter — see
`docs/window_manager.md`, "Fonts in glyph memory".

Switching changes the column count, so the document rewraps from the
top. That is the same case a resize is, and the one place
`wrap_from()`'s paragraph-local shortcut cannot help, since every line
can move.

The caret stays on the same **character**, not the same screen
position: `cursor` is a buffer offset and survives the rewrap
untouched. That is precisely why the buffer and the line table are
separate things.

## Launched with a file

If the file browser (or anything else) starts `text` with a pending
launch argument, it is claimed at startup and opened — see "Launch
arguments" in `docs/widgets.md`. The claim happens even when the
editor doesn't want it, so a stale argument can't be left for whatever
the user opens next.

## Selection

An **anchor** plus the cursor. The selected range is whichever order
they happen to be in, so extending backwards needs no special case and
the caret stays the moving end — which is what makes shift+arrow and
shift+drag feel right.

`-1` means no selection, deliberately rather than `anchor == cursor`:
a selection can collapse to zero width mid-drag without ceasing to
exist, and treating that as "none" makes the next shift+arrow start
from the wrong place.

Selected text is drawn by splitting each line into up to three runs —
before, inside, after — and drawing the middle one through
`z_fb_draw_text2()` with the colours swapped. That's one hardware
glyph blit per character, exactly like the normal path; inverting
afterwards with a fill would be a second pass over the same pixels and
would fight the no-flash rule. A selection continuing onto the next
line also highlights the rest of the row past the text, or a
multi-line selection reads as a stack of ragged fragments.

Movement redraws the **union of the old and new selected ranges**, not
just the two rows the caret touched — with a selection those ranges
are what changed appearance. Repainting the whole text area on every
shift+arrow would be hundreds of glyph blits per keystroke.

`delete_selection()` is shared by every path that replaces a selection
— typing over it, Backspace, Delete, Cut and Paste — so one place
knows how a selection becomes an edit. Paste does the delete and the
insert as a single edit rather than calling both, which would rewrap
and repaint twice for one user action.

Shift+click needs the modifier state, and `Z_WM_MOUSE` carries buttons
but no modifiers, so the last-seen modifier byte from `Z_WM_KEY` is
correlated with the click.

### Three bugs worth remembering

All three presented as *"text isn't displayed"*, which is nowhere near
where any of them lived.

**Half-clamped range.** `draw_row()` clamped `a` only against 0 and
`b` only against `n` — each end in the direction that matters for a
selection *overlapping* the line. For a line entirely outside it, the
other direction bites: a line past the selection's end gets a negative
`b`, and the trailing run then draws `tmp + b`, reading before the
buffer, at a negative x that clips away entirely. The line vanished.
Which lines vanished depended on where the selection was, so it looked
like an unrelated rendering fault.

**Stale collapsed anchor.** A click sets `anchor == cursor`, which is
not a selection. Nothing cleared it, so the moment an edit moved the
cursor, `anchor != cursor` and a selection sprang into existence that
the user never made — which, with the bug above, hid everything after
it. `after_edit()` now clears the anchor, and a release that selected
nothing drops it.

**Titlebar clicks.** wm forwards pointer samples to the focused window
whenever the cursor is over it, and its hit test is the whole window
rect — titlebar included. A drag is suppressed while wm owns it, but a
click on a titlebar *icon* is not: wm handles the icon and forwards
the same press, with negative content y. Clamping that to row 0 meant
clicking Save also placed the caret at the top and started a selection
there. `handle_mouse()` now ignores samples outside the content area
unless a drag is already in progress — that exemption matters, because
a drag legitimately leaves the window and must keep extending.

## Not implemented

No undo, no search. Each wants a real design
rather than a corner of this file, and none is needed to write a note.
