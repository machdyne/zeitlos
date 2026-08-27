# read

A Markdown document viewer. `sw/apps/read`.

```
> run wm
> run read
```

Renders GitHub-flavoured Markdown from files of **any size**, with
working links. `.MD` files opened from the file browser come here
rather than to `text` — a Markdown file is something to read far more
often than something to edit.

## Files of any size

The document is never held in memory. That constraint drives the whole
design: the Timeless Computing book is 85KB assembled, and "any size"
means the next one may be worse.

Instead a **sparse index** is built by streaming the file once on
open. Every `idx_stride` source lines, a checkpoint records the byte
offset, the line number, and — crucially — the *parser state*. To draw
a screen at line L, the reader seeks to the nearest checkpoint at or
before L, restores that state, replays forward, and renders.

**The parser state is what makes this work.** Without it, a checkpoint
landing inside a fenced code block would resume thinking it was in
prose, and every `#` in the code would become a heading. That's why
`md_state_t` is small and trivially copyable.

The index is a fixed 384 entries and the *stride* grows to fit — a
document longer than 384 × stride lines doubles the stride and keeps
every other entry. That costs a little more replay per frame and no
memory at all, which is the right trade for a reader that must not
have a maximum file size.

Cost per frame is bounded by the stride plus a screenful, so it
doesn't grow with the document.

## Positions are blocks, not lines

A position is (block start, display line within it). Two reasons:

**Generated Markdown is hard-wrapped.** Pandoc splits link syntax
across source lines:

```
-   [Introduction to Timeless
    Computing](#introduction-to-timeless-computing)
```

A reader that renders one source line at a time shows raw brackets
where a link should be — and the book's entire table of contents is
made of these. So continuation lines are joined into one block before
parsing (`md_continues()`).

**Scrolling has to be reversible.** A block always wraps the same way
regardless of where it was entered from, which would not be true if a
position could land mid-block. `make render MODE=scroll` checks
exactly this: down N display lines then up N must land where it
started, from every position. 12,208 round trips across five real
documents, all exact.

Scrolling up therefore can't just decrement — it uses `prev_block()`,
which replays from the nearest checkpoint. Bounded by the stride, so
it doesn't get slower as the document grows.

## What renders

| construct | rendering |
| --- | --- |
| `# heading` | 6x12 font for h1/h2, 5x8 below; h1 gets a rule |
| `` `code` `` | inverse video |
| `[text](target)` | underlined, focusable, clickable |
| lists | marker in a hanging indent, wrapped text aligned under it |
| `> quote` | indented, with a rule down the left |
| fenced/indented code | preformatted, never wrapped |
| tables | verbatim |
| `---` | horizontal rule |
| `**bold**`, `*italic*` | markers stripped, no styling |

**Tables render verbatim, not laid out into columns.** The font is
fixed width, so a Markdown table's own alignment already lines up on
screen — and real column layout would need the whole table measured
before drawing any of it, which a streaming reader can't do. The
`|---|---|` separator row is dropped.

**There is no bold or italic face.** One weight per font, and
underline already means "link". Markers are stripped so the text reads
correctly. Inline code and links are the two inline styles a 1bpp
display can actually carry — and inline code is by far the most common
construct in this corpus, so it gets the one that reads best.

## Hand-written Markdown, not generated

The input is Markdown as a person wrote it. **Pandoc's output is not
supported**, and was deliberately un-supported after briefly working.

The Timeless Computing book's chapters are Markdown to begin with;
running them through Pandoc first only produces a downstream form
aimed at a print renderer. Reading the sources is both simpler and
better — the chapters render well, the generated form doesn't.

Supporting it needed three special cases: fenced divs (`:::`),
attribute blocks (`{#anchor}`), and Pandoc *simple tables*
(whitespace-aligned with a `----- -----` rule, which this parser turns
into a horizontal rule and wrapped paragraphs). Those constructs
appear **zero** times across the chapter sources and all 28 project
docs — they served exactly one generated file — and each is a rule
that can silently eat ordinary content. A line of colons in a document
about C++ scope resolution would have vanished.

Two things that came out of that exercise stayed, because neither is
Pandoc-specific:

- **Lazy continuation** of hard-wrapped lines. Any document wrapped at
  80 columns needs it, and several project docs are.
- **Telling a nested list item from an indented code block by
  context.** This occurs in the chapter sources independently of
  anything generated.

Distribute the chapters, not the assembled book.

## Links

Tab and Shift+Tab cycle the links **on screen** — tabbing to something
off screen would move the view without being asked. Enter follows the
selected one; clicking follows directly. The focused link is boxed
rather than underlined, since underline already means "link".

`#anchor` targets are resolved by slugifying every heading in the
document GitHub-style and scanning for a match. That's a full pass,
but it only happens on a click; an anchor table built at open time
would cost memory proportional to the heading count, which is what
this design avoids.

A link to a non-Markdown file hands off through `ztype` exactly as the
file browser does. External `http://` links say so rather than
silently doing nothing.

Backspace goes back, through a 12-deep history.

## Keys

| key | does |
| --- | --- |
| Space / PageDown | page down |
| Shift+Space / PageUp | page up |
| arrows | one display line |
| Home / End | start / end |
| Tab / Shift+Tab | cycle the links on screen |
| Enter | follow the selected link |
| Backspace | back |
| Ctrl+C, right-click | copy the selection |
| Escape | clear the selection |
| Ctrl+O | open |

