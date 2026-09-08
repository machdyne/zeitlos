# PNG

`sw/common/zimg.c`, behind `Z_IMG_HAVE_PNG`. On in `web` and `view`.

## What it decodes

| | |
|---|---|
| colour types | grayscale, RGB, palette, grayscale+alpha, RGBA |
| bit depths | 1, 2, 4, 8 (palette and grayscale); 8 only for colour |
| filters | all five — None, Sub, Up, Average, Paeth |
| chunks | IHDR, PLTE, IDAT across any number of chunks, IEND |

Alpha is **read and discarded**. There is nothing to composite
against on a 1bpp screen, and treating a transparent pixel as its own
colour is closer to right than treating it as white.

## What it refuses, and why refusing is the point

| | |
|---|---|
| interlaced (Adam7) | `Z_IMG_E_UNSUPPORTED` |
| 16-bit samples | `Z_IMG_E_UNSUPPORTED` |
| wider than 2048 | `Z_IMG_E_TOOBIG` |

**Adam7 breaks row streaming outright.** Its seven passes each have
their own geometry, so a "row" arriving from the decompressor is not a
row of the picture, and reassembling one needs the whole image
resident — the one thing `zimg.c` is built never to require.
Interlaced PNG is rare on the web, and a clear refusal beats a wrong
picture.

That applies to all three: a decoder that half-handles a variant
produces something that looks like a rendering bug rather than an
unsupported file, and the person seeing it has no way to tell which.

## Memory

The scratch lives in `zimg.c`'s decoder union with every other format,
so the cost is the **difference against the largest existing member**
(GIF's LZW dictionary, ~17.6KB) rather than the whole thing:

| | |
|---|---|
| DEFLATE window | 32KB, and not optional — back-references reach 32768 bytes |
| two scanlines | 16KB at the 2048-pixel cap |
| palette | ~1KB |
| **net cost** | **~33KB of .bss** |

Two scanlines because every filter except None refers to the row
above, so the previous row must survive until the current one is
complete. They are two fixed buffers alternated by a flag rather than
swapped pointers: the filter byte sits at `[0]` and the pixels at
`[1]`, so `prev = cur + 1; cur = t` makes `cur` creep forward a byte
every row.

The 2048-pixel width cap is what keeps the scanlines at 16KB. At
`Z_IMG_SRC_MAX_W` (5120) with 16-bit RGBA they would be 80KB alone.
2048 still allows a 1/2 or 1/4 reduction onto a 640-pixel screen,
which is what a page image needs.

## The compressed stream

A zlib stream (RFC 1950) spread across one or more IDAT chunks, whose
boundaries have **nothing to do with the deflate structure**. It is
fed to `sw/common/zinflate.c` in whatever pieces the chunks provide,
which is exactly the case that decoder was built to survive — see
docs/http.md for the three suspend-point bugs its chunk sweep caught.

`zinflate` is already linked in `web` for `Content-Encoding: gzip`, so
PNG there costs only the decoder and its scratch.

## CRCs

Chunk CRCs are **read and discarded**, like the gzip trailer. A
CRC-32 over every byte costs more than this machine can spare on an
image it is about to dither to one bit, and a corrupt IDAT already
fails in the decompressor. Said plainly because "has a CRC" and
"checks the CRC" are easy to confuse.

## Testing

    python3 sw/common/tests/gen_png_corpus.py /tmp/pngc
    cc -std=gnu99 -DZ_IMG_HAVE_PNG=1 -I sw/common \
       -o /tmp/t sw/common/tests/test_png.c \
       sw/common/zimg.c sw/common/zinflate.c && /tmp/t /tmp/pngc

19 checks: every colour type, power-of-two scaling, and all five
refusals. The corpus is written by PIL — i.e. by libpng — so the
decoder is checked against an encoder it had no hand in.

The five row filters are not tested individually because they cannot
be: libpng picks per row, so one image with hard edges and a flat
field exercises all of them and no test can ask for a particular one.

Each success is checked for **ink**, not just a return code: a decode
that "succeeds" into an empty buffer would otherwise pass, and
`rv == OK` says the stream parsed, not that a picture came out.
