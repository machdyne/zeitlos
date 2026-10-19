# zcanvas: drawing pages

`sw/common/zcanvas.[ch]` is a 2D rasterizer for pages: fills, thick
strokes, frames, patterns, shades and scaled images, into a 1bpp bitmap
in the framebuffer's packing or an 8-bit grey band for a printer.
`sw/common/zcfont.[ch]` draws text with it ([fonts](fonts.md)), and
`sw/common/zpage.[ch]` says what shape a page is and where it goes on
paper. They were written for `slide`; any app may link them.

## Why it exists

`slide` draws a card at 100% in its window, at 200% when zoomed, at
640x480 in a show and at 300 dpi on paper. Every one of those has to
come from the same description of the card, never from a scaled copy
of its pixels: a doubled 1-bit image is blocky on screen and worse on
paper.

That needs a rasterizer that takes the description at any scale.
`zsvg` ([svg](svg.md)) already has the right shape: it flattens to
edges and emits spans. But it works in floats, which are software
floating point on this CPU, it draws 1-pixel strokes only, and it
stops at the screen. zcanvas is the same idea in fixed point, plus
the things a page needs.

## The model

**Colour is light, not ink.** A 1 bit is white and a 0 bit black; a
grey byte of 255 is white. This is how `zimg` already decodes images
(bright pixels set), so a photo on a card is not a negative, and it is
what paper is. A slide card on the dark desktop is therefore a white
page with black ink, as HyperCard's were.

**Tone is darkness, from 0 (white) to 16 (black).** These are the 17
levels of `zgfx.h`'s shaded fills, with the blitter's own 4x4 Bayer
matrix, so a shade drawn here looks like one the hardware draws.
- On a grey target a tone is a real grey, and the printer halftones
  it.
- Patterns are the 8x8 tiles of `zwidget.h`'s table. Their 1 bits are
  ink in a tone; their 0 bits are white (opaque) or left alone.

**Coordinates are 24.8 fixed point** (`zc_fix`; `ZC_I(n)` is n).
- Paths are built in **user** units through an affine transform
  (`zc_xf_t`, 16.16) into **device** pixels.
- A target is a window onto device space: a buffer with an origin. A
  band of a printed page is just a target whose origin is the band's
  first row, so the same calls draw any band of any page.
- `zc_xf_rot90()` lays a landscape card on portrait paper.

**Coverage is by pixel centre, with no antialiasing.** A pixel is
inside a shape when its centre is. Two shapes that share an edge
therefore never both paint a pixel and never both miss one. That is
also what keeps a page rendered in bands identical, bit for bit, to
the page rendered whole; the tests check this.

**Patterns are anchored to the page** (`px`, `py`), not to the buffer.
A pattern does not shift between bands or when the card is drawn
elsewhere.
- `pat_scale` is device pixels per pattern bit.
- On a printed page it is the print scale, so patterns keep their
  on-screen size on paper instead of becoming a grey fuzz at 300 dpi.

**No dynamic memory.**
- Fills need scratch for their edge table, given once with
  `zc_scratch()` (20 bytes an edge).
- A path's storage is the caller's.
- `zcfont` keeps about 15KB of working paths in `.bss`.

## Primitives

