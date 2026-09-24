# Text encoding

How Zeitlos stores, passes around and draws text that is not ASCII.
The keyboard side -- what produces those characters -- is
[keyboard_layouts.md](keyboard_layouts.md).

## The rule

**Text that crosses a process boundary is UTF-8.** That covers the
clipboard, messages, port streams, window titles and speech.

ASCII is valid UTF-8, so every app that only ever produces ASCII
already follows the rule and needs no change. Inside an app, text can
be kept however suits it:

- **ISO 8859-15 bytes** (Latin-9) -- one byte, one character, one cell.
  The simplest thing for an app that only needs Western European text;
  it converts at its edges with `z_utf8_to_l9()` and `z_l9_to_utf8()`.
- **UTF-8** -- for an app that must not lose anything it cannot show,
  such as an editor. It walks the text with `z_utf8_next()`.

Apps move to UTF-8 one at a time; nothing has to change all at once.

## Why ISO 8859-15 and not 8859-1

Latin-9 is Latin-1 with eight rarely used symbols replaced by the euro
sign and the letters French, Finnish and Estonian were missing:

| byte | Latin-1 | Latin-9 |
|---|---|---|
| `0xA4` | ¤ | € |
| `0xA6` | ¦ | Š |
| `0xA8` | ¨ | š |
| `0xB4` | ´ | Ž |
| `0xB8` | ¸ | ž |
| `0xBC` | ¼ | Œ |
| `0xBD` | ½ | œ |
| `0xBE` | ¾ | Ÿ |

It is the same number of glyphs, so the choice costs nothing in glyph
memory. The eight dropped symbols can still be typed on some layouts
(¦ on a UK keyboard, ´ and ¨ as a dead key followed by Space); they
draw as the missing-glyph box.

## The hardware fonts

`z_font_5x8` and `z_font_6x12` -- the two fonts that live in glyph
memory and are drawn by the blitter -- cover ASCII and the upper half
of Latin-9, 192 glyphs each. Bytes 0x80-0x9F are C1 control codes,
never drawn, and are not stored: a font records that gap in
`gap_lo`/`gap_hi` (`zfont.h`), and `z_font_index()` skips it. Without
the gap the two fonts would need 4480 bytes; with it they fill the
3840-byte font region exactly:

| font | glyphs | bytes | offset |
|---|---|---|---|
| `z_font_5x8` | 192 x 8 rows | 1536 | 0 |
| `z_font_6x12` | 192 x 12 rows | 2304 | 1536 |

`z_font_5x7` and `z_font_8x16` are unchanged: ASCII only, software
rendered.

