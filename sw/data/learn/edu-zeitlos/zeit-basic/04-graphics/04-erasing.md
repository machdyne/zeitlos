# Drawing and erasing

```lesson
(objective "Draw in white or black, erase a shape by drawing it again inverted, and read a pixel with POINT.")
(minutes 20)
(keywords "invert (a pixel)" "function (in BASIC)")
```

In this lesson you rub things out.

`COLOR` sets how the drawing statements that follow it draw:

| Statement | Draws |
|---|---|
| `COLOR 1` | in white (as BASIC starts) |
| `COLOR 0` | in black: over a white shape, it erases |
| `COLOR 2` | inverted: white pixels become black and black ones white |

```basic
10 CLS
20 BOX 100, 80, 220, 160, 1
30 COLOR 0
40 CIRCLE 160, 120, 30
50 COLOR 1
```

draws a white box, then a black circle on it.

```sources
(source "docs/basic_app.md, Beyond BASIC 1")
```

```check
(choice
  (ask "After COLOR 0, a program draws a circle on an empty black screen. What do you see?")
  (right "Nothing: a black circle on black")
  (wrong "A white circle"
    "COLOR 0 draws in black. On a black screen, black on black cannot be seen.")
  (wrong "An error message"
    "Drawing in black is allowed; it just cannot be seen on black.")
  (hint "Which colour does COLOR 0 draw in?"))
```

---

## Inverting twice

To **invert** is to turn it white if it is black, and black
if it is white. Invert it twice and it is back as it was. So a shape
drawn twice with `COLOR 2` is gone, and whatever was under it is
untouched.

```figure
(zb-screen)
(define n (slider 'times 0 3 1 "Boxes" 1))
(define odd (= 1 (modulo n 2)))
(if odd (rect (zb-sx 90) (zb-sy 50) (* zb-k 140) (* zb-k 140) 'paper))
(ring (zb-sx 160) (zb-sy 120) (* zb-k 60) (if odd 'ink 'paper) 1.5)
(text 160 218 "CIRCLE 160, 120, 60: COLOR 2" 'small 'centre)
(text 160 234 (string-append "BOX 90, 50, 230, 190, 1 drawn " (zb-n n)
  (if (= n 1) " time" " times")) 'small 'centre)
```

```check
(choice
  (ask "A white circle is on the screen. With COLOR 2, a program draws a filled box over it, then draws the same box again. What is on the screen?")
  (right "The white circle, as before")
  (wrong "Nothing at all"
    "The second box inverts every pixel back, the circle's too: they return to what they were before the first box.")
  (wrong "A white box"
    "That is after the first box. The second inverts the same pixels back again.")
  (hint "Each box inverts the same pixels. What does inverting twice do?"))
```

---

## Why it matters

Erasing by drawing again is how things move: draw a shape, wait, draw
it again to erase it, and draw it a little further on. With `COLOR 2`
the background behind it survives. You use this in the next unit.

```check
(choice
  (ask "Why erase a moving shape with COLOR 2 rather than with CLS?")
  (right "COLOR 2 removes only the shape, and leaves everything else on the screen")
  (wrong "CLS cannot be used while a program runs"
    "CLS works in a program. But it clears the whole screen, everything else included.")
  (wrong "COLOR 2 is the only way to draw in black"
    "COLOR 0 draws in black too. COLOR 2's point is that drawing the same shape again restores what was under it.")
  (hint "What does CLS clear?"))
```

---

## Reading a pixel

`POINT(x, y)` is a **function**: it gives back a value, here the colour
of the pixel at x, y: 1 for white, 0 for black. A point off the screen
gives 0.

```basic
10 CLS
20 CIRCLE 160, 120, 50
30 PRINT POINT(210, 120); POINT(160, 120)
```

The first point is on the circle's right edge, the second at its
centre, which a circle does not fill. It prints `10`.

```check
(number
  (ask "What does POINT(160, 120) give after CLS and BOX 100, 80, 220, 160, 1?")
  (answer 1)
  (wrong-if 0 "The box is filled, and 160, 120 is inside it: that pixel is white.")
  (hint "Is the point inside the filled box?"))
```

---

```check
(choice
  (ask "A program draws with COLOR 0 and forgets to change back. What happens to the shapes it draws after that?")
  (right "They are drawn in black, so on a black screen they cannot be seen")
  (wrong "They are drawn in white, as BASIC starts"
    "BASIC starts with COLOR 1, but COLOR stays as the program last set it. Write COLOR 1 to draw in white again.")
  (wrong "They are inverted"
    "That is COLOR 2. COLOR 0 draws in black until it is changed.")
  (hint "Does COLOR change back by itself?"))
```

So: `COLOR 1` draws white, `COLOR 0` black, `COLOR 2` inverts so that
drawing twice erases, and `POINT` reads a pixel.
