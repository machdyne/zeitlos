# Fonts for pages

The desktop's text is drawn by the blitter from glyph memory: 5x8 and
6x12, fixed cells at most 8 pixels wide ([window
manager](window_manager.md), [text encoding](text_encoding.md)). That
is right for terminals and lists, but not for a slide title at 40
points, a printed page, or a card zoomed to 200%.

`sw/common/zcfont.[ch]` draws text through [zcanvas](zcanvas.md)
instead, in software. Its fonts are **all public domain**:

| Kind | Format | Source | Draws as |
|---|---|---|---|
| Hershey stroke fonts | `.zhf` | A. V. Hershey, U.S. National Bureau of Standards (1967) | strokes of a pen, at any size and weight |
| misc-fixed bitmaps | `.zfn` | X11 misc-fixed ("Public domain font. Share and enjoy.") | pixels, crisp at their own size |
| the built-in 5x8 and 6x12 | compiled in | `zfont_data.c` (misc-fixed and font8x8) | the same |

Card text never goes through glyph memory or the blitter's glyph path.
So enlarging glyph memory would not change what `slide` can draw; it
would only let `wm` keep a third UI font resident.

## The Hershey fonts

Hershey drew these as strokes for plotters and displays. Each glyph is
a few polylines, which is exactly what a 1-bit page wants.
- At 100% a regular face is drawn with a one-pixel pen and stays clean
  at every size.
- At print resolution the same strokes are swept with a pen of the
  same width in page units, so a printed glyph is the screen's glyph at
  600 dpi. What is on paper is what was on the screen.

The faces on the card are in `/sys/fonts/`:

| File | From | Face |
|---|---|---|
| `sans.zhf` | rowmans | Roman simplex: a clean single-stroke sans |
| `sansb.zhf` | rowmand | Roman duplex: the same, bold |
| `serif.zhf` | timesr | Roman complex: a serif text face |
| `serifb.zhf` | timesrb | bold serif |
| `serifi.zhf` | timesi | italic serif |
| `serifbi.zhf` | timesib | bold italic serif |
| `heavy.zhf` | rowmant | Roman triplex: a heavy serif for titles |
| `script.zhf` | scripts | script |
| `scriptb.zhf` | scriptc | script, heavier |
| `gothic.zhf` | gothiceng | English blackletter |
| `symbol.zhf` | symbolic | symbols, ASCII positions as in the source |

**What each face covers.**
- **ASCII**, from the Hershey files themselves.
- **Latin-9**, composed by `tools/gen_fonts.py`:
  - accented letters as a base glyph plus an accent drawn over it,
    following an italic's slant;
  - ligatures (Æ, Œ) as two glyphs set close;
  - ß as the face's own Greek beta with its stem cut at the baseline;
  - µ as its mu;
  - the remaining symbols (£, €, ¥, ©, ®, §, ¶, ordinals, superscripts,
    guillemets, ±, ×, ÷) drawn in the face's proportions.
- **Greek** U+0391-U+03C9, from Hershey's Greek: simplex Greek for the
  sans and script faces, complex for the serif ones.
- **Typographic characters** text pasted from elsewhere is full of:
  curly quotes, en and em dash, bullet, ellipsis, minus, trade mark,
  and the four arrows.
- Anything else uses a look-alike if `zutf8.h` knows one; failing that
  it is drawn as an empty box.

The accents and symbols added here are dedicated to the public domain
like the data they extend.

**Sizes and weight.** A Hershey font is designed in 32 units to the em
with a cap height of 21; a size S draws it at S/32. The default pen is
1.4 units, which is one pixel at the sizes where one pixel is right and
enough to close up the parallel strokes of a duplex or triplex face
when large. `zc_style_t` can set:
- `weight`: the pen, in user units;
- `bold`: a heavier pen, for a face that is not bold;
- `italic`: a fifth of a slant, for a face that is not italic;
- `track`: extra letter spacing;
- `underline`.

