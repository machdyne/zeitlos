# The screen's size

Every board's display is 640x480 today, and VRAM is 640x480 at one bit
a pixel. That is the hardware's business. **What a program lays itself
out against is the size the system reports**, so that a later board
with a wider or larger display -- 800x480, 1280x720, 1920x1200 -- is a
change to the gateware and the system, not to every program.

## Asking

| | |
|---|---|
| `z_screen_size(&w, &h)` | the screen's size in pixels (zeitlos.h) |
| `z_screen_w()`, `z_screen_h()` | the same, one at a time |
| `(screen-size)` | the same in Scheme: `(640 480)` |
| `Z_SCREEN_MIN_W`, `Z_SCREEN_MIN_H` | 640x360: the least it will be |
| `Z_SCREEN_MAX` | 4095: the most any interface carries |

**640x360 is the floor.** 640 wide, because every app's windows
assume it; 360 high, because a 16:9 desktop at one bit a pixel fits
today's 37.5 KB of VRAM at 640x360 (28 KB) -- a 16:9 board need not
wait for more memory. A window no larger than 640x360 fits every
screen there will be. Game mode's 320x240 is a mode, not the desktop.

The size changes while programs run, on a machine with screen modes
([screen_modes.md](screen_modes.md)): 640x480, 640x360, 800x600,
1024x768 or 1280x720, chosen with Ctrl+Super+Esc or in Settings. wm
lays the desktop out again and sends every program `Z_WM_SCREEN`
(zwm.h); a program that keeps the size asks again then. A kernel from
before the call (`Z_SYS_SCREEN`) answers 640x480, so a program that
asks runs on every kernel.

**Use it** for anything that means "the screen": centring a dialog,
clamping a window's size, putting the keyboard at the bottom, fitting
a slide to a full-screen show.

**Not** for the framebuffer. `Z_SCREEN_W` and `Z_SCREEN_H` (zgfx.h)
are VRAM's geometry -- its stride, its rows, the accent plane's sheet
below the desktop, game mode's viewport -- and the drawing code
addresses VRAM with them. A buffer the size of the framebuffer, and a
whole-framebuffer clear, use those. Nothing else should.

## Where the size travels

wm's messages carry coordinates in 12 bits, 0-4095
([window_manager.md](window_manager.md)):

| pack | x | y | |
|---|---|---|---|
| `Z_WM_PACK_MOUSE` | 9:0, top two bits 26:25 | 19:10, top two bits 28:27 | 31:29 reserved |
| `Z_WM_PACK_XY` (redraw, moved) | 19:10, top two bits 30:29 | 9:0, 11th bit 31 | y to 2047: bit 28 is `Z_WM_REDRAW_DAMAGE` |
| `Z_WM_PACK_PLACE` | 9:0, top two bits 21:20 | 19:10, top two bits 23:22 | 31:24 reserved |
| `Z_WM_PACK_RESIZE` | 12 bits already | | |

**Below 1024, every word is what it was** when the fields were 10 bits,
so a program built before the change reads it exactly as before, and a
new one reads an old wm's words correctly. (`test_wmpack.c` checks
both.) The window position's y stops at 2047, which covers every size
named above; a screen taller than that needs another message.

## What stays 10 bits

Nothing, now: the cursor and the pointer's x are 11 bits on the boards
with a 1280-wide mode -- bit 28 of the USB cursor register and bit 23 of
the virtual mouse register, which read 0 before
([screen_modes.md](screen_modes.md)). A program still never reads them
for layout: it gets coordinates from wm.

## Trying another size

`zeitlos.cfg`'s `system.screen.size` sets what the system reports, for
testing ([config.md](config.md)):

```
system.screen.size: 640x360
```

640x360 to 4095x4095; anything else is ignored, and said so. The
display stays 640x480. At **640x360** -- the floor, and what a 16:9
board would have -- wm draws the desktop in the top 360 rows and the
rest stays black, so a program that still assumes 480 rows is plain to
see. A **larger** size shows, in the log, where a program would go on a
bigger screen (the dock goes to the bottom of 768 rows, off the
640x480 display). `make -C sim/sys test-screen` boots the emulator with
a card holding only the 640x360 line and checks that the dock, text and
the on-screen keyboard that pops up for it (with no keyboard plugged
in) are all inside it.

## Where the programs are

Done: wm (the desktop, placement, clamping, dragging, the dock, the
keyboard pop-up, full screen, maximize, the lock screen's box), dialogs
(`zdialog.c`), the on-screen keyboard (its "bottom" place), slide (the
editor's window and the show's fit), chip8's clear (the framebuffer,
now named), and the Scheme API.

**wm keeps every window on the screen.** A window asked for larger
than the screen is made the screen's size (wm logs "asked for WxH,
larger than the screen"), and the app gets the size it got in the
reply, as it always has. A new window's cascade that would run it off
the right or bottom edge brings it back in.

**Every windowed app fits 640x360.** Started one by one at 640x360 in
the emulator (50 apps: every app on the card with a window, and the
core ones), two asked for more than the screen: learn (600x420) and
info (180x370). Both lay themselves out from the size they get, so the
clamp is all they need -- learn's page is shorter, info shows a
process fewer. Every other window was 360 tall or less.

Fine as they are: learn, draw, view, pix and mahjong compare sizes with
640 and 480 to keep inside their page and canvas buffers, which are the
framebuffer's size; `automate` writes the virtual mouse register and
clamps to the hardware's 639x479.

Still 640x480: the full-screen demos and games (mausrun, mauskart3d,
planedemo, gpu3d, gpudemo, gldemo, space3d) draw the whole framebuffer.
On a 16:9 board their bottom 120 rows would be off the display; they
need to draw into `z_screen_size()`, or use game mode's 320x240, which
is a mode of its own. That is for when such a board exists.

`Z_SCREEN_W` and `Z_SCREEN_H` are the framebuffer's geometry at run
time now (zgfx.h, [screen_modes.md](screen_modes.md)): a buffer that
needs a size when it is compiled uses `Z_FB_640_W/H` (a 640x480 page,
as draw's canvas and learn's page) or `Z_FB_MAX_W/H`.
