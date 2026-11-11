# Touch

A finger on a touch screen, a pen on a tablet, and a machine with no
keyboard: what Zeitlos does with each, and where it is done.

The desktop does not change for touch. The same windows, dock and apps
take a finger as a mouse, with wm reading the gestures a finger makes.
The one thing that grows is the on-screen keyboard, which opens by
itself when it is needed.

Not in the small build (`Z_SMALL`, a 1MB machine:
[flash_apps.md](flash_apps.md), "Two sizes of machine"): there a finger
is read as a mouse and nothing more.

**Untested on hardware** (November 2026). Everything below has been
checked in the emulator ([zsys.md](zsys.md)), on its web page in a
phone-sized Chromium, and with host tests, but not yet on a board with
a real touch screen or tablet.

## Where a finger comes from

Every source writes the **virtual mouse**, `reg_vmouse` at
`0xf000_0400` (`VMOUSE`, on every board, `rtl/sysctl.v`):

| bits | field |
|---|---|
| 9:0, 19:10 | x, y, on the 640x480 screen |
| 22:20 | buttons (1 left, 2 right, 4 middle) |
| 24 | present: the pointer is this one, not the USB mouse's |
| **25** | **TOUCH**: a finger, not a mouse or a pen |
| **31:26** | **wheel**: notches, six bits, wrapping, counted by the writer |

Bits 25 and 31:26 came in November 2026; an older bitstream reads them
as 0 -- a mouse, no wheel -- and the macros are `Z_VMOUSE_*` in
`sw/common/zsoc.h`. The hardware cursor follows the virtual mouse while
it is present, and wm reads it before the USB mouse; the first move of
the USB mouse takes the pointer back (wm writes 0).

The writers:

- **`usb`**, for a USB touch screen or pen tablet (below);
- **the emulator's page**, for a phone or a tablet ([zsys.md](zsys.md));
- **the remote desktop's page** from a phone ([remote_desktop.md](remote_desktop.md)):
  bit 6 of the buttons byte is TOUCH, bits 5:3 a signed count of wheel
  notches (-4 to 3), in the same five bytes as before -- the ESP32
  refuses any other length, and an older Zeitlos reads only bits 2:0;
- `sw/apps/automate` and the remote desktop from a PC, as a mouse.

## A finger, read as a mouse (wm)

`sw/apps/wm/touch.c`, a filter every pointer sample passes through when
TOUCH is set; a mouse or a pen goes straight through.

| the finger | what wm and the apps see |
|---|---|
| lands | the pointer moves there; **no button yet** |
| lifts within 6 pixels (`TOUCH_SLOP`) | a **tap**: the left button down and up where it landed |
| moves more than 6 pixels | a **drag**: the left button down where it landed, then following |
| stays put for 500 ms (`TOUCH_LONG_MS`) | a **long press**: the right button, until it lifts |
| two fingers dragged | the **wheel**, from the writer (a notch every 30 page pixels, or every 16 screen rows from a USB touch screen) |

Holding the press back is what lets a drag start where the finger
landed: a window's titlebar is grabbed where it was touched, not 6
pixels later.

**The pointer hides** while the last input was a finger -- a cursor
jumping to each tap is noise -- and comes back with the first mouse
movement.

### Bigger targets, the same picture

Nothing on the screen changes size. Where a finger lands is moved onto
one of wm's own controls when it fell just short of one
(`touch_snap()` in `wm.c`), and the whole contact is offset by as much,
so a drag moves with the finger:

| control | for a finger |
|---|---|
| a titlebar icon (close, new, open...) | anywhere within 8 pixels of it |
| the titlebar | 8 pixels above it, and 4 into the content below |
| the resize grip | 24x24 pixels (12x12 for a mouse), from inside the corner |
| a dock icon | anywhere in its cell, the padding included |

Front to back: the first window the finger is inside ends the search,
since what is behind it there is covered. A finger already on a
control, or in a window's content and near nothing, stays where it
landed: the app gets exactly what was touched. Apps' own buttons are
the apps' (`zwidget` could do the same for them later).

On a phone, where the whole screen is small, the emulator's page also
zooms: pinch, and pan with the same two fingers.

## The keyboard, when it is needed

wm opens `keyboard` ([keyboard_app.md](keyboard_app.md)) along the
bottom of the screen when **the focused window wants text and there is
no keyboard**, and closes it when the focus moves to a window that does
not. A machine with a mouse and no keyboard gets it the same way.

**No keyboard** means neither USB port reports one, and no key from
anything but the on-screen keyboard has come for ten minutes (the
remote desktop's keys, or a demo's, say a keyboard is there). The
on-screen keyboard marks its own keys (`Z_KBD_EV_ONSCREEN`, bit 26 of a
key event, kept by the kernel) so that wm can tell. `zeitlos.cfg`:

```
system.keyboard.onscreen: auto      # auto (the default), always, or never
```

**Wanting text** is the app's to say:

- **`Z_WIN_FLAG_TYPES`** for a window that is all typing: `text`,
  `term`, `sheet`;