**The acknowledgement.** The original distribution allows any use,
provided an acknowledgement travels with the data. It credits Dr. A.
V. Hershey at the U.S. National Bureau of Standards for the fonts, and
James Hurt of Cognition, Inc. for the `.jhf` format. The data may be
converted to any format except NTIS's own. The notice is in
`sw/data/font/hershey/NOTICE` and on the card as
`/sys/fonts/hershey.txt`. The original Usenet description is beside
the notice in the tree, as `sw/data/font/hershey/hershey.txt`.

## The bitmap fonts

misc-fixed, Latin-9 plus the typographic characters above (207 glyphs
each):

| File | Cell | | File | Cell |
|---|---|---|---|---|
| `m6x13.zfn` `m6x13b` `m6x13o` | 6x13 regular, bold, oblique | | `m9x15.zfn` `m9x15b` | 9x15 |
| `m7x13.zfn` `m7x13b` `m7x13o` | 7x13 | | `m9x18.zfn` `m9x18b` | 9x18 |
| `m7x14.zfn` `m7x14b` | 7x14 | | `m10x20.zfn` | 10x20 |
| `m8x13.zfn` `m8x13b` `m8x13o` | 8x13 | | | |

**On screen** they are drawn 1:1 at their own size, which suits
captions, labels and controls. At other sizes they are enlarged pixel
for pixel, with synthetic bold (a one-pixel smear) and italic (a pixel
every four rows) if asked.

**On paper there are no printer fonts.** The printer the system is
designed for speaks raster, not PostScript, so a bitmap font prints as
its pixels enlarged. That is crisp but blocky. **Text that will be
printed belongs in a Hershey face.**

## The formats

Both formats are described in `tools/gen_fonts.py`. In short:

- **ZHF1**:
  - header: units per em, cap height, x-height, ascent, descent,
    space advance, and the family name;
  - index: one u16 codepoint and u16 offset per glyph;
  - per glyph: an advance, an ink box, and a list of int8 points from
    the glyph's origin with the baseline at 0, x = -128 marking pen-up.
- **ZFN1**: the format `tools/gen_jfont.py` already wrote for the
  Japanese font. A fixed cell, a sorted codepoint table, MSB-first
  rows. Byte 7, previously 0, is now the ascent; 0 still means the
  bottom row is the baseline.

`zc_font_open()` checks a file before using it:
- every glyph inside the data;
- codepoints ascending;
- no index past the end.

A damaged file is refused when it is opened rather than read out of
bounds when it is drawn. The tests open 400 truncated and corrupted
copies.

## Making them

```
python3 tools/gen_fonts.py                  # .zhf from sw/data/font/hershey/*.jhf
python3 tools/gen_fonts.py --sheet DIR      # and a PNG proof sheet per face
python3 tools/gen_fonts.py --bdf DIR        # .zfn from misc-fixed BDFs
```

**The two sources.**
- The `.jhf` sources and the notice are in the tree.
- The misc-fixed sources are not: they are the PCF files of Debian's
  `xfonts-base`, turned into BDF with `pcf2bdf`. The script's header
  has the loop.

The generated files are committed. The release copies every `.zhf` and
`.zfn` to `/sys/fonts/`, along with the notice (`FONT_FILES` in
`release/lib/mkfatimg.py`). They belong to no app, so they are the
system's ([layout](layout.md), rule 3).

## Drawing text

```c
zc_font_t serif;
zc_font_open(&serif, file_image, file_len, NULL);    // the file stays loaded
zc_style_t st = { &serif, ZC_I(24), 0, 0, 0, 0, 0 };
zc_text_draw(&target, &xf, &st, x, baseline, "Grüße", 7, &black);
```

| Call | Does |
|---|---|
| `zc_text_metrics()` | ascent, descent, and baseline-to-baseline distance, in user units |
| `zc_text_width()` | the width of a string |
| `zc_text_advance()` | the advance of one character |
| `zc_text_wrap()` | greedy word wrap into `zc_line_t` lines, breaking a word too long for a line between characters; lines exclude trailing spaces |

Every glyph's origin sits on a whole device pixel, so text does not
shimmer as it moves, and a cached glyph is the same pixels as one
drawn fresh.

