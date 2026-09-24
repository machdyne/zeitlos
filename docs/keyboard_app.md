# keyboard -- the on-screen keyboard

`sw/apps/keyboard`. A keyboard drawn on the screen, labelled for
whichever layout is active, and typed with the pointer.

```
> run keyboard
```

or **Super+K**, or its dock icon. It is single-instance: starting it
again while it is up does nothing (it registers as `keyboard0` and a
second copy exits at once). It has two jobs:

- **Seeing and trying a layout.** Every key shows what it types in the
  active layout, with Shift and AltGr as they stand. The status line
  under the keys says what the last key produced -- the character, its
  codepoint and its UTF-8 bytes -- or that it was a dead key waiting for
  the next one. The **Lay** key (bottom right, showing the layout's two
  letters) steps through every layout there is, not just the ones in
  `system.keyboard.layouts`, and makes each the active one.
- **Typing with no keyboard at all** -- a touchscreen, or a machine
  whose only input is a mouse.

## Where the keys go

A key is sent exactly the way a physical key arrives: as a raw USB HID
event, a press and a release, through `hid_inject()` -- the path the
ESP32 remote desktop already uses. The kernel stamps the active layout
into it, and `wm` translates and delivers it like any other key
([keyboard_layouts.md](keyboard_layouts.md)). So everything a real
keyboard gets, this gets, with no code of its own for any of it: the
layout, dead keys, AltGr, Caps Lock, Super+Space, Japanese input,
`wm`'s shortcuts.

The keys go to the **focused window**, where typed keys always go. The
keyboard's own window never takes focus: it is created with
`Z_WIN_FLAG_NO_FOCUS` ([window_manager.md](window_manager.md)). Clicking
it raises it and delivers the click, and it can be dragged, but the
focus stays on the window you were typing into, and Alt+Tab passes it
by. So: focus the window to type into, then tap keys.

## Keys

Five rows, 60 quarter-key units each, the usual PC shape with the ISO
keys (`<>` beside left Shift, `#` beside Enter) and the key beside
right Shift that Brazilian and Japanese keyboards have; a layout that
does not use a key leaves it blank. The bottom row has Esc, the
modifiers, Space and the arrows.

**Labels** come from `z_kbd_translate()` -- the call `wm` makes, on the
same generated tables -- so they cannot disagree with what the key
types. Nothing in the app knows any layout. Ctrl is left out when
labelling, or every letter would show as a control code. A dead key
shows its accent; the few accents ISO 8859-15 lacks on their own (the
lone acute, diaeresis, cedilla) show as the ASCII mark that looks most
like them rather than the missing-glyph box.

**Modifiers are sticky**, which is what a pointer needs: tap Shift, Ctrl,
Alt, AltGr or Super and it holds (drawn inverted) for the next key, then
lets go; every label follows it while it holds. **Caps** stays until
tapped again. Super then Space is Super+Space -- the next layout in the
configured cycle.

The keyboard follows the active layout: switch it with Super+Space, or
from `settings`, and the labels change within an eighth of a second.

## Size

About 50KB: the drawing code, and the layout tables every layout needs.
It is built with section garbage collection and formats its status line
by hand -- `printf` alone would link newlib's whole formatter, floating
point included, and was two thirds of the first build's 158KB.

## Tests

`sw/apps/keyboard/tests/keys.c` runs the real `keyboard.c` with a
scripted mouse and kernel: every row is the full width, a click injects
a press and a release of the right usage, labels follow the layout
(German Y key labelled `z`, a-umlaut, the dead keys), sticky Shift is
carried in one event and then let go, AltGr is the right-Alt bit, Caps
Lock stays, and Lay steps the active layout. Given an argument it also
draws the keyboard:

```
cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/kb_keys \
   sw/apps/keyboard/tests/keys.c sw/common/zwin.c sw/common/zobj.c \
   sw/common/zeitlos.c sw/common/zfont_data.c sw/common/zkbd.c
/tmp/kb_keys /tmp/kb        # also writes /tmp/kb-de.pbm, /tmp/kb-fr-altgr.pbm
```

## Not done

- A key shows Latin-9 and the missing-glyph box; its labels are in the
  5x8 font, so a layout's non-Latin symbols (on some AltGr levels) are
  boxes.
- Kana are made by `wm` from romaji after the key is sent, so on the
  Japanese input layouts the keys show romaji, as a Japanese keyboard
  does.
- No key repeat: a pointer has no "held".
