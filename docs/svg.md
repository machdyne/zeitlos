# SVG

`sw/common/zsvg.c` — vector rendering for a 1bpp display.

## Why it is worth more here than raster is

A photograph dithered to one bit is mush. A **diagram** is nearly
lossless: line art, logos, charts and maps were already shape-and-edge
rather than tone, so they survive the trip to monochrome almost
perfectly.

SVG is what the web uses for exactly that material — Wikipedia's
diagrams, most site logos — and it arrives as a few kilobytes of text
rather than a few hundred of pixels. It also **scales**: a 24-pixel
logo and a full-screen diagram come from the same file, which matters
when the display is 640x480 and the source was drawn for something
else.

## How it draws

Filled shapes go through a scanline rasterizer: geometry is flattened
to edges, edges are sorted per raster line, and each line is filled as
a set of horizontal spans.

That maps directly onto `zgfx.h`'s
`z_fb_hw_span_begin()`/`z_fb_hw_span()` — a **hardware span filler**
whose own header describes it as *"the inner loop of something that
has already clipped"*. This is that something. Strokes are line
segments and go to `z_fb_hw_line()`.

**The renderer never touches the framebuffer.** It calls back with
spans and lines, so the same code serves `view`, `web`, an off-device
test that writes a bitmap, and a future drawing app that wants the
geometry rather than the pixels.

The edge array is **caller-owned**, like `zinflate`'s window. Its size
is a policy question — how complex a drawing is worth rendering — and
that belongs to the app.

## What it supports

| | |
|---|---|
| elements | `path`, `rect` (incl. rounded), `circle`, `ellipse`, `line`, `polyline`, `polygon`, `g` |
| path data | all of `MmLlHhVvCcSsQqTtAaZz`, including elliptical arcs |
| transforms | `translate`, `scale`, `rotate` (incl. about a point), `matrix`, `skewX`, `skewY`, nested |
| fill rules | nonzero and even-odd |
| fitting | `viewBox` fitted to the target box, aspect preserved, centred |

`fill="none"` is honoured. Any other colour paints, because a display
with one ink cannot make the distinction.

## Paint comes from `style=`, not just `fill=`

**Inkscape, and most real-world SVG, writes every paint into a
`style` attribute and none into presentation attributes.** A renderer
reading only `fill="..."` sees a document with no colours at all and
paints everything solid — which is exactly how an illustration came
out as one black silhouette.

`style` is read first and the presentation attribute second, which is
the CSS cascade's order and the one that matters when a file has both.
It is not a CSS parser: a semicolon-separated list of `name:value`,
which is all SVG presentation style is in practice.

## Colour becomes a shading level

A fill's luminance (Rec.601, as `zimg.c` uses) selects one of
`Z_SVG_LEVELS` levels, passed to the span callback. `zgfx.h`'s
`z_fb_hw_span_begin()` takes exactly such a level, so a shaded span
costs no more than a solid one.

Without it every fill is ink, and a coloured illustration — as opposed
to line art — is a silhouette.

`fill:none` and anything resolving to white are not drawn at all: on
paper white paint covers what is beneath it, and here there is nothing
to cover with. An unrecognised colour is solid, because an unknown
colour drawn as ink is visible and drawn as white is missing.

The bitmap sink uses an **ordered (Bayer) dither**, so a shaded region
has a stable pattern that does not crawl when redrawn or scrolled.
Error diffusion would look better on a photograph and is wrong here:
shapes are painted independently, so error from one would leak into
the next and a flat region would come out streaked.

## Sniffing needs more than a magic number

`<svg` is searched for in the first kilobyte, not expected at offset
zero. An XML declaration, a DOCTYPE and a generator comment all come
first — Inkscape puts the tag at **offset 114** in an ordinary file.

The callers size their sniff buffer accordingly. A 16-byte buffer,
which is all a raster magic number needs, reports every real SVG as
unrecognised.

## What it does not

No CSS, scripting, animation, filters, gradients, or **text**.