| Call | Does |
|---|---|
| `zc_path_move/line/cubic/quad/arc` | build a path (user units, through the path's transform) |
| `zc_path_rect/ellipse/rrect` | closed shapes, each its own subpath, all clockwise |
| `zc_fill(t, path, ZC_NONZERO / ZC_EVENODD, paint)` | fill every subpath |
| `zc_stroke(t, path, width, closed, work, paint)` | a round pen swept along the path; under 1.25 device pixels, 1-pixel lines |
| `zc_frame_rect/ellipse/rrect` | outlines **inside** the box (below) |
| `zc_blit_bits` | a 1bpp image, scaled nearest-pixel, opaque |
| `zc_blit_mask` | the same as a mask: black source pixels paint, white ones leave the target alone |
| `zc_blit_gray` | an 8-bit image: resampled onto grey targets, shade-dithered onto 1bpp |
| `zc_dither` | grey to 1bpp at a given size: resample once, Floyd-Steinberg once |
| `zc_resample` | grey to grey (box when reducing, bilinear when enlarging) |
| `zc_span`, `zc_fill_rect_dev`, `zc_line_dev` | device-space primitives for callers with their own geometry |

**Strokes vs frames.**
- A stroke is a disc swept along the path. Each segment becomes a
  capsule, and all capsules turn the same way, so a nonzero fill of
  them is their union: round caps and round joins, at any width,
  through the same rasterizer as everything else. That suits lines,
  arrows, polygons and glyphs.
- A frame is the outline of a rectangle, ellipse or rounded rectangle,
  drawn as an even-odd fill of the shape and the shape shrunk by the
  width. It lies inside the box the fill covers, as QuickDraw's pen
  did.
  - A 1-unit frame on whole coordinates lands on whole pixels.
  - A rectangle keeps square corners.
  - It costs two shapes' worth of edges rather than a capsule per
    segment.

**Curves** flatten in device space. The number of steps follows the
device size, about one per eighth of a square root of pixels, so the
error stays under a tenth of a pixel from a dot to a full page.

## Images

`zimg` decodes and dithers in one pass into a fixed bitmap. That is
right for `view`, but a card shows an image at its object's size,
which changes when the object is resized, zoomed or printed.
`z_img_decode_gray()` (in `zimg.h`) decodes to grey rows instead, and
the caller keeps them:
- On screen, `zc_dither()` scales the grey image to the object's size
  and dithers it once. A resized image is re-dithered from the grey,
  not rescaled from dots.
- On paper, the grey goes to the printer through `zc_blit_gray()`.
- Turned, through any transform, `zc_blit_xf()` draws a grey, 1bpp
  or mask source: each device pixel's centre is taken back into the
  source (bilinear for grey, nearest for bits). The print service
  uses it for a card laid a quarter turn on a sheet, and zcfont for a
  bitmap glyph on such a card. An unturned transform is the ordinary
  blit.

The grey image is at most `Z_IMG_MAX_W` x `Z_IMG_MAX_H` (640x480, a
power-of-two reduction of the source), the same bound as the decoders
themselves.

## Pages

`zpage.h` describes what shape a card is.
- **A shape** is a paper size or a screen ratio, plus an orientation:

  | Kind | Sizes |
  |---|---|
  | paper | Letter, Legal, A4, A5, A6, 4x6, 5x7, 3x5 (index card) |
  | ratio | classic (HyperCard's 512x342), 4:3, 16:9, 16:10, square |
  | custom | any size in mm, or any ratio |

- **The design grid** is the card's coordinate system at 100%. A
  landscape card is 512 across and a portrait one 440 down, so it fits
  a window with room beside it:

  | page | grid |
  |---|---|
  | classic | 512x342 |
  | 4x6 | 512x341 |
  | 3x5 | 512x307 |
  | Letter landscape | 512x396 |
  | A4 landscape | 512x362 |
  | 4:3 | 512x384 |
  | 16:9 | 512x288 |
  | Letter portrait | 340x440 |
  | A4 portrait | 311x440 |

- **Placing on a sheet.** `zpage_place()` puts a card on a sheet
  (`zpage_sheet()`: a paper, a resolution, margins).
  - With **fit**, the card is as large as the printable area allows.
  - With **actual size**, it is the card's own paper size, centred.
    A card the size of its sheet covers it edge to edge.
  - A card is turned a quarter when that makes it larger, for example
    a landscape slide on portrait Letter.
  - Several to a sheet go in the grid of columns and rows that makes
    them largest, with a gutter, and stay upright so handouts read as
    a stack. Four 4x6 cards fitted on Letter, six 16:9 slides on A4.
- **Cut marks.** `zpage_cut_marks()` draws them in the gutters and
  never across a card.

## Recording: `ZC_REC`

`zc_target_rec(t, ops, ctx, w, h)` makes a target that draws nothing:
every call is described to a recorder (`zc_rec_ops_t`) instead, after
the transform and with the clip as it stands.

| Call | is described as |
|---|---|
| `zc_fill()`, and the frames (`zc_frame_*()`, which fill) | `fill(path, rule, paint)` |
| `zc_stroke()`, `zc_line_dev()` | `stroke(path, width, closed, paint)`, width in device units (0: as thin as the device goes) |
| `zc_span()`, `zc_fill_rect_dev()`, `zc_clear()` | `rect()` |
| `zc_blit_bits()`, `_mask()`, `_gray()` | `image(kind, source, rect)` |
| `zc_text_draw()` | `text(style, transform, x, y, string)`; its underline follows as a stroke |

So an app's drawing code is its printing code: given a recording
target, the same calls that draw a card in its window describe it as
a page. [zpaper](zpaper.md) is the recorder, and the print service
plays the description back at the printer's resolution.

## Speed

`sw/common/tests/bench_zcanvas` is a 512x342 card: a title, five
wrapped lines of body text, a caption, a shaded rounded rectangle, an
ellipse frame and a line. It runs under `sim/zsim-headless`, which
counts retired instructions:

```
make -C sw/common/tests/bench_zcanvas && sim/zsim-headless -n 0 sw/common/tests/bench_zcanvas/bench_zcanvas.bin
```

| | instructions |
|---|---|
| card, no glyph cache | 2.43M |
| card, filling the cache | 1.59M |
| card, from the cache | 0.70M |
| the card at 4x (2048x1368 grey) in 22 bands of 64 rows | 19.2M |

A card from the cache is therefore a few tens of milliseconds at
48MHz, depending on the core's cycles per instruction and the memory.
On hardware it is measured with the cycle counter in the final test
phase.

A printed sheet is much more: a Letter sheet at 300 dpi is 8.4
million grey pixels. [print](print.md#speed) has its measurements.

What made it this fast:
- slopes computed in 32 bits;
- edges sorted by index, so the edge records never move;
- pattern and shade masks built a word at a time;
- an aligned blit loop;
- font ratios computed once per font and size;
- ASCII glyphs found without a search.

## Testing

```
make -C sw/common/tests -f Makefile.zcanvas test        # the checks
make -C sw/common/tests -f Makefile.zcanvas look        # + images in /tmp/zcanvas
make -C sw/common/tests -f Makefile.zcanvas look-fonts
make -C sw/common/tests -f Makefile.zcanvas look-page
```

All three suites build with ASan and UBSan.

- **`test_zcanvas`** (36 checks) covers:
  - exact rectangle coverage, and abutting shapes that neither
    overlap nor gap;
  - ellipse area; even-odd versus nonzero;
  - stroke width, joins and caps; hairlines;
  - frames inside their box;
  - no pixel outside the clip;
  - bands equal to the whole page, both grey and 1bpp;
  - transforms;
  - pattern anchoring, and the word-at-a-time masks against the
    per-pixel rule at three cell sizes;
  - shade density;
  - blits: 1:1 at odd offsets, hanging off the edge, as a mask,
    doubled;
  - grey resampling and dithering.
- **`test_zcfont`** is listed in [fonts](fonts.md).
- **`test_zpage`** (40 checks) covers:
  - every grid;
  - placement: turned, actual size, edge to edge, n-up, handouts;
  - cut marks;
  - grey decoding of the demo PGM (the file's own samples) and JPEG.

The `look` targets write PGM images to look at before anything
reaches a screen, as `zrender.h` does for panels.
