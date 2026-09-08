# HTML parsing and layout

How `sw/apps/web` turns a stream of bytes into something on a
640x480 1bpp screen, and which properties have to hold for scrolling
to work.

See [web_app.md](web_app.md) for the browser as a whole.

---

## The pipeline

```
  bytes --> html.c --> html_line_t blocks --> layout.c --> pixels
             |                                   |
             +-- uni.c: UTF-8 -> ASCII           +-- wrapping, indent,
             +-- entity decoding                     styles, hit rects
```

Four stages, each in its own file, none of which include anything
from Zeitlos except `zgfx.h` and `zfont.h`. That is what allows all
four to be compiled and tested on the build machine
(`sw/apps/web/tests/`).

This used to claim that was where every bug had been found. It is no
longer true, and the exception is instructive: the parsing and layout
bugs were all caught on the build machine, but the **image** work
produced a run of bugs that only hardware showed -- a blit stride in
the wrong units, a spool handle closed out from under the page, a
decode box constrained by markup that did not match its own file.
Each of those lives at a seam between this pipeline and something
else, which is exactly where host tests stop reaching.

---

## Folding to ASCII is not optional

Every font in this tree covers `0x20` to `0x7f` and nothing else --
`sw/common/zfont_data.c` declares each `z_font_t` with exactly that
range. `z_fb_draw_char()` silently draws nothing outside it.

So a page full of typographic quotes does not render as a page of
wrong characters, it renders as a page of holes: text that is
present, correct, and invisible. Since essentially every page worth
reading is UTF-8, and English Wikipedia in particular is full of en
dashes, curly apostrophes and non-breaking spaces, folding is what
makes the text legible at all.

`uni.c` maps one codepoint to zero to three ASCII bytes:

| Input | Output | |
|---|---|---|
| `—` U+2014 | `--` | em dash, as `sw/apps/read` already shows it |
| `–` U+2013 | `-` | |
| `’` `“` `”` | `'` `"` `"` | |
| `…` U+2026 | `...` | |
| NBSP | space | so the line can wrap |
| soft hyphen | *(nothing)* | |
| `ö` `é` `ß` | `o` `e` `ss` | diacritic dropped, ligature expanded |
| anything else | `?` | |

The last row is deliberately not a silent drop. A dropped character
makes a sentence read as complete when a word is missing; a `?` says
something was here and could not be shown, which is true and which
the reader can act on.

No non-Latin transliteration. Greek, Cyrillic and Han all become `?`.
Doing better costs tens of kilobytes for text that would still be
unreadable at 5x8.

---

## What the parser does not do

- **No DOM.** An element stack, and blocks streamed out as they
  complete. Nothing can be queried or restyled after the fact,
  because nothing is retained. This is the single decision that makes
  a 350KB page fit.
- **No CSS.** Not parsed, not fetched. Source order is the layout.
  Document-shaped pages read correctly; application-shaped pages
  read as a list of their parts.
