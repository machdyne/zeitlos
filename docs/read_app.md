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

### Where a scroll's time actually goes

Measured on hardware: a reader that draws with the same hardware
blitter as everything else was still slow, on a fast-memory board and
a slow-memory one alike, and equally slow with and without hardware
scrolling. So the cost was never drawing, and never memory latency
either. It was CARD READS.

Three things were paying for the same bytes repeatedly:

**The read buffer was 512 bytes.** Every refill is one SD read over
SPI, and a screenful of source is 2-3KB, so a single scroll cost a
double-figure number of card reads. Now 2KB: same code, a quarter of
the reads, 1.5KB more RAM.

**`rd_seek()` discarded the buffer unconditionally**, even when the
target byte was already in it. Seeks here are overwhelmingly short and
backward -- to the nearest checkpoint, or to the top of the screen --
so the target usually WAS in the buffer. Now it just moves the cursor
when it can.

**`seek_line()` restarted from the index on every call.** A one-line
scroll calls it twice -- `scroll_down()` to find the new top,
`draw_body_from()` to render it -- and each restarted up to
`idx_stride` (32) source lines back, re-parsing blocks it had read
moments earlier. Reading straight through a document meant the same
32 lines re-parsed on every keypress, each replay a seek plus card
reads.

There is now a POSITION CACHE: a checkpoint of the same kind as an
index entry (offset, line, parser state) but for wherever the reader
most recently was. `seek_line()` starts from it when it is at or
before the target and no earlier than the nearest checkpoint -- so it
can never make a seek longer, and never runs the parser backward,
which it cannot do.

Replaying forward from there is exactly as correct as replaying from
an index entry: both are a byte offset plus the parser state that
belongs at it, which is the premise the whole index rests on.
`make render MODE=seekcache` proves it rather than assuming it --
52,450 forward replays from arbitrary mid-points across three real
documents, every one landing in a parser state identical to replaying
from the start. That is the check that matters, because the failure
it guards against is silent: a fenced code block resumed as prose
renders every `#` inside it as a heading, and only when the reader
arrived from a particular direction.

### Scrolling forward moves pixels

A one-line scroll used to redraw the whole body -- 23 span draws and
26 block parses to shift the screen nine pixels. `scroll_forward()`
now blits the surviving pixels and draws only the strip the blit
exposed.

The shift comes from the STEP TABLE recorded during layout: one entry
per scroll step, including the blank lines and rules that cost a step
but produce no row, each holding where its block BEGINS (before
`space_before`). After k steps the new top is old step k, so the shift
is `step_y[k] - MARGIN` -- taken from the layout already in hand. The
first attempt at this measured the shift by laying the new screen out
a second time, and that pass cost more than the blit saved.

Then one layout pass draws the strip, and a VERIFY loop checks that
every step whose whole band sits above the strip really did move by
exactly that much, falling back to a full redraw otherwise. A step
reaching INTO the strip was redrawn regardless and is not required to
match -- requiring it rejects nearly every scroll, since the block at
the seam is precisely the one whose surroundings changed.

`make render MODE=seam` proves the result equals a full redraw pixel
row by pixel row: **24,864 simulated scrolls across five documents at
two widths, zero mismatches, ~98% taking the accelerated path.** The
remainder are page-sized scrolls that land past the last recorded
step, where a full repaint is right anyway.

A selection highlight or a focused-link box forces the full path:
both are overlays drawn on top of content, which the blit would
translate with nothing erasing the original -- the same ghost class as
the caret in `sw/apps/text`.

### Switching documents

Two bugs lived here, and the second was introduced by the buffered
reader optimisation above.

**Only one handle at a time.** `open_path()` opened the new document
before closing the old, so a failed open left the reader with what it
already had. Sound in principle, but `Z_FS_MAX_OPEN` was 4 at the time,
in the WHOLE SYSTEM, shared by every process -- and the file dialog
holds one or two of its own while it is up. Opening a second document
intermittently ran out and reported "Can't open" for a perfectly
readable file. It now closes first and reopens the previous path if the
new open fails, which keeps the safety and the handle.