- **`z_win_wants_text(win, on, caret_y)`** for a window that wants text
  only sometimes -- and **`z_edit` calls it by itself** when a text field
  gains or loses the focus, so every dialog's field, `web`'s URL bar,
  `irc`'s line and `slide`'s fields need nothing more. The message is
  `Z_WM_WANTS_TEXT` (`zwm.h`).

**Making room**: if the keyboard would cover the caret (or, not known,
the window's bottom), wm moves the window up just enough, and puts it
back when the keyboard goes -- unless it has been moved since. The
keyboard stays above the window being typed into, raised again a moment
after a drag.

**For a finger**, wm opens it **large**: the whole width of the screen,
608x203 pixels, keys 40x34 labelled in the 8x16 font -- about 7.5 mm a key
on a 7" 800x480 panel. Opened for a mouse it is the small one at the
bottom, and Super+K or its dock icon opens the small one anywhere, as
before. A keyboard wm opened is closed by wm; one opened by hand stays
until its close box. Closing wm's by hand leaves it closed for that
window until the focus moves.

`keyboard` is a core app, in flash, so a touch-only machine with no card
can type ([flash_apps.md](flash_apps.md)).

## USB touch screens and pen tablets

`sw/apps/usb/usbh_dig.c`. A touch screen or a pen tablet reports where,
not how far, so it is not a boot device and the hardware cannot carry
it as it carries a mouse: it gets a **RAW poll slot**, its reports land
in 64 bytes of the packet buffer (`0x320`), and `usb` reads each one,
scales X and Y to the screen and writes the virtual mouse.

What is understood, from the report descriptor -- the best of the
device's application collections, in this order:

| collection | read | written |
|---|---|---|
| Touch Screen or Touch Pad (Digitizers 0x04, 0x05) | the first contact's Tip Switch, X, Y; Contact Count | TOUCH, the left button; two fingers turn the wheel |
| Pen (Digitizers 0x02) | Tip Switch, Barrel Switch, In Range, X, Y | the left button, the barrel the right; hovering moves the pointer; out of range gives the pointer back to the mouse |
| a mouse with absolute X and Y (Generic Desktop 0x02) | buttons 1 and 2, X, Y | as a mouse |

The third is the "mouse mode" many touch monitors and tablets start in,
and QEMU's `usb-tablet`. A boot mouse whose report protocol turns out to
be absolute is put in report protocol and read the same way.

One tried for: an XP-Pen-made **UGTABLET 4 inch PenTablet** (28bd:0913).
Its interface 0 is a boot mouse whose report protocol has the pen as
an absolute mouse (report 9: three buttons, X and Y 0..32767,
pressure), beside a relative mouse and the express keys as a keyboard;
`usb` reads report 9, so the pen points and its tip clicks. Its
interface 1 is the pen as a digitizer with pressure and tilt (inside a
Push/Pop, which the parser follows). On hardware, interface 0 answered
every poll with NAK -- nothing to send -- in the tablet's default mode,
so when the first interface `usb` reads is an absolute mouse, it reads
the device's next HID interface too and keeps the better: a touch
screen, then a pen, then an absolute mouse. For this tablet, interface
1, the pen. Both descriptors are test cases in
`tests/test_dig.c`.

A RAW slot's new data is what raises `poll_change` (IRQSTAT bit 3);
`usb` turns that interrupt on while a digitizer is bound, and polls
every ~10 ms besides. (The first build had neither, and on hardware the
tablet bound and the pen moved nothing: nothing stepped the device.)
`lsusb` shows the layout it read and what has come in -- reports, the
ones with the digitizer's report ID, the last report's first bytes, and
the last virtual mouse word.

`usb` says what it found at plug-in -- `usb: 3: a touch screen, X
0..4095, Y 0..4095` -- and `lsusb` shows `class=digitizer`. One
digitizer at a time; packets up to 64 bytes.

**Not done:** Windows-style multi-touch screens start in single-touch
mode until the host sets their Device Mode feature report; nothing sets
it, so they are read as they start. Pressure and tilt are read by
nothing yet -- a drawing app with pressure is a natural next step. A
tablet that needs a vendor's command to report at all (some older
Wacoms) is not one of these.

## Kaugummi

Kaugummi's RGB LCD connector has a 4-wire resistive touch panel's four
lines (XR, YD, XL, YU) on FPGA balls, with no ADC. Touch-or-not can be
read digitally; a position needs a controller (an XPT2046, say) or an
experiment on the board. Left for later; when it comes, it is one more
writer of the virtual mouse, and nothing above changes.

## Tests

- `sw/apps/wm/tests/test_touch.c`: tap, drag, long press, the snap's
  offset, a mouse taking over mid-drag, a pen's barrel (`make test` in
  `sw/apps/wm`).
- `sw/apps/usb/tests/test_dig.c`: QEMU's tablet, a relative mouse (not
  one), a touch screen with report IDs and a contact count, a pen with
  hover, barrel and out of range; finding the interface (`make` in
  `sw/apps/usb/tests`).
- `sim/sys`: `test_zdev` checks the virtual mouse and plugging the
  keyboard; `make test-input` boots with `--no-keyboard`, taps the
  welcome closed, taps `text` in the dock, checks the keyboard opens for
  it and that it does not with a keyboard plugged in; `make web-test`
  does the same in a phone-sized Chromium.