- **No JavaScript**, permanently. See
  [web_app.md](web_app.md#why-no-javascript).
- **No HTML5 tree-construction recovery.** The adoption agency
  algorithm and table foster-parenting exist so that broken markup
  produces the same *tree* everywhere. With no tree there is nothing
  to agree about: a stray `</b>` closes a `<b>` if one is open and is
  ignored otherwise.
- **No emphasis as a visual style.** One weight per font, and
  underline already means "link". `<b>`, `<i>`, `<strong>` and `<em>`
  keep their text and lose their distinction -- the same conclusion
  `md.h` reached for Markdown.

Two things it *does* do that are easy to get wrong:

- `<script>`, `<style>` and `<textarea>` are **raw text**. Without a
  raw-text tokenizer state, `if (a < b)` inside a script becomes a
  tag and the rest of the document is eaten.
- A numeric character reference in the range `0x80`-`0x9F` is remapped
  through Windows-1252, which the HTML specification requires.
  `&#151;` is a page that meant an em dash; decoding it as the C1
  control it literally names produces nothing on screen.

---

## Blocks

A block is the unit that gets its own vertical space: one paragraph,
one heading, one list item, one table row, one line of a `<pre>`.

```c
typedef struct {
    html_kind_t kind;       // PARA, HEADING, PRE, QUOTE, LIST, RULE,
                            // TABLE, IMAGE
    uint8_t     level;      // heading level, list depth, quote depth
    char        marker[8];  // list bullet or number, already resolved
    char        text[HTML_LINE_MAX];
    uint16_t    len;
    uint16_t    img_w, img_h;  // IMAGE: declared size, 0 if not given
    bool        truncated;  // hit HTML_LINE_MAX and lost its tail
    bool        tight;      // continues the previous block (a <br>)
    html_span_t spans[HTML_MAX_SPANS];
    uint8_t     nspans, nlinks;
    char        links[HTML_MAX_LINKS][HTML_LINK_MAX];
} html_line_t;
```

An `HTML_IMAGE` block carries its caption in `text` and its `src` as
link 0, so the renderer can put a hit rectangle over the box and the
browser can resolve it exactly as it does an anchor.

The list marker is resolved by the **parser**, not the renderer,
because only the parser knows the ordered-list counter.

`tight` exists because `<br>` is still how most of the web writes a
line break. Without it every `<br>` in an address block or a poem
opens a full paragraph gap.

---

## Checkpoints, and the one contract that matters

A document too large to hold in memory is rendered the way
`sw/apps/read` renders Markdown: keep a sparse index of
`(byte offset, parser state)` pairs, and reproduce any screen by
seeking to the nearest one and replaying forward. That works only
because `html_state_t` is a flat POD -- an element stack and a few
counters, no pointers.

`html_mark_offset()` and `html_mark_state()` report the most recent
legal resume point. The contract is not the obvious one:

> **The mark visible during an emit callback is the resume point for
> that same block, not for the block after it -- and several
> consecutive blocks can share one mark, in which case it resumes at
> the first of them.**

Both halves matter. Blocks are not always ended by a tag: a newline
inside `<pre>` ends one and so does `<br>`, and neither produces a
new mark. So an index records each **distinct** mark the first time
it is seen, together with how many blocks were emitted before the
block that first carried it:

```c
if (html_mark_offset(ctx) != last_recorded) {
    record(html_mark_offset(ctx), html_mark_state(ctx), nblocks);
    last_recorded = html_mark_offset(ctx);
}
nblocks++;
```

Getting this wrong costs one duplicated or one missing block at every
checkpoint. On screen that looks like a scrollbar that does not quite
line up, not like a parser bug, which is why it is checked directly
rather than left to be noticed.

### Where a mark is legal

A mark can only be taken where **nothing about the block under
construction lives in the context rather than in `html_state_t`** --
no pending text, no classified block kind, no list marker, no open
anchor, no open span. Anywhere else, a resume silently disagrees with
the original parse.

Two real bugs of exactly that shape were caught by the resume
invariant and by nothing else:

- A mark taken between `<h1>` and its text lost the heading level,
  because it lived in the context. The heading came back as a
  paragraph.
- A mark taken after `<li>` lost the resolved list marker. The item
  came back with no bullet.

In both cases a parse from the start was correct every time, and
every unit case passed.

---

## Layout

Wrapping is greedy, recomputed on every draw. There is no
width-keyed cache, because a cache would need invalidating on every
window resize and the work is a scan of one block.

A **position** in a document is `(block, display line within block)`,
the same two-part shape `read` uses. Scrolling by whole blocks jumps
a paragraph at a time and is unusable; the sub-line offset makes
scrolling smooth without making the index depend on the window width.

`<pre>` is clipped rather than wrapped -- that is what makes it
preformatted.

### What a 1bpp display can carry

Two inline styles, because that is how many are distinguishable
without a second font weight: **inverse** for `<code>`, **underline**
for links.

For headings, two signals, spent separately:

- the larger font (6x12 rather than 5x8) says *this is a heading*
- a rule underneath says *this is a top level one* (h1, h2)

The first version gave the larger font only to h1 and h2. Rendered
off-device, that turned out to make `<h3>` pixel-for-pixel identical
to the paragraph above it -- and `<h3>` is the level Wikipedia
subsections actually use, so the effect was a page with no visible
structure below the first two headings. Not something an assertion
would have caught.

### Screen coordinates, once, at the top

`layout_cfg_t.x` is a **screen** x-origin, not a content-relative
one, because `z_fb_hw_box()` and `z_fb_hw_line()` take screen
coordinates while everything else an app draws with is
content-relative. That mismatch is what `sw/common/tests/zrender.h`
records as the cause of two of `sw/apps/logic`'s three layout
failures. Resolving it once, in the configuration struct, means it
cannot be got wrong per call.

---

## Testing

```
cd sw/apps/web
make test                                  assertions
make test PAGES="$(ls ~/pages/*.html)"     invariants on real pages
make render DOC=page.html                  writes /tmp/web.pbm
```

Three invariants in `tests/test_html.c` do the real work:

1. **Chunk independence.** Feeding in chunks of 1, 7, 512 and
   all-at-once must produce identical output. 512 is what the real
   transport delivers (`ZSTREAM_CHUNK_SIZE_DEFAULT`), so a bug here
   is a bug on hardware only.
2. **Resume.** Parsing from any mark must reproduce exactly the tail
   of the full parse.
3. **Bounds and termination.** Every block NUL-terminated within
   `HTML_LINE_MAX`, every span inside its block's text, every span's
   link index inside its block's link table.

And in `tests/test_layout.c`:

- `layout_count()` must equal what `layout_draw()` actually draws, at
  every width, including when drawn one line at a time -- which is
  what a partially-scrolled first block on screen does. If those
  disagree, scrolling down N lines and back up N lands somewhere
  else.
- A supplied image bitmap must **replace** its placeholder box rather
  than being drawn inside or over it. Drawing both is the easy
  mistake, and a leftover frame around a loaded picture reads as a
  rendering fault.
- The image blit is checked with a **striped** source, because the
  stride is in bytes and passing words scrambles every row by a
  factor of four -- which does not look like a stride bug, it looks
  like broken dithering. A solid bitmap cannot catch it: every row
  looks the same however they are indexed, which is why the first
  version of these tests passed while the picture on screen was
  unrecognisable. The check was verified by reintroducing the bug and
  confirming it fails.

`make render` is not a nicety. Whether a wrapped list item lines up
under its own text or under its bullet, whether a link underline
collides with the line below, whether an infobox is readable or a
column of pipes -- none of those are things anyone writes an
assertion for in advance, and all of them are obvious in one look.

---

## Images

`<img>` becomes an `HTML_IMAGE` block, drawn as an empty box with a
caption inside. **Nothing is fetched.**

### What the box says

| | |
|---|---|
| caption | the `alt` text, or the filename from `src` when there is none |
| size | the `width`/`height` attributes, when the markup gives them |
| empty `alt` | nothing at all — the image is decorative and says so |

A filename rather than the word "image", because it tells a reader
more about what is missing and is what they would need in order to go
and look at it.

The declared size is honoured because it is most of the point: a page
of thumbnails and a page of banners should not look the same, and a
reader should be able to tell a decorative flourish from the diagram
the text refers to. **Most of the modern web sizes images in CSS
instead**, so an absent size is the common case and the renderer
picks 160x90.

Three clamps, each for a different reason:

- **Wider than the content** is scaled down keeping the aspect ratio.
  A squashed box would misrepresent what is missing.
- **Smaller than three characters wide or two lines tall** is grown,
  so a tracking pixel that survived the empty-`alt` check is still
  visible as something.
- **Taller than 24 lines** is capped, so one `height="4000"` cannot
  push the rest of the page off the bottom. 24 rather than 12,
  because at an 8-pixel font 12 lines is 96 pixels and an ordinary
  120-pixel diagram was being squashed.

### A block of its own

An image is its own block, not inline text. The layout engine draws a
box, and a box cannot sit inside a wrapped line.

The cost is that an image in the middle of a paragraph splits it into
two — which reads acceptably and is far simpler than inline boxes
would be. `tests/test_html.c` pins that behaviour so it is a decision
rather than a surprise.

### Clicking loads the picture, in place

The `src` rides along as link 0 of the block, so `layout.c` puts a hit
rectangle over the whole box and the browser resolves it against the
page URL exactly as it does an anchor. Clicking **loads the image
where the box is** rather than navigating away from the page it
illustrates.

The box shows `Loading image...` first, set before anything blocks, so
it says what is happening rather than sitting inert through a lookup
and a fetch.

**One image is held at a time.** A decoded bitmap is the size of the
box it fills and there is no dynamic memory, so a page of twenty
thumbnails would need a budget nobody can state in advance. One is
always affordable and is what a reader actually looks at; clicking
another replaces it.

The image spools to its **own file**, not the page's. `page.c` fetches
blocks from the spool on demand rather than holding the document in
memory, so writing an image over it would destroy the page the image
belongs to.

The fetch itself reuses `start_fetch()` rather than running a second
path beside it. DNS, TLS, redirects, keep-alive and the receive loop
are all the same; only where the bytes land and what happens when they
stop differ. It therefore inherits connection reuse, so clicking an
image on the page you just loaded usually costs no handshake at all.

**The blit stride is in BYTES**, not words -- `o.wpl * 4`, the same as
`sw/apps/view` passes. Getting this wrong scrambles every row by a
factor of four, and it does not look like a stride bug: it looks like
the dither is broken, which is how it was reported.

`tests/test_layout.c` now blits a STRIPED source for exactly this. A
solid bitmap cannot catch it -- every row looks the same however they
are indexed -- which is why the first version of these tests passed
while the picture on screen was unrecognisable. The test was checked
by reintroducing the bug and confirming it fails.

**Clipping.** `z_fb_hw_blit_mem()` clips to the SCREEN and nothing
else (`zgfx.h`), so `layout.c` narrows the rectangle itself against
the caller's clip. Without that, an image scrolled up paints over the
toolbar and outside the window.

**Sizing.** The decoders scale at decode time and there is no
intermediate greyscale to re-scale from (see `zimg.h`), so the box
size has to be known BEFORE decoding. `layout_image_box()` is exposed
for exactly that, rather than the browser deriving the same number
independently -- which is how a picture ends up a pixel wider than the
frame around it.

`zimg` downscales by powers of two only, so an image is drawn at the
largest 1/2^n that fits. A box declared 320x120 may hold a 150-pixel
picture with the remainder left blank. Blank rather than stretched: a
dithered bitmap does not survive resampling.

### The decode box is not the placeholder box

The decoder is given the whole area an image could occupy, **never
the size the markup declared**, and the drawn box then follows
whatever came back.

Honouring the declared size looked more respectful of the page and was
wrong, because `zimg` scales by **powers of two only**. Real markup
does not match its files: en.wikipedia.org serves a 120x179 thumbnail
in a tag that says `width="115" height="171"`. Five pixels too wide to
fit, no 115/120 scale available, so the decoder dropped to 1/2 and
produced 60x89 -- a quarter of the area, for a five-pixel discrepancy
nobody could see.

The declared size still sizes the PLACEHOLDER, which is what it is
good for: it says how much room to leave before anything is loaded.

### The box follows the picture once it loads

A placeholder is sized from the markup's `width`/`height`, but `zimg`
scales by powers of two only -- so a 250x224 picture in a 250-wide box
comes back **125x112** and leaves half the box empty. That reads as a
hundred-odd pixels of nothing between an image and the paragraph below
it.

Once an image is loaded the box is sized to what was actually decoded.
It is matched on the **src** rather than a block number, so
`layout_count()` and `layout_draw()` agree without either needing to
know which block it is looking at -- only one image is loaded at a
time, so one src is enough.

### Scrolling past an image

A single up/down step onto or off an image moves the WHOLE block.

Stepping through a picture a text line at a time is wrong for the same
reason stepping through a paragraph a pixel at a time would be: the
line is not the unit the content is made of.

### Formats

Whatever `sw/common/zimg.c` decodes: **BMP, PNM, GIF, JPEG and PNG**
(see docs/png.md), plus **SVG** through `sw/common/zsvg.c` (see
docs/svg.md), which is rendered rather than decoded.

SVG matters most of the four on this display. A photograph dithered to
one bit is mush; a diagram is nearly lossless, and site logos and
Wikipedia's diagrams are SVG.

The format is sniffed from CONTENT, not the filename: a `.jpg` that is
really a PNG is common on real sites.

On a 1bpp screen a photograph is mostly noise, so line art and
diagrams are what this is really for -- which is the argument for SVG
over any of the raster formats.