## Opening draws the document once

Dismissing the Open dialog makes wm repair the region it covered,
which asks this window to redraw — and `zdialog` services that redraw
itself, before returning the chosen path (see `dlg_run()`, which has
to, or the caller's next filesystem access blows wm's ack timeout).

So the natural order was: draw the **old** document to fill the hole,
return the path, load the new file, draw the **new** document. Two
full renders, the first of them immediately thrown away and visibly so
on a document of any size.

A `loading` flag now covers the span from the dialog closing to the
new document being rendered. During it, a redraw clears and acks but
doesn't render the body: wm gets its ack on time, the area is blank
for the instant the load takes, and the text is drawn exactly once.

Cancelling the dialog clears the flag and repaints, since in that case
the window really does need its old content back.

Link navigation and Back don't go through a dialog, so they were
already single-draw.

That wasn't the whole story, though. `open_path()` also calls
`update_title()`, and `Z_WM_SET_TITLE` used to hand the *owner* a full
redraw request — which arrived after `do_open()` had already
repainted, producing a second render from the main loop. Fixed in wm;
see "Repairs that nobody can see" in `docs/window_manager.md`.

## Scrolling is one seek per page

Both directions do a **single forward pass**.

They didn't at first: `scroll_down()` called `block_subs()` per display
line, and that seeks — a file seek plus a replay from the nearest
checkpoint — so a page scroll did it thirty times over, re-parsing the
same blocks repeatedly. That's what made PageDown feel slow on a real
document.

Reading forward is naturally sequential: the stream is already
positioned after each block, so the next one costs a read and nothing
else. Scrolling up can't read backwards, so `collect_before()` walks
once from the nearest checkpoint recording block starts and their
display-line counts, then steps back through that list.

Measured on eleven real documents: **one seek per page**, down from
about thirty.

The scrollbar's page size is a real screen's worth of bytes, measured
from the last render (`bottom_off - top_off`). It was 1, which broke
clicking in the trough: `z_scrollbar_mouse()` pages by `page - 1`, so
a click below the thumb moved zero bytes and landed on the same block
— no scroll. Clicking *above* still worked, because the clamp at zero
happened to produce a different position. One cause, one asymmetry. It
also fixes the thumb size, which was always at its minimum regardless
of how much of the document fitted on screen.

Scrolling down **clamps** at the end of the document while scrolling
up does not, so a round trip there lands earlier than it started. That
is a reader refusing to scroll past the end, not a position bug, and
the reversibility test excludes those cases deliberately.

## Scrolling doesn't flash

Dragging the scrollbar produced a pointer sample every few
milliseconds, and each one re-parsed and re-rendered the whole
document — visibly a flicker, most of it spent on positions the reader
passed straight through.

Now a drag only moves the **thumb**, which is cheap and gives
immediate feedback, and marks the body dirty with a deadline. The body
redraws once the pointer has been still for 140ms, or immediately on
release. The thumb tracks your hand; the page arrives when you stop.

The main loop's `z_proc_wait()` takes that deadline as its timeout, so
a drag that ends by the pointer simply stopping still repaints.

A second source of flash was `repaint()` clearing the whole window on
every scroll. `draw_body()` fills its own region before drawing, so
that clear added nothing except a blank frame — at scroll rates,
exactly the flash it looked like. Scrolling now uses `repaint_body()`;
the full clear is reserved for `Z_WM_REDRAW` and resizes.

## Selection and copy

Drag to select, right-click or Ctrl+C to copy, Escape to clear.

Selection works over what was **drawn**, not over the source. The
source isn't in memory — that's the whole design — and a display line
is the result of block joining, inline stripping and wrapping, so the
only place the rendered text exists is a record kept as it's drawn.
What lands on the clipboard is therefore exactly what you could see,
with the markup already resolved.

It's drawn as an **overlay**: selected text is redrawn inverse after
the body. That also overrides the inverse used for inline code, which
is the right precedence and avoids threading two states through the
run splitting.

**A press doesn't follow a link.** Whether a press is a click or the
start of a drag isn't knowable yet, so it anchors a selection and the
decision happens on release: no movement means click, movement means
selection. Following immediately would make it impossible to select
the text *of* a link, which is often the text most worth copying.

**Scrolling clears the selection.** It's anchored to screen rows, so
scrolling would leave it highlighting whatever moved into those rows —
text nobody selected. Keeping it would need source-anchored positions,
which is a document model this app deliberately doesn't have.

## Testing

Two host suites, both run with the cross-compiler bypassed:

```
cd sw/apps/read
make test DOCS="../../../docs/*.md"    # parser
make render DOC=file.md MODE=scroll    # layout and scroll reversibility
```

`make test` is 14,747 checks over the book, its chapter sources and
all 28 project docs — asserting no overruns, no spans past end of
line, no spans pointing at absent links, and no document ending inside
a fence.

**Running it over real documents is what found the bugs.** Both of the
hard ones came from the corpus and neither would have been in a
hand-written case list:

- Hard-wrapped link syntax, which produced the block model. Hand-
  wrapped documents have the same problem.
- Nested list items indented four spaces being parsed as indented code
  blocks. Resolved by context (`md_state_t.in_list`) rather than by
  indentation alone, so a bullet inside a genuine code block is still
  safe.
