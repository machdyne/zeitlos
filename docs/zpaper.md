# zpaper: pages for printing

`sw/common/zpaper.[ch]`. How an app hands pages to the print service
([print](print.md)): described, not drawn.

## Why a description

An app that prints draws its pages the way it draws its window,
through [zcanvas](zcanvas.md), onto a recording target. Nothing is
rasterized in the app. Each fill, stroke, picture and run of text is
written to a job file as it is drawn, in the page's own units. The
print service then turns that into whatever the printer wants -- a
300 dpi raster for an inkjet, PostScript for a laser, a PDF for a file
-- and lays the pages on sheets, because only it knows the paper and
the printer's margins.

So:
- an app pays a few KB for printing (the writer);
- a job is small: vectors, text as text, pictures by their path. The
  seven-card slide tour is 21.7KB;
- every app prints through one renderer, the screen's own, so what is
  on paper is what was on the screen.

## Writing a job

```c
#include "../../common/zpaper.h"

static bool to_file(void *ctx, const void *d, uint32_t n);   // fs_write_chunk ...

zp_writer_t w;
zp_writer_init(&w, to_file, &fh);
zp_job(&w, "title", "My deck");
zp_job(&w, "nup", "4");
for (each page) {
    zc_xf_t xf;
    zc_target_t *t = zp_page_begin(&w, &page_shape, &xf);   // a zpage_t
    draw_my_page(t, &xf);              // the same code as on screen
    zp_page_end(&w);
}
if (w.failed) ...                      // a write failed
```

A picture from a file goes by its path, and the print service decodes
it at the size it prints:

```c
if (t->fmt == ZC_REC) zp_image_file(t, "/media/images/squirrel.jpg", contrast, bright, x, y, w, h);
else ...                                // the app's own decoded copy
```

`sw/apps/slide/printjob.c` is the whole of slide's side: about
twenty lines.

Then [zprint](print.md#printing-from-an-app) starts the print service
on the file.

## The file

```
"ZPP1", u32 version (1)
records: u8 op, u8 0, u16 0, u32 length, then the bytes, padded to 4
```

Little-endian throughout. A reader skips what it does not know.

| Record | Holds |
|---|---|
| `JOB` | one option, `key=value` ([print](print.md#job-options)) |
| `PAGE` | the page's grid (page units), paper or ratio, orientation, size in tenths of a mm, zpage name |
| `END` | the end of a page |
| `CLIP` | the clip for what follows |
| `RECT` | a device rectangle and its paint |
| `FILL` | a path (subpaths, points), nonzero or even-odd, its paint, its box |
| `STROKE` | a path, a round pen's width, closed or not, its paint, its box |
| `IMAGE` | pixels: grey, 1bpp, or a 1bpp mask with its paint |
| `IMAGE_FILE` | a picture's path, contrast, brightness and rectangle |
| `TEXT` | a run: font name, size, pen, tracking, bold and italic, its transform, position, paint, box, and the UTF-8 |

**Units.** Drawing records are in device units: page units times
`ZP_SUB` (16), 24.8 fixed point. Curves are flattened at that
resolution while recording, which is finer than any printer gets them
(a slide unit is 6.4 dots across Letter at 300 dpi).

**Paint** is zcanvas's: a shade (tone 0..16) or an 8x8 pattern with
its tone and whether it is opaque. One pattern bit is one page unit,
anchored at the page's corner, as on the screen.

**Text stays text.** A run keeps its font's name and style, not its
glyphs. The raster player draws it with the same font file; the PDF
and PostScript writers make a font of the glyphs used.

## Reading a job

| Call | Does |
|---|---|
| `zp_open()`, `zp_next()` | walk the records; a record that would run past the end stops the walk and sets `bad` |
| `zp_job_get()` | an option's value |
| `zp_pages()` | where each page starts and how long it is |
| `zp_page_info()`, `zp_page_zpage()` | a page's shape, as recorded and as a `zpage_t` |
| `zp_decode()` | one drawing record, checked: counts, lengths and indices inside the record |
| `zp_play_page()` | draws a page onto any zcanvas target, through a transform |

**Playing a page.** `zp_play_page(pl, page, len, target, xf)` draws a
page through `xf` (page units to device pixels) onto a target -- in
the print service, one band of a sheet at a time:
- what lies outside the band is skipped by its box;
- fonts and pictures come from the caller's loaders;
- a pattern turns with the page when the page is laid a quarter turn;
- what cannot be drawn (a missing font, a picture that will not
  decode) is counted in `skipped`, and the rest of the page still
  prints.

## As SVG

`sw/common/zpsvg.h`: a page of a job as an SVG document, for a drawing
program or a web page -- what slide's `/export svg` writes
([slide_app.md](slide_app.md#diagrams)).

```c
zp_svg_t s = { put, load, release, font, ctx };
zp_svg_page(&s, job, len, 0);		// page 1, written through put()
```

| Record | becomes |
|---|---|
| `CLIP` | a `<clipPath>` and a group |
| `RECT`, `FILL` | a `<rect>`, a `<path>` (even-odd kept) |
| `STROKE` | a `<path>` with a round pen of its width |
| shades / patterns | greys / an SVG `<pattern>` of the 8x8 bits, one bit a page unit |
| `TEXT` | `<text>` in the nearest generic family (sans-serif, serif, cursive, fantasy, monospace), bold and italic kept; with `font`, its width as `textLength`, so a reader's font fits where the run was |
| `IMAGE_FILE` | an `<image>`, its file embedded (JPEG, PNG, GIF, BMP) when `load` gives it, else linked; grey, as it prints |
| `IMAGE` | an `<image>`: the pixels as an 8-bit grey BMP |

The document is in page units (`viewBox="0 0 512 342"` for a classic
card), its size in millimetres for a paper page. It is written a
string at a time, so a card's SVG needs no memory of its own. A mask
image (rare: icons) is not said; it is counted in `skipped`.

## Testing

`make -C sw/common/tests -f Makefile.zcanvas paper` (`test_zpaper`,
ASan and UBSan):
- a page of every kind of record, recorded, read back and decoded:
  options, page shapes, a text run (and one of three letters, shorter
  than the padding: such runs were once dropped), a picture by path;
- **played back at 300 dpi in bands is drawn directly at 300 dpi**:
  151 of 1.2 million inked pixels differ (0.012%), all on the edge of
  an ellipse flattened at 16 units a page unit rather than at device
  resolution;
- **turned a quarter is the same page turned**: played onto a
  sideways cell and turned back, 1 pixel of 195,000 differs, so
  patterns, pictures and bitmap text turn exactly with the page;
- a newer version refused; the job cut short at every length, and
  3000 copies with two bytes scribbled on, read without a fault.

`look-paper` writes the pages to `/tmp/zcanvas`.

SVG: `make -C sw/apps/slide test` (`tests/test_svg.c`) writes a
flowchart card and every card of the tour through `zp_svg_page()`,
checks that each document is well-formed (tags nest and close, text
escaped, runs with their widths) and leaves them in `/tmp/slide/svg`.