The glyphs come from the public-domain misc-fixed BDFs already in
`sw/data/font/` (Markus Kuhn's ucs-fonts), including the local change
to the 5x8 full stop, so nothing was drawn by hand. `bdf_to_mem.py`
extracts all 192 by default (`--ascii` for the old 96), looking each
Latin-9 byte up by its Unicode codepoint; `gen_font_data.py` writes
`zfont_data.c`. The ASCII glyphs came out bit-identical to what was
there before, apart from DEL.

### The missing-glyph box

0x7F (DEL) is not a character and the BDFs have no glyph for it, so
`bdf_to_mem.py` puts a hollow box there, as tall as the font's capital
H. `zfont.h` calls it `Z_GLYPH_MISSING`. It is what the codepoint
drawing functions below show for anything the font cannot draw -- a
Japanese character, a Latin-1 symbol Latin-9 dropped, a malformed byte
-- always one cell wide, so the text after it stays in its column. A
font without the box (5x7, 8x16) shows `?` instead.

## Drawing

The byte functions -- `z_fb_draw_char()`, `z_fb_draw_text()`,
`z_win_draw_text()` and their `2` forms -- take Latin-9 bytes. Before
this change, bytes 0x80-0xFF drew nothing; now 0xA0-0xFF draw their
Latin-9 glyph.

For UTF-8 and codepoints (`zgfx.h`, `zwin.h`):

| function | draws |
|---|---|
| `z_fb_draw_cp(x, y, cp, ...)` | one codepoint |
| `z_fb_draw_cp2(x, y, cp, fg, bg, ...)` | one codepoint, solid cell |
| `z_fb_draw_utf8(x, y, s, ...)` | a UTF-8 string |
| `z_fb_draw_utf8_2(x, y, s, fg, bg, ...)` | a UTF-8 string, solid cells |
| `z_win_draw_utf8(win, x, y, s, ...)` | a UTF-8 string, window-relative |
| `z_win_draw_utf8_2(win, x, y, s, fg, bg, ...)` | the same, solid cells |

Every character maps to its Latin-9 glyph, so all of ASCII and Latin-9
draws in hardware exactly as fast as ASCII always has. The UTF-8
functions decode a run at a time and hand the runs to the byte
functions, so clipping, the skip past a clip's right edge and the wait
for the last blit all behave identically. The four UTF-8 entries are
also in libz for programs compiled with `zcc` (ABI 7, [libz.md](libz.md)).

## Helpers (`sw/common/zutf8.h`)

Header-only, so using them needs no Makefile change: every app lists
its common objects by hand, and a header is the one thing that needs
nothing added.

| function | does |
|---|---|
| `z_utf8_next(&s, end)` | decode one codepoint and advance; `Z_UTF8_BAD` (U+FFFD) for a malformed byte, consuming exactly that byte |
| `z_utf8_next_z(&s)` | the same for a NUL-terminated string |
| `z_utf8_valid(s, len)` | true if every byte is part of valid UTF-8 |
| `z_utf8_count(s, len)` | codepoints, counting each malformed byte as one |
| `z_utf8_put(cp, out)` | encode one codepoint, returns its length (1-4) |
| `z_l9_to_cp(b)` / `z_cp_to_l9(cp)` | Latin-9 byte to codepoint and back; 0 if Latin-9 has none |
| `z_l9_to_utf8(...)` | convert a Latin-9 string to UTF-8 |
| `z_utf8_to_l9(..., subst, &lost)` | convert UTF-8 to Latin-9, substituting and counting what does not fit |

The decoder rejects overlong forms, surrogates, values above U+10FFFF
and truncated sequences, and never reads past `end`. Because a
malformed byte is consumed alone, a caller can tell which byte it was
(`s[-1]`) and keep it -- how an editor avoids corrupting a file it
cannot fully decode. Latin-1 text read as UTF-8 is the common case of
that: `ä` in Latin-1 is the single byte 0xE4, which UTF-8 cannot start
a character with.

## Japanese

Japanese is drawn from a **12x12 font held once for every app** by a
small service, `sw/apps/jfont`. Start it with `system.font.japanese:
yes` in `/zeitlos.cfg` (wm runs it at boot) or `run jfont`.

**The font** is Shinonome `shnmk12`: 6,879 JIS X 0208 glyphs --
hiragana, katakana, 6,356 kanji, fullwidth forms and JIS symbols --
public domain (its authors declare they will not exercise their
rights, which is how Japanese law allows it; see the `xfonts-shinonome`
copyright file). 12x12 is exactly two cells of `z_font_6x12`, so a
character sits in the two columns a terminal, `wcwidth()` and
`z_cp_width()` give it. `tools/gen_jfont.py` converts the BDF,
mapping each JIS code to Unicode through EUC-JP, into
`sw/data/font/jp12.zfn` (178,868 bytes, format in the script's header):
a sorted codepoint table and 24 bytes a glyph. The release puts it at
`/font/jp12.zfn`.

**Why a service.** The font is 175KB: more than any app's 16KB heap,
and too much to load into every process that might draw a kanji. Every
byte of memory has one physical address that any process may read
([mpu.md](mpu.md)), so `jfont` loads it once and apps read the glyphs
out of its memory. It costs about 190KB of RAM while it runs -- a fifth
of a 1MB board -- which is why it only runs when asked. Its own code is
3.7KB; it prints without stdio for exactly that reason.

**Finding it takes no message.** A lookup happens inside drawing code,
in whatever app is drawing, and must not touch that app's mailbox. So
`zgfx.c` finds `jfont` with `z_proc_list()` -- by its REGISTERED name,
`jfont0`, which is what the list reports (the pid registry numbers
names; the first release looked for `jfont` and never found it) -- and
scans
its block once for the descriptor `jfont` wrote in front of the font
(`sw/common/zjfont.h`): two magic words, written last so a half-loaded
font is never seen, and a check word that includes the descriptor's
own physical address, so a stray copy of the magic -- or a dead
`jfont`'s memory reused -- never matches. The address is then kept and
re-checked with two loads before every use. While `jfont` is not
running the search is retried every two seconds, not every character.

**Drawing.** `z_fb_draw_utf8()`, `z_fb_draw_cp()` and the window forms
give a CJK character two columns always. At 6x12 with `jfont` running
it is drawn from the font, in software -- the blitter's glyphs are at
most 8 pixels wide -- and otherwise it is the missing-glyph box and a
blank, still two columns, so text after it never moves. At 5x8 there is
no Japanese font small enough to read; switch `text` to 6x12 (the
titlebar font button).

**Where it shows.** `text` counts columns by width
([text_editor.md](text_editor.md)): Japanese types, wraps between any
two characters, and the caret, Up/Down and clicks land on the right
column. Programs compiled with `zcc` get it too (libz gained
`z_proc_list()` for the lookup). `term` keeps each wide character's
codepoint in its cell and its scrollback, so Japanese is copied and read
aloud as itself, and drawn when `term` is built at 6x12
([terminal.md](terminal.md), "UTF-8"). `read` still turns each line into
one glyph byte per column, so Japanese there is correctly spaced boxes;
drawing it needs its line model to carry codepoints. Japanese is typed
with the `ja` layouts ([keyboard_layouts.md](keyboard_layouts.md),
"Japanese input"), in kana: kanji conversion awaits a dictionary.

## Known gaps

- **`text` is UTF-8** now -- the first app to keep UTF-8 internally
  ([text_editor.md](text_editor.md), "Encodings").
- **`term`, `read` and `vi` are UTF-8** too: `term` decodes what
  arrives into one Latin-9 cell per character, two for a wide one
  ([terminal.md](terminal.md), "UTF-8"); `read` turns each line into
  glyph bytes as it reads it ([read_app.md](read_app.md),
  "Characters"); `vi` has nextvi's own UTF-8 back on.
- **Filenames stay ASCII.** The FAT layer is built 8.3-only
  (`FF_USE_LFN 0`) with `FF_CODE_PAGE 932` (Shift-JIS) in
  `sw/os/fs/fatfs/ffconf.h`. In code page 932, bytes such as 0xE4 are
  the first half of a two-byte character, so an accented byte in a
  short name would be misparsed. Until that is revisited, keep file
  names ASCII.

## Tests

`sw/common/tests/test_text.c` covers the decoder (valid, malformed,
overlong, surrogate, truncated, Latin-1-as-UTF-8), every codepoint's
encode/decode round trip, every Latin-9 byte's round trip, the
conversions, and the font indexing -- including that the two hardware
fonts fill glyph memory exactly:

```
cc -std=gnu99 -Wall -I sw/common -o /tmp/test_text \
   sw/common/tests/test_text.c sw/common/zfont_data.c
/tmp/test_text
```