The table is 8 now, raised when `hex` arrived (it holds one open
read-write for its whole lifetime), so this particular collision no
longer has to happen. Closing first is kept regardless -- it is the
smaller footprint, not a workaround.

**The buffer outlived the file.** `rd_seek()` serves a target straight
from `rbuf` when the offset falls inside it, without touching the
file. Offset 0 of a freshly opened document always falls inside a
buffer holding offset 0 of the previous one -- so a new document
rendered as the old one's contents, or appeared to load and then
not refresh. Harmless before that fast path existed, because every
seek discarded the buffer; adding the optimisation moved the
obligation to invalidate into `open_path()`, and it was not done.

Both are the same shape: a cache whose invalidation condition lives
somewhere the cache's author was not looking.

### Loading feedback

Opening a document blocks the app for as long as it takes. The window
used to keep showing the PREVIOUS document throughout, which reads as
the app having ignored the click -- worse than a blank area, because
stale text is indistinguishable from the document that was asked for.

`do_open()` now clears the body and draws "Loading <name> ..." BEFORE
the blocking call, and an expose during the load repaints the message
rather than a blank window. Deliberately not a busy cursor: this
blocks the app, not the system, and the cursor belongs to the window
manager.

### Measured after the blit: parsing is what is left

    total 7583k = seek 3k + parse 5466k + wrap 171k + draw 558k
                  + fill 3k  (rdline 2236k mdparse 1009k)
                  [26 blk 1 sub 0 io 0k]

Drawing fell from 7841k to ~560k -- one span redrawn instead of
twenty-three, which is the blit doing exactly what it was supposed to.
A scroll went from ~280ms to ~158ms.

What is left is almost entirely READING AND PARSING. Note also the
~1400k that appears in no bucket: that is `scroll_down()` walking
blocks on its own, outside `draw_body_from()`. Counting it, parsing is
about 84% of a scroll and drawing is 7%.

Inside that, `rdline` was consistently twice `mdparse` -- more time
spent getting bytes out of the buffer than parsing them. `rd_raw()`
called `rd_byte()` per character, so every byte of the document cost a
function call plus a "do I need to refill?" test. It now scans the
buffer for the newline and copies the run, with the refill test once
per buffer instead of once per byte.

That reader is not covered by the layout harness, which has its own
file access, so it has its own test: `rdraw_test.c` extracts the real
`rd_raw()` from `read.c` at build time -- not a copy that can drift --
and compares it against the original per-byte implementation over
whole files. 8,400 lines across eight files at three buffer sizes,
including CRLF, a file with no trailing newline, an empty file, blank
lines and 5000-character lines, checking both the line contents and
the final file offset (which matters, since `rd_tell()` feeds the
position cache). Zero mismatches.

**The step table also replaces scroll_down() on the fast path.** It
already says where n steps lands -- it was built by walking those very
blocks during the last layout -- so re-reading and re-parsing them to
reach the same answer was pure duplication, and it was the ~1400k that
appeared in no profiling bucket. Equivalent by construction rather
than by hope: the previous code called `scroll_down()` and then
CHECKED the result against this table, and that check passed 24,864
times across five documents before the table replaced it. The check
survives for the case it was actually catching -- a step past the
bottom of the screen, where a block cut short by `y + f->h > view_h`
lets the step count and the layout legitimately disagree.

### The layout cache

`layout_run()` now records where it STOPPED: the parser state, file
offset, source line and y before the first block that did not fit --
or after the last block, if everything fit. That is the whole cache;
it costs one `md_state_t` (6 bytes) and three integers.

A forward scroll whose new top lies within the completed blocks:

1. drops the rows, links and steps that scrolled off, and shifts the
   rest by the same amount as the blit