**The glyph cache.** `zc_text_cache(mem, bytes)` keeps stroke glyphs
that have been drawn through an unrotated transform, at their device
size, onto a 1bpp target or a grey print band (coverage is all or
nothing either way, so a cached glyph is the same pixels on both).
After that a glyph is one blit.
- It empties itself when full.
- The tests check that cached and uncached drawings are identical,
  including with a cache so small it keeps emptying.
- From the cache, a card's text costs about a third of drawing it
  fresh ([zcanvas](zcanvas.md#speed)).

**Turned text.** Through a turned transform (a card laid sideways on
a sheet) stroke glyphs are stroked turned, and bitmap glyphs are
turned with `zc_blit_xf()`: they turn with the card.

**For font writers.** The PDF and PostScript writers in
[print](print.md) make each face a Type 3 font of the glyphs a job
uses. `zc_font_resolve()` gives the codepoint actually drawn (a
look-alike, or 0 for the box), `zc_font_strokes()` a stroke glyph's
points in font units, `zc_font_rows()` a bitmap glyph's rows, and
`zc_text_pen()` the pen a style is drawn with.

## Big text

The glyph blitter draws glyphs at most 8 pixels wide, from glyph memory,
so the resident fonts stop at 6x12 ([window_manager.md](window_manager.md),
"Fonts in glyph memory"). A wider font goes through
`sw/common/zbigline.[ch]` instead: a line is put together in memory --
each glyph row a shift and an OR into a buffer laid out like the
framebuffer -- and copied to the screen with `z_fb_hw_blit_mem()`,
background and all.

```c
z_bigline_begin(text_w, line_h);           // the line, all background
x = z_bigline_text(x, s, n, &z_bigfont_10x20);
z_bigline_invert(sel_x0, sel_x1);          // a selection
z_win_select(&win);                        // the window's visible region
z_bigline_put(screen_x, screen_y, &text_area);
```

- **Clipping.** The blitter's scissor clips the copy to the window's
  visible region; `z_bigline_put()` clips it to the rectangle given (a
  text area, short of the scrollbar).
- **No flash.** The copy writes every pixel of the line, so the row is
  not cleared first.
- **Characters.** A character the font has not got is drawn as its
  look-alike (`zutf8.h`), else as a box; a wide one takes two cells; a
  control character is a space.
- **Cost.** A 10x20 character is roughly 1,300-2,400 CPU cycles to put
  together, against about 112 for a hardware glyph -- around 1 ms for a
  line of big print, 6-11 ms for a full 320x240 window (estimated from
  the measured blitter numbers in [gpu_blitter.md](gpu_blitter.md), not
  yet measured on the board). Fine for big print, where a window holds a
  few hundred characters; not a replacement for glyph memory in `term`.
- **Memory.** One line buffer, 2 KB, in the module: a process draws one
  line at a time.
- **No memory copy, no big text.** `z_bigline_available()` is false on
  a bitstream whose blitter has no memory-copy mode; an app then offers
  no big size.

**The fonts are compiled in**, by `tools/gen_bigfont.py`, from the
`.zfn` files in `sw/data/font/zfn/` -- the same misc-fixed data as the
card's. Glyph rows are stored in framebuffer order (bit k is column k),
so drawing needs no bit reversal. `python3 tools/gen_bigfont.py --check`
fails if a compiled font no longer matches its `.zfn`. Today there is one,
`z_bigfont_10x20` (207 glyphs, 8.7 KB), used by `text`'s big print.

## Testing

`make -C sw/common/tests -f Makefile.zcanvas fonts` runs
`test_zcfont`, 35 checks with ASan and UBSan, against the real files in
`sw/data/font`. It covers:
- metrics;
- refusing damaged files;
- coverage of Latin-9, Greek, quotes, dashes and arrows;
- widths, scaling and tracking;
- wrapping:
  - the lines are the words in order and each fits;
  - newlines and blank lines;
  - long words, the line limit, empty text, trailing spaces;
- the cache against the rasterizer;
- a card printed at 4x in bands against the whole page.

`look-fonts` writes the sample card at 100%, at 200% and at print
resolution to `/tmp/zcanvas`.