Text is the significant omission and it is deliberate: laying out
`<text>` needs font metrics, and every substitute produces a diagram
whose labels are *wrong* rather than missing. An unknown element is
skipped rather than fatal — a missing label beats losing the diagram
it labels.

Stroke **width** is ignored; every stroke is one pixel. On monochrome
a two-pixel line is not twice as visible, it is twice as heavy, and
diagrams read better thin.

`preserveAspectRatio` is always treated as the default `xMidYMid
meet`. A wrongly stretched diagram is worse than a correctly
letterboxed one.

## No libm

`zsvg.c` implements `sqrt`, `sin`, `cos`, `tan`, `atan2`, `floor`,
`ceil` and `fabs` itself.

It is shared code in `sw/common`, meant for `view`, `web` and a
drawing app later, and **a library that only links when every caller
remembers `-lm` breaks the next time someone uses it**.
`sw/apps/gpu3d/stl.c` already sets that precedent.

The omission also hid in a way worth knowing about: **the host
compiler folds these into builtins**, so the file compiled and linked
cleanly on the build machine and failed only on the target, where they
become calls into a library nothing was linking. `tests/test_svg.c`
now checks the substitutes against libm so they stay honest.

Accuracy, measured: sin and cos to 2e-7, sqrt to 1e-7 relative, atan2
to 2e-6. A tenth of a pixel at a 300-pixel radius is 3e-4 radians, so
these clear the bar by three orders of magnitude.

The sin series folds its argument into ±π/2 before evaluating. Without
that fold the error at the ends of ±π is 7e-3 — a hundredth of a
radian, which at a 300-pixel radius is **three pixels of arc, and
visible**. `sin(π − x) == sin(x)` makes the fold free.

## Details that are easy to get wrong

**Half-open scanline intervals.** An edge is counted when
`y0 <= sy < y1`. Closing that interval counts a vertex shared by two
edges twice, which is the classic cause of single-pixel holes at shape
joins.

**`S` and `T` after a non-curve.** The reflected control point is the
current point, not the previous curve's — getting this wrong shows as
a kink halfway along a smooth path.

**A full-circle arc is degenerate**, because its start and end
coincide and the centre is then undefined. Circles are drawn as two
half-arcs.

**Arc radii are in user space**, so they are scaled by the transform's
magnitude rather than passed through.

## In the apps

Both render into an off-screen 1bpp bitmap via
`z_svg_render_bitmap()`, then blit -- so scrolling and redrawing cost
nothing extra, and the rasterizer runs once per load rather than once
per frame.

That helper is in `zsvg.c` rather than in each app because every
caller wants the same thing, and writing the sink three times would be
three chances to get the bit order wrong -- a mistake that looks like
a broken rasterizer rather than a packing error.

| | |
|---|---|
| `view` | `VIEW_SVG=1`, document buffer 64KB, 2048 edges |
| `web` | `WEB_SVG=1`, document buffer 32KB, 1024 edges |

Both sniff SVG **before** the raster formats, because it is text and
not one of `zimg`'s formats at all.

**Fitted to the box, and for a vector that is not a compromise**:
there is no native pixel size to lose. A logo declared 24x24 in the
markup is drawn at 24x24 and looks right, where the same logo as a PNG
would have been a blurry thumbnail. In `view` the drawing is rendered
at the document size and panned, exactly like a large bitmap.

A file larger than the buffer is refused rather than truncated: half a
drawing is not a drawing.

## Testing

    cc -std=gnu99 -I sw/common -o /tmp/t sw/common/tests/test_svg.c \
       sw/common/zsvg.c -lm && /tmp/t

35 checks. Because the renderer emits spans rather than pixels, the
test collects them into a small bitmap and asserts on what was
actually covered — the only thing that catches a rasterizer being
subtly wrong.

The viewBox is 64x64 into a 64x64 target, so document coordinates are
pixel coordinates and the assertions are exact: a rect's corners are
filled and the row above it is not.

The fill-rule test is two concentric squares wound the same way.
Nonzero fills the middle, even-odd leaves a hole; **if those two agree,
winding direction is being ignored** and both are wrong.
