# Cells and zoom

```lesson
(objective "Open pix, find its parts, make a new sprite sheet, and zoom in and out.")
(minutes 15)
(keywords "sprite" "sprite sheet" "cell (in pix)" "zoom (in pix)")
```

`pix` is for small pictures, made one pixel at a time: the dock's
icons, the mouse pointer, fonts, and the art of games. It is in the
dock's **Tools** group.

A **sprite** is a small picture that a game moves about the screen: a
player, a coin. A **sprite sheet** is a file of many sprites, each with
a name.

Started on its own, pix makes a new sprite sheet with one sprite of 16
by 16 pixels.

```sources
(source "pix (docs/pix_app.md)")
```

```check
(choice
  (ask "A game has a mouse that runs, in 4 pictures. Where would the 4 pictures be kept?")
  (right "In one sprite sheet, as 4 sprites")
  (wrong "In one sprite, 4 times as large" "Each picture is its own sprite. A sprite sheet holds them together in one file.")
  (wrong "In 4 slide decks" "slide makes cards. The art of a game is kept in a sprite sheet, made in pix.")
  (hint "A sheet holds many sprites."))
```

---

## pix's screen

pix's screen is 320 by 240, in a window or on the whole screen.

- **The canvas**, the big part at the top left, shows the picture you
  are drawing, zoomed. Each picture in a file is a **cell**:
  a sprite, an icon, or a letter of a font.
- **The side column**, at the right: the tools, the colours, and the
  New, Open and Save buttons.
- **The preview**, at the bottom left, shows the cell at its real size.
- **The strip**, beside it, shows all the cells; click one to draw in it.
- **The status line**, at the foot.

```check
(choice
  (ask "You want to see how your sprite looks at its real size. Where do you look?")
  (right "The preview, at the bottom left")
  (wrong "The canvas" "The canvas is zoomed, so each pixel is a big square. The preview shows the cell at its own size.")
  (wrong "The side column" "The side column has the tools and colours.")
  (hint "Which part shows the cell one pixel to one pixel?"))
```

---

## Zoom

**Zoom** is how many times bigger the canvas draws each pixel:
from 1 to 16 times. Press **+** and **-**, or turn the wheel. From 6
times up, a grid shows where each pixel ends.

```figure
(define z (slider 'zoom 1 16 6 "zoom" 1))
(define face '("..####.." ".#....#." "#.#..#.#" "#......#" "#.#..#.#" "#..##..#" ".#....#." "..####.."))
(mk-pixels face 20 30 z)
(mk-pixels face 260 40 1)
(text 264 64 "1:1" 'centre 'small)
(text 20 22 (string-append "canvas, " (fmt z 0) " times") 'small)
(caption "An 8 by 8 sprite on the canvas, and at its real size")
```

```check
(number
  (ask "A sprite is 16 pixels wide. At a zoom of 8, how many screen pixels wide is it on the canvas?")
  (answer 128) (from "16 * 8")
  (wrong-if 24 "That adds the zoom. Each pixel is drawn 8 times as wide, so multiply.")
  (wrong-if 2 "That divides. Zooming in makes each pixel bigger, not smaller.")
  (hint "Each of the 16 pixels is drawn 8 wide."))
```

---

## Moving between cells

**PageUp** and **PageDown** go to the cell before and after; **Home** and
**End** to the first and last. **Ins** makes a new cell and asks for its
name. **F2** renames the cell you are on; **F3** goes to a cell by name.

The status line shows which cell you are on, its name and its size,
such as `3/49 term  32x32`.

```check
(text
  (ask "The status line says 2/5 coin 16x16. What is the name of the cell you are drawing in?")
  (accept "coin")
  (wrong-if "2" "2 is its place among the 5 cells. Its name comes next.")
  (hint "After the numbers comes the cell's name, then its size."))
```

---

## Something new

**Ctrl+N** asks what to make, in a few words:

| Words | Makes |
|---|---|
| sprite 16 16 | a sprite sheet with one 16 by 16 sprite |
| icon | a set of icons, one 32 by 32 icon |
| font 6 12 | a font, with the letter A, 6 by 12 |
| picture 320 240 | a black-and-white picture, draw's kind |
| picture 320 240 16 | a picture in 16 colours |

```check
(choice
  (ask "You want a sheet of 32 by 32 sprites. What do you type after Ctrl+N?")
  (right "sprite 32 32")
  (wrong "icon" "An icon is 32 by 32 too, but it is for the dock, not a game.")
  (wrong "picture 32 32" "A picture is a single cell. A sheet of sprites is sprite.")
  (hint "The kind first, then the width and height."))
```

---

```try
(try (app "pix")
  (task "Open the Tools group in the dock and start pix. Press + three times, then - once. Read the status line. Then come back.")
  (check (choice
    (ask "Did the sprite on the canvas get bigger or smaller, all told?")
    (right "Bigger")
    (wrong "Smaller" "+ zooms in and - zooms out. Three steps in and one out leave it bigger than it was.")
    (wrong "It stayed the same" "Three steps in and one out leave it one or more steps bigger."))))
```

So: pix draws cells, such as the sprites of a sheet, zoomed on the
canvas, with the real size in the preview; + and - zoom, and PageUp and
PageDown go between cells.