2. repositions the file to the cached offset (usually still inside
   the read buffer) and resumes the block loop from the cached state
   at `end_y - shift`, drawing only into the strip

Nothing above the resume point is read or parsed again. Decorations
(rules, quote bars, markers, underlines) need no cache at all -- the
blit carries them and only the strip is redrawn -- which is what keeps
this small.

Only COMPLETE blocks are cached. The block cut short by the screen
bottom is re-laid-out on resume, since more of it may now fit; its
rows stay in `vlines[]` for selection until then. A scroll whose new
top lands inside that block has nothing cached past it and takes the
one-layout-pass path instead.

This is what fixes the 150-block document: those zero-height blocks
get walked once, when they first scroll into range, and never again
while they stay above the resume point. Previously every keypress
re-walked all of them.

`make render MODE=seam` now models the resume itself -- old layout,
end state, blit, resume from the end state into the strip -- and
compares against a fresh full layout from the new top. **26,484
simulated scrolls across five documents at two widths, ~90% via the
cache, zero mismatches.**

Measured on hardware afterwards: a line-down went from ~5,500k cycles
to 500-2,000k, with `blk` in single digits instead of 26 (or 150).
That is 10-40ms.

**A bug the harness let through, and what it taught.** The first
version of the cache path forgot to clear the strip before drawing
into it -- `draw_body_from()` does that clear, and calling
`layout_run()` directly skipped it -- so the pre-blit bottom rows
stayed on the glass under the new ones. The harness passed because it
modelled the strip as already empty. It now models the glass
honestly: after the blit the strip still holds the old rows until a
distinct clear step removes them. With the clear omitted it fails
1,097 of 1,266 scrolls; with it, zero. The general lesson is the one
worth keeping: a harness must model what the hardware DOES, not what
the code is supposed to arrange, or it verifies the intention rather
than the result.

**A second bug, the other way round.** Headings and other elements
vanished when scrolling down. The block loop used to keep reading
blocks after one did not fit -- harmless when layout was a one-shot,
since y never decreases and nothing after a cut can draw -- but the
snapshot taken at the top of each iteration was being overwritten by
those later blocks, so the cache's resume point ended up PAST the cut
block and everything between. The harness had stopped at the cut all
along, which is why it saw nothing: this time the model was right and
the code diverged from it. `layout_run()` now stops at the cut.

Two harness changes came out of that. It now CHAINS: after a cached
scroll it performs a second line-down from the state the resume left
behind, because a resume that records its end state wrongly only
shows on the next scroll -- which is what reading actually does.
And with the overrun simulated in the model it fails 2,002 of 1,266
scrolls (both the direct and the chained check), zero otherwise. After a one-line scroll every block
but one is the same block, wrapped the same way, and `vlines[]` was
built to be redrawable without the parser. Keeping the laid-out screen
and parsing only what scrolled in would take the remaining ~5000k down
to roughly one block's worth.

### Measured: where a repaint's 13.5M cycles go

Steady-state one-line scroll on sergei_ml1, hardware glyph path
confirmed (`read: glyph blit HW` at startup):

    total 13424k = seek 3k + parse 4772k + wrap 31k + draw 7841k
                   + fill 152k   (rdline 2024k mdparse 882k)
                   [26 blk 23 sub 0 io 0k]

13.4M cycles at 48MHz is 280ms, which is what a scroll feels like.
The important entries:

- **io 0k, seek 3k.** The card is no longer the wall. The position
  cache and the 2KB buffer did their job; this used to be the whole
  problem and is now nothing.
- **draw 7841k for 23 spans** -- ~5,300 cycles per glyph, on the
  HARDWARE path. Seven register writes should not cost that; MMIO
  stores and the blitter-idle poll each stall the CPU, and at ~7-12
  cycles per instruction it adds up.
- **parse 4772k for 26 blocks**, of which only 882k is `md_parse`
  itself. `rdline` -- the byte-at-a-time reader and its one-line
  look-ahead -- is 2024k, more than twice the parser.

