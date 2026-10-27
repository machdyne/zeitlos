# The page and the brushes

```lesson
(objective "Paint in draw with the pencil, the brush and the eraser, and move around its page.")
(minutes 15)
(keywords "pixel")
```

`draw` paints in black and white, in the manner of the first painting
programs. It is in the dock.

A picture on a screen is made of tiny squares in rows, each one colour:
**pixels**. draw's page is 640 pixels across and 480 down, each black or
white.

There is no colour, so patterns, as in slide, do the work colours
would. draw has
16 of them, along the bottom of its window.

```sources
(source "draw (docs/draw_app.md)")
```

```check
(number
  (ask "draw's page is 640 pixels across and 480 down. How many pixels is that?")
  (answer 307200) (from "640 * 480")
  (wrong-if 1120 "That adds the sides. The pixels fill a rectangle: 480 rows of 640 each.")
  (hint "480 rows, each of 640 pixels."))
```

---

## The window shows part of the page

The whole page does not fit in the window beside the tools and the
patterns, so the window shows part of it. The **scrollbars** at the
right and the foot of the page, or the mouse wheel, move the part you
see.

```figure
(define sx (slider 'x 0 240 0 "across" 10))
(define sy (slider 'y 0 180 0 "down" 10))
(define k 0.4)
(rect 32 20 256 192 'paper)
(frame 32 20 256 192 'ink 1)
(circle 100 80 26 'grey)
(rect 190 130 60 50 'cool)
(path '(60 190 120 150 180 196 260 120) 'ink 2)
(frame (+ 32 (* k sx)) (+ 20 (* k sy)) (* k 400) (* k 300) 'ink 2.5)
(caption "The page; the box is the part in the window")
```

```check
(choice
  (ask "You drew at the bottom right of the page, and now you cannot see it. Is it lost?")
  (right "No: scroll right and down to see it")
  (wrong "Yes: draw keeps only what is in the window" "draw keeps the whole page. The window shows part of it; the scrollbars move that part.")
  (wrong "Make the window larger, or it is lost" "A larger window shows more, but the drawing is there either way.")
  (hint "Move the box in the figure. Does the page change?"))
```

---

## The tools

Down the left are seven tools: the pencil, the brush and the eraser,
then four shapes (the next lesson). Click one, then drag on the page.

- The **pencil** draws a line one pixel wide, in black. If you start a
  stroke on a black pixel, it draws white instead, which is how to clear
  a single pixel.
- The **brush** is wider and round, and paints with the pattern chosen
  along the bottom.
- The **eraser** is a square that makes everything under it white.

```sources
(source "draw (docs/draw_app.md), Drawing; draw's help (F1)")
```

```check
(choice
  (ask "You start a pencil stroke on a black pixel and drag across white. What colour does the pencil draw?")
  (right "White, for the whole stroke")
  (wrong "Black, as always" "The pencil draws white when the stroke starts on black.")
  (wrong "It changes at each pixel it crosses" "The colour is chosen where the stroke starts, and kept for the whole stroke.")
  (hint "Where did the stroke start?"))
```

---

## No undo

draw has **no undo**. A stroke, once made, stays, unless you paint over
it or rub it out. Save often (the last lesson of this unit), and save
a copy under another name before a big change.

```check
(choice
  (ask "You brush across a drawing by mistake. What can you do in draw?")
  (right "Rub it out with the eraser, or paint over it; or open the last saved copy")
  (wrong "Press Ctrl+Z" "draw has no undo. Many apps do, but not this one.")
  (wrong "Nothing at all" "The eraser and painting over it can fix it, and a saved copy can be opened again.")
  (hint "What does the help say about undo?"))
```

---

```try
(try (app "draw")
  (task "Open draw from the dock. Pick the brush and a pattern along the bottom, and paint a little. Then pick the eraser and rub part of it out. Press F1 to read draw's help, then come back.")
  (check (choice
    (ask "What does draw's help say about undo?")
    (right "There is no undo")
    (wrong "Ctrl+Z undoes" "draw's help ends: There is no undo.")
    (wrong "Esc undoes" "Esc in draw stops a file loading or saving. There is no undo."))))
```

So: draw paints black and white on a 640 by 480 page; the brush paints
with a pattern, the pencil a pixel at a time; and there is no undo.
