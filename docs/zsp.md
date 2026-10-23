# Sprite sheets (.zsp)

A sprite sheet is a text file of named pictures -- sprites, tiles, the
mouse pointer's shapes -- each pixel a colour index or see-through,
with a palette of 2, 4, 16, 64 or 256 colours. pix edits them
([pix_app.md](pix_app.md)); games, wm (the pointer, [cursor.md](cursor.md))
and the host tools read them.

`sw/common/zsprite.c` reads, writes and reduces them: pure C, no
hardware and no files, so pix, the games and the host tests share it.
A colour picture (a ZBM with planes, [pix_app.md](pix_app.md)) carries
the same palette in a few bytes: `zsp_palette_to_bin()` and
`zsp_palette_from_bin()`, the layout in `sw/common/zbm.h`.
Tested by `make -C sw/common/tests -f Makefile.zsprite` (350 checks).

## The idea: one sheet, any board

A board shows 2, 16, 64 or 256 colours ([color.md](color.md)). Art is
drawn once, in as many colours as the artist wants, and every board
shows it -- the 256-colour sheet on a 16-colour board, the 16-colour
one in a monochrome window -- **without anything being worked out from
RGB when it is shown**. Each palette entry says what stands in for it
with fewer colours, and those answers are stored in the sheet:

| Field | With fewer colours | In sheets of |
|---|---|---|
| `to64` | the entry 0..63 shown at 64 colours | 256 |
| `to16` | the entry 0..15 shown at 16 colours | 64, 256 |
| `slot` | one of 4 slots (each with its own colour) at 4 colours | all |
| `ink` | one of 17 dither greys, 0 black .. 16 white: the monochrome screen, and black or white (ink ≥ 8) at 2 colours | all |

The palette is ordered so that **each smaller palette is the start of
the larger one**: entries 0..15 are the 16-colour palette, 0..63 the
64-colour one, so an entry below 16 stands for itself at 16.

So what pix previews as 16 colours is exactly what a 16-colour board
shows, and a game loads its art reduced once, at load time, to the
board's planes: the same drawing code over fewer planes, as fast as
before.

### Defaults, and the artist's choices

When a palette is made, or a colour's RGB changes, the fields are
worked out once (`zsp_defaults()`):

- `to64`, `to16`: the nearest entry of the smaller palette (RGB
  distance, green weighted most).
- `slot`: the brightness quartile.
- `ink`: the brightness, 0..16.

A field the artist sets is **given**: written with a `!` after it, and
never worked out again. pix's palette editor (F8) changes them; D puts
one back to its default.

### Must differ

Two colours that must stay apart -- the player and the background --
are a `differ` pair. `zsp_collision()` finds the first pair that
reaches the same entry at 64, 16, 4 or 2 colours, and pix warns with
the colour count.

## The format

```
zsp 1
colours 16
slots 000 555 aaa fff
colour 0 000 0 0
colour 1 fff 3 16
colour 9 740 1 5!
...
differ 1 9
# comments are kept as they are; anim: an animation, 80 ms a frame
anim run 80 mouse0 mouse1 mouse2 mouse1
sprite mouse0 16 12
	......9999......
	.....9111199....
	...
```

| Line | Means |
|---|---|
| `zsp 1` | the format |
| `colours N` | 2, 4, 16, 64 or 256; before the first sprite. None: 2 |
| `slots a b c d` | the 4-colour form's own colours, RGB444 (sheets of 4 and more) |
| `colour i rgb [to64] [to16] [slot] [ink]` | entry i: its RGB444, then the fields its sheet has (table above), each with `!` if given. A missing entry keeps the default palette's |
| `differ a b` | a must-differ pair |
| `sprite name w h` | then h rows of w pixels (a name has no spaces; up to 23 letters; at most 1024 x 1024) |
| `anim name ms cell...` | an animation: each frame shows `ms` milliseconds (10 to 10000), the cells by name, one perhaps more than once, up to 64 frames ([pix_app.md](pix_app.md#animation)) |
| anything else | kept, and written back where it was: comments |

**Rows**: tabs first (spaces too, when there are more than the row
needs), then the pixels:

- 16 colours or fewer: one character a pixel -- `.` or a space
  see-through, `0`..`9` `a`..`f` an index. The alphabet
  `ar_sprite_make()` already reads ([arcade.md](arcade.md)).
- 64 or 256: two hex digits a pixel, `..` see-through.
- **A sheet with no `colours` line is 2 colours**, and there `0` is
  colour 0 and any other mark (`#`, `X`, `1`) colour 1: what the
  pointer's file and hand-drawn art use.

A 2-colour sheet with plain black and white writes no `colour` lines,
so the pointer's file looks as it always did. A sheet read and written
again comes back byte for byte (mausrun's two, below, are checked).

## The default palette

`zsp_palette_init()`: the Commodore 64's sixteen first (0 black, 1
white, so 2-colour art looks right in it -- the same as
`z_color_palette_c64`); in a 64-colour sheet, 16..63 three shades of
each (darker, lighter, darkest); in 256, 64..255 twelve more of each,
from near black to near white. Slots: black, dark grey, light grey,
white.

## For a game

```c
zsp_sheet_t s;
static uint8_t px[16 * 16], op[16 * 16];
int planes = z_color_planes_max();               /* 4, 6 or 8 */
int n = planes >= 8 ? 256 : planes >= 6 ? 64 : 16;

if (zsp_parse(&s, text, len, err, sizeof err)) {
    int i = zsp_find(&s, "mouse0");
    zsp_pixels(&s, i, n, px, op);                /* reduced once, here */
    ar_sprite_from_indices(&spr, px, op, s.spr[i].w, s.spr[i].h);
    zsp_free(&s);
}
```

| Function | Does |
|---|---|
| `zsp_parse(s, text, len, err, n)` | text to a sheet; false with "line 7: ..." |
| `zsp_write(s, &len)` | the sheet as text (malloc'd) |
| `zsp_init`, `zsp_free`, `zsp_add`, `zsp_find` | a sheet, a sprite by name |
| `zsp_pixels(s, i, target, px, opaque)` | sprite i reduced to `target` colours, one byte a pixel |
| `zsp_reduce(p, idx, target)` | one index with fewer colours |
| `zsp_shown_rgb(p, idx, target)` | what it looks like then, RGB444 |
| `zsp_ink(p, idx)` | its grey, 0..16 |
| `zsp_palette_init`, `zsp_defaults`, `zsp_lum` | palettes |
| `zsp_collision(p, &a, &b, &tier)` | the first must-differ pair that collides |

## Where sheets are

| File | What |
|---|---|
| `/data/wm/cursor.zsp` | the pointer's large pair, 2 colours ([cursor.md](cursor.md)) |
| `sw/data/mausrun/tiles.zsp` | mausrun's 29 tiles, 2 colours ([mausrun.md](mausrun.md#art)) |
| `sw/data/mausrun/sprites.zsp` | its 25 sprites, 4 colours: 1 the sprite, 2 its outline |