The conclusion is not that any one of these is pathological. It is
that the machine runs at ~6.9 MIPS and a repaint asks it to re-read,
re-parse, re-wrap and re-draw an ENTIRE SCREEN in order to move that
screen up nine pixels. The work is redundant, not slow.

So the two remaining optimisations are the ones that do less work
rather than the same work faster, and the measurement says to do
BOTH -- they are 54% and 36% of the total, and either alone leaves
the other:

1. **Move the pixels instead of redrawing them** (draw, 7841k). A
   hardware scroll blit plus one drawn row replaces 23 span draws.
   This was tried before and did not help, for two reasons that no
   longer hold: it was paired with a second full layout pass, and
   card I/O dominated everything at the time.

2. **Keep the laid-out screen instead of rebuilding it** (seek +
   parse + wrap, ~4800k). After a one-line scroll, every block but
   one is the same block, wrapped the same way, at a known offset.
   `vlines[]` already records what each row needs.

Together they take a repaint from ~13.4M cycles to something near
1.5M -- the one new block, one drawn row, and the blit. That is the
difference between 280ms and 30ms.

### The older note: one measurement, two fixes

After the I/O work above, a one-line scroll still redraws the whole
body -- 25-ish rows to move the screen up nine pixels. Whether that
costs mostly PARSING or mostly DRAWING decides which optimisation is
worth doing, and the two are different enough that guessing wastes a
build cycle either way.

Build it with:

    cd sw/apps/read
    make clean && make EXTRA_CFLAGS=-DREAD_PROFILE=1

`clean` first: `-MD` tracks header dependencies, not flag changes, so
a plain rebuild keeps the old objects.

`make CFLAGS+=-DREAD_PROFILE=1` also works now -- the Makefile uses
`override CFLAGS +=` so the required flags survive. It did NOT before,
and the failure was silent: a command-line CFLAGS replaced
`-DZ_GFX_HW_BLIT`, `-Os` and section GC, producing a software-rendering
build three times the size that crashed on start. See
`docs/app_runtime.md`.

**A trap worth knowing before adding any debug print to an app here.**
`read` calls `printf` exactly twice and neither call has a conversion
specifier, so the compiler rewrites both into `puts()`, the full
`vfprintf` formatter is never referenced, and `--gc-sections` drops
it. Adding a single `%lu` links the whole formatter back in:
`read.bin` goes from ~50KB to ~150KB, overruns the space the loader
has for it, and the app crashes on start rather than merely being
big.

That happened twice during this work, and both times the size jump
looked like a mystery rather than a consequence of the debug print
that caused it. The profiling code therefore formats its numbers by
hand and emits them with `fputs()` -- 684 bytes of text instead of
~100KB. If you add a print of your own here, check the binary size
afterwards.

It prints, after each repaint:

    read: repaint NNNk cyc = parse NNNk (N blocks, io NNNk, N refills)
                            + draw NNNk (N rows)

- **parse dominates** -- the screen is re-read and re-parsed to move
  it nine pixels. Fix: keep the laid-out screen across scrolls and
  shift it, parsing only the block that scrolled in. `vlines[]`
  already holds everything a row needs to be redrawn.
- **draw dominates** -- fix: stop redrawing 25 rows to change one.
  Move the pixels with the blitter and draw only the exposed strip.
  This was tried once and did not help, but it was paired with a
  second full layout pass that added back more than the blit saved,
  and the position cache above did not exist yet. Worth retrying on
  the measurement rather than on the memory.
- **io dominates** (large `io` inside `parse`, many refills) -- the
  card is the wall, and the next lever is a larger `RBUF` or reading
  ahead, not either of the above.

The counters are all-or-nothing per phase and cost nothing when the
flag is off. `rp_report()` prints from the event loop, never from the
draw path -- printf at the bottom of a deep call chain is how a 16KB
stack gets exhausted.

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
