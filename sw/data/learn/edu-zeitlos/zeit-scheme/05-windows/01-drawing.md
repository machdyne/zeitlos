# A window to draw in

```lesson
(objective "Open a window from Scheme and draw lines, boxes and text in it by their coordinates.")
(minutes 20)
(keywords "pixel" "drawing area" "coordinates (in a window)")
```

`win-create` opens a window and answers with its number. Keep the
number in a variable, since every drawing procedure needs it:

```
> (define w (win-create "Drawing" 200 120))
w
```

The window is 200 by 120 **pixels**, the dots the screen is made of.
Its frame and title bar take some of that, so its **drawing area**,
the part a program draws in, is 4 pixels narrower and 15 shorter:
196 by 105.

```sources
(source "Zeitlos docs: repl.md, Windows and drawing; zapi.md, Windows and apps")
```

```check
(number
  (ask "You open `(win-create \"Big\" 300 200)`. How wide is its drawing area, in pixels?")
  (answer 296) (from "300 - 4")
  (wrong-if 300 "The frame takes some of the width: the drawing area is 4 pixels narrower.")
  (wrong-if 285 "15 is what the title bar and frame take from the height. The width loses 4.")
  (hint "4 narrower than the window."))
```

---

## Coordinates

A place in the drawing area is given by two numbers, its
**coordinates**: x, how far across from the left edge, and y, how far
down from the top. Both count from 0, so the top left is 0, 0 and in
a 196 by 105 area the bottom right is 195, 104.

Drag the ring to see the coordinates of any place.

```figure
(define p (drag 'p 180 150))
(zs-area-win 196 105 "Drawing")
(zs-line 0 0 195 104)
(zs-box 20 20 60 50 'ink)
(zs-text 70 30 "Hello")
(define px (clamp (round (/ (- (car p) zs-ax) zs-s)) 0 195))
(define py (clamp (round (/ (- (cadr p) zs-ay) zs-s)) 0 104))
(caption (string-append "x " (fmt px 0) ", y " (fmt py 0)))
```

y grows downwards, unlike on a graph in school.

```check
(choice
  (ask "Where is the point x 0, y 104 in that drawing area?")
  (right "The bottom left corner")
  (wrong "The top right corner"
    "x 0 is the left edge, and y 104 is as far down as the area goes.")
  (wrong "The top left corner"
    "The top left is 0, 0. Here y is 104: far down.")
  (hint "x is across from the left; y is down from the top."))
```

---

## Drawing

The window in the figure was drawn with three expressions:

```
(line w 0 0 195 104 1)
(box w 20 20 60 50 1)
(text w 70 30 "Hello" 1)
```

`line` joins two points. `box` fills the rectangle between two
corners, both included. `text` writes words with their top left
corner at the point. The last number is the colour: 1 draws in the
foreground colour, 0 in the background colour, which rubs out.
`(win-clear w)` blanks the whole area.

```check
(number
  (ask "How many pixels wide is the filled rectangle of `(box w 20 20 60 50 1)`?")
  (answer 41) (from "60 - 20 + 1")
  (wrong-if 40 "Both corners are included: from 20 to 60 is 41 pixels, counting 20 and 60.")
  (hint "Count the x positions from 20 to 60, both ends included."))
```

---

## A drawing stays

A window with nothing but drawing in it keeps what was drawn: move it,
cover it and uncover it, and the drawing is still there. To close it
from Scheme, `(win-destroy w)`, or click its close box.

```try
(try (app "repl")
  (task "In repl, open the window with (define w (win-create \"Drawing\" 200 120)), then type the line, box and text expressions above. Then rub out the box with (box w 20 20 60 50 0). Come back here.")
  (check (choice (ask "What did (box w 20 20 60 50 0) do?")
    (right "It filled the box with the background colour, so the box disappeared")
    (wrong "It drew a second box" "Colour 0 is the background colour: drawing in it rubs out.")
    (wrong "Nothing" "It drew: in the background colour, over the box that was there.")
    (hint "0 is the background colour."))))
```

```check
(choice
  (ask "Which draws a line along the very top of a 196 by 105 drawing area?")
  (right "`(line w 0 0 195 0 1)`")
  (wrong "`(line w 0 0 0 104 1)`"
    "That line keeps x at 0 and goes down: it is the left edge.")
  (wrong "`(line w 0 104 195 104 1)`"
    "y 104 is the bottom: that line runs along the bottom edge.")
  (hint "Along the top, y stays 0."))
```

---

## Practice

```check
(number
  (ask "A window made with `(win-create \"Note\" 120 64)`. What is the largest y you can draw at?")
  (answer 48) (from "64 - 15 - 1")
  (wrong-if 49 "The area is 49 pixels high, but y counts from 0: 0 to 48.")
  (wrong-if 63 "The title bar and frame take 15 from the height: 49 pixels, 0 to 48.")
  (hint "64 - 15 = 49 pixels high, counted from 0."))
```

So: a window's drawing area is measured in pixels from its top left
corner, and `line`, `box` and `text` draw in it.
