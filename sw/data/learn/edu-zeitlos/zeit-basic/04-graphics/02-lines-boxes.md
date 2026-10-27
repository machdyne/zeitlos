# Lines and boxes

```lesson
(objective "Draw lines, boxes and filled boxes from their corner coordinates.")
(minutes 20)
```

In this lesson you draw shapes with one statement each.

- `LINE x1, y1, x2, y2` draws a straight line from the point x1, y1 to
  the point x2, y2.
- `BOX x1, y1, x2, y2` draws a rectangle with corners at x1, y1 and
  x2, y2.
- `BOX x1, y1, x2, y2, 1` fills it.

Drag the two points, and choose LINE, BOX or a filled BOX.

```figure
(zb-screen)
(define a (drag 'a 80 50))
(define b (drag 'b 220 150))
(define kind (choice 'kind '("LINE" "BOX" "filled") "Draw"))
(define x1 (zb-px (car a)))
(define y1 (zb-py (car (cdr a))))
(define x2 (zb-px (car b)))
(define y2 (zb-py (car (cdr b))))
(define l (min (zb-sx x1) (zb-sx x2)))
(define t (min (zb-sy y1) (zb-sy y2)))
(define w (max 1 (abs (- (zb-sx x2) (zb-sx x1)))))
(define h (max 1 (abs (- (zb-sy y2) (zb-sy y1)))))
(if (= kind 0) (line (zb-sx x1) (zb-sy y1) (zb-sx x2) (zb-sy y2) 'paper 1))
(if (= kind 1) (frame l t w h 'paper 1))
(if (= kind 2) (rect l t w h 'paper))
(text 160 226
  (string-append (if (= kind 0) "LINE " "BOX ") (zb-n x1) ", " (zb-n y1) ", "
                 (zb-n x2) ", " (zb-n y2) (if (= kind 2) ", 1" ""))
  'body 'centre)
```

```sources
(source "docs/basic_app.md, Beyond BASIC 1")
```

```check
(choice
  (ask "Which statement draws a filled rectangle?")
  (right "BOX 10, 10, 100, 50, 1")
  (wrong "BOX 10, 10, 100, 50"
    "Without the fifth number BOX draws only the outline. A 1 at the end fills it.")
  (wrong "LINE 10, 10, 100, 50, 1"
    "LINE draws a line, and takes four numbers. A filled rectangle is BOX with a 1 at the end.")
  (hint "The shape is a BOX. What makes it filled?"))
```

---

## Corners

A box needs only two opposite corners: the top left and the bottom
right, or the other way round. BASIC works out the other two.

```check
(choice
  (ask "BOX 50, 20, 150, 80 draws a box. Where is its bottom left corner?")
  (right "50, 80")
  (wrong "150, 20"
    "150, 20 is the top right: x = 150 is the right side and y = 20 the top.")
  (wrong "50, 20"
    "50, 20 is the top left, one of the two corners given. y = 20 is the top, since y counts down.")
  (hint "Left is the smaller x, bottom is the larger y."))
```

---

## A house

```basic
10 CLS
20 BOX 100, 120, 220, 200
30 LINE 100, 120, 160, 70
40 LINE 160, 70, 220, 120
50 BOX 145, 160, 175, 200, 1
```

Line 20 is the walls, lines 30 and 40 the roof, and line 50 a filled
door.

```check
(number
  (ask "In the house above, how many pixels higher than the top of the walls is the peak of the roof?")
  (answer 50) (from "120 - 70")
  (wrong-if 70 "70 is the peak's y coordinate, counted from the top of the screen. The walls start at y = 120.")
  (hint "The walls' top is at y = 120 and the peak at y = 70."))
```

---

```try
(try (app "basic")
  (task "In BASIC, type NEW, then the house program, then RUN. Add a window with a line 60 BOX ... of your own, and RUN again. Come back.")
  (check (choice
    (ask "Where must the window's coordinates be, to be on the walls?")
    (right "x between 100 and 220, y between 120 and 200")
    (wrong "x between 120 and 200, y between 100 and 220"
      "That swaps x and y. The walls run across from x = 100 to 220 and down from y = 120 to 200.")
    (hint "Look at line 20: BOX 100, 120, 220, 200."))))
```

---

## Lines from a loop

With a loop, many lines make a pattern:

```basic
10 CLS
20 FOR X = 0 TO 320 STEP 32
30 LINE 160, 239, X, 0
40 NEXT X
```

All the lines start at the bottom middle and fan out to the top edge.

```check
(number
  (ask "How many lines does this program draw?")
  (answer 11) (from "320 / 32 + 1")
  (wrong-if 10 "X = 0 is a line too: 0, 32, 64 ... 320 is 11 values.")
  (hint "Count the values of X: 0, 32, 64, and so on up to 320."))
```

---

```check
(choice
  (ask "Which statement draws a line straight down the middle of the screen, from top to bottom?")
  (right "LINE 160, 0, 160, 239")
  (wrong "LINE 0, 120, 319, 120"
    "y stays at 120 while x goes from 0 to 319: that line runs across, not down.")
  (wrong "LINE 0, 0, 319, 239"
    "That runs from the top left corner to the bottom right: corner to corner.")
  (hint "Going straight down, x stays the same while y changes."))
```

So: `LINE` joins two points, `BOX` draws a rectangle from two opposite
corners, and a 1 at the end of `BOX` fills it.
