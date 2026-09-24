# Captions

Large text over the whole desktop, with no window: the narration
captions in the [demos](demo.md), and any system-wide indicator — a
volume level, a mode change — that should be seen without taking focus
from anything.

    sw/common/zcaption.h/.c         the API, and the renderer wm uses
    sw/common/tests/test_zcaption.c host test (draws PBMs you can look at)
    sw/apps/wm                      draws it (Z_WM_CAPTION, zwm.h)

```c
#include "zcaption.h"

z_caption_show("Hello, world", Z_CAPTION_SCALE(3));
z_caption_show("Volume 40", Z_CAPTION_COMPACT | Z_CAPTION_CENTER
                            | Z_CAPTION_TIMEOUT(1500));
z_caption_hide();
```

Link `zcaption.o`. The text is copied, so a stack buffer is fine.

## What it looks like

The 6x12 font — the larger of the two in glyph memory — scaled up by
software, once per change. That keeps all of ISO 8859-15: umlauts, ß,
accents, the euro sign. A character Latin-9 lacks (kana, for one) is
the missing-glyph box. `\n` breaks a line; long lines word-wrap, up to
three lines.

| Option | |
|---|---|
| `Z_CAPTION_SCALE(1..3)` | size; 0 means 2 |
| `Z_CAPTION_BOTTOM` / `_TOP` / `_CENTER` | position; bottom sits just above the dock |
| `Z_CAPTION_COMPACT` | a box fitted to the text instead of a full-width band |
| `Z_CAPTION_INVERSE` | lit box, dark text (default: dark box, lit frame and text) |
| `Z_CAPTION_TIMEOUT(ms)` | hide by itself; 0 = until replaced |

There is **one** caption. Showing one replaces the last. It does not
belong to the process that set it and outlives it; give it a timeout if
it should go away by itself.

### How big

| Scale | Cell | Letters per line | On a phone (the 640-pixel frame ≈ 390pt wide) |
|---|---|---|---|
| 1x | 6x12 | ~100 | ~7pt — not legible |
| 2x | 12x24 | ~50 | ~15pt — the floor; fine full-screen on YouTube |
| 3x | 18x36 | ~33 | ~22pt — comfortable on a phone |

A common rule of thumb for burned-in subtitles is a cap height of at
least 5% of the frame, which is 24 pixels at 480 lines: 2x is the
minimum, 3x is what the social cut uses. If a video is cropped to 1:1
or 9:16, the band is wider than what survives; use `Z_CAPTION_COMPACT`
with short text so the box stays in the middle.

## How it stays on top

Apps draw straight into the framebuffer, inside the visible regions wm
gives them, so a caption drawn by some other process would be painted
over by the next repaint — and would damage whatever it covered. The
caption is therefore wm's own:

- **It is an occluder** in every window's visible region
  (`window_visible_region()`), in front of everything. Apps are clipped
  around it by the regions they already obey — including wm's chrome
  and the dock, which are drawn through the same regions.
- **wm's "unrestricted" state is the screen minus the caption** while
  one is up (`wm_unclip()`, which replaced every
  `z_gfx_clear_visible()` in `wm.c`). Every zgfx fill, line, glyph and
  blit honours the visible list, so no other code in wm needed to know.
- **It is re-blitted, not redrawn**: rendered once per change into a
  bitmap, then one blitter operation copies it to the screen. The copy
  writes identical pixels each time, so refreshing it cannot flicker.
  wm refreshes it after any repair and twice shortly after it appears
  (about 50 and 300ms), to cover a frame an app had in flight against
  its old, wider region.

Changing to a caption of the **same size** touches nothing but the
caption's own pixels. That is why the default is a full-width band:
consecutive one-line captions occupy exactly the same rectangle, so no
window underneath is asked to repaint between them. A change in size
narrows or widens the regions of the windows underneath; the ones that
gain pixels are repaired and asked to redraw.

Not drawn while an app holds the screen (`Z_WM_GAME_GRAB`) or while a
window drag's XOR band is up; it comes back with the repaint that
follows. In game mode (the 320x240 camera) it is laid out inside the
camera at half the scale, since the camera doubles every pixel.

Clicks on the caption go to whatever is under it: it is not a window
and never takes focus.

## Cost

About 12KB of `wm` bss (the 640x144 bitmap) and 2KB of code. Rendering
is per glyph row, a few word operations each: well under a frame for
three lines at 3x.

## Tests

```
cc -std=gnu99 -Wall -DZCAPTION_HOST -I sw/common -o /tmp/tcap \
   sw/common/tests/test_zcaption.c sw/common/zcaption.c sw/common/zfont_data.c
/tmp/tcap /tmp/cap          # also writes /tmp/cap-*.pbm
```

Latin-9 conversion, band and compact geometry, wrapping, the
three-line limit, that same-length captions have the same geometry,
and the frame.

## Not done

- Kana and kanji: the caption uses the hardware font's Latin-9 only.
  The `jfont` service could supply them, at 12x12 scaled.
- The layout indicator over the dock (Super+Space) and a volume display
  are natural callers; neither has been moved over yet.
