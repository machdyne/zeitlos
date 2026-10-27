# The palette and pix's files

```lesson
(objective "Change a colour of the palette, save pix's kinds of file, and change an icon of the dock.")
(minutes 20)
(keywords "RGB")
```

**F8** opens the palette over the canvas: all the colours as a grid, and
the fields of the first colour.

A colour is made of three amounts of light: red, green and blue, each
from 0 to 15. This way of writing a colour is **RGB**. All three at 0 is
black; all three at 15 is white.

In the palette, **Up** and **Down** choose a field, and **Left** and
**Right** change it (with Shift, by 4). F8 or Esc closes it.

```sources
(source "pix (docs/pix_app.md), Colour: the palette (F8)")
```

```check
(choice
  (ask "A colour has Red 15, Green 15 and Blue 0. What colour is it?")
  (right "Yellow")
  (wrong "White" "White needs blue at 15 too. Red and green light without blue make yellow.")
  (wrong "Blue" "Blue is at 0, so there is no blue in it at all.")
  (hint "Red light and green light together, with no blue."))
```

---

## Fewer colours, worked out for you

Changing a colour's RGB also works out what stands in for it with
fewer colours (what F7 shows). A field you set by hand is marked `!`
and kept; **D** puts it back to what pix works out.

```check
(choice
  (ask "You change Red, Green and Blue of colour 5. What happens to its stand-in at 4 colours?")
  (right "pix works it out again, unless you set it by hand")
  (wrong "Nothing: stand-ins never change" "Changing RGB works out every field not set by hand.")
  (wrong "It is deleted" "Every colour keeps a stand-in. pix works out a new one.")
  (hint "What does the ! mark on a field mean?"))
```

---

## Saving

**Ctrl+S** saves; the first time, pix asks for a name. **Ctrl+Shift+S**
saves under another name. The file's name is at the right end of the
status line, with a `*` while there are changes not saved.

The kind of file is what you opened or made, and its name should end
the same way:

| Ending | Holds |
|---|---|
| .zsp | a sprite sheet, with its animations; the mouse pointer too |
| .zic | icons, such as the dock's |
| .zfn | a font |
| .zbm | a picture: draw's kind, or in colour |

Opening a PNG, GIF, BMP or JPEG picture makes a colour picture of it,
which saves as a `.zbm`.

```check
(text
  (ask "You made a sprite sheet for a game called rocket. What is a good name for its file?")
  (accept "rocket.zsp")
  (wrong-if "rocket.zbm" "A .zbm is a single picture. A sprite sheet is a .zsp.")
  (wrong-if "rocket" "Give it the ending of a sprite sheet, so files knows to open it in pix.")
  (hint "Sprite sheets end in .zsp."))
```

---

## The dock's icons

The dock's icons are in `/data/wm/dock.zic`. Open it in pix (Ctrl+O),
and each icon is a 32 by 32 cell, named after its app.

That file itself is never written over. Saving it writes only the icons
you changed to `/data/wm/icons.zic`, and the dock lays those over its
own. The dock shows them after the configuration is read again:
**Reload file** in `settings`. To have all the first icons back, delete
`icons.zic`.

```sources
(source "pix (docs/pix_app.md), Files: .zic, the dock's icons")
(source "The dock's files (docs/dock.md)")
```

```check
(choice
  (ask "You redrew the icon for calc and saved. Later you want the old one back. What do you do?")
  (right "Delete /data/wm/icons.zic, and reload")
  (wrong "Nothing can bring it back: dock.zic was changed" "dock.zic is never written over. Your icon is in icons.zic.")
  (wrong "Press Ctrl+Z in the dock" "The dock has no undo. Your icons are kept in icons.zic, which you can delete.")
  (hint "Where did pix save your icon?"))
```

---

```try
(try (app "pix")
  (task "Open pix from the Tools group. Press F8, and with Up and Down find the Red field of the first colour. Press Right a few times and watch the colour change. Press Esc. Then come back.")
  (check (choice
    (ask "Which keys changed the amount of red?")
    (right "Left and Right")
    (wrong "Up and Down" "Up and Down choose which field. Left and Right change it.")
    (wrong "0 to 9" "In the palette, Left and Right change a field; with Shift, by 4."))))
```

So: F8 opens the palette, where each colour is red, green and blue from
0 to 15; Ctrl+S saves a `.zsp`, `.zic`, `.zfn` or `.zbm`; and the dock's
icons you change are kept apart in `icons.zic`.
