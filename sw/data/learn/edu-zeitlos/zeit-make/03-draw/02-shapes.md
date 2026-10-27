# Lines, boxes and ovals

```lesson
(objective "Draw lines, rectangles, filled rectangles and ovals in draw, and choose their pattern.")
(minutes 15)
```

After the pencil, brush and eraser come draw's four shape tools:

- the **line**;
- the **rectangle**, an outline;
- the **filled rectangle**, filled with the chosen pattern;
- the **oval**, an outline.

For each, press where the shape starts and drag to where it ends.
While you drag, draw shows an outline of where the shape will go (for
the oval, the box it will fill). When you let go, the shape is painted.

```sources
(source "draw (docs/draw_app.md), Drawing; draw's help (F1)")
```

```check
(choice
  (ask "You are dragging out an oval, and you see a rectangle. What is wrong?")
  (right "Nothing: while you drag, an oval shows as the box it will fill")
  (wrong "The rectangle tool is picked" "That may be so, but the oval tool also shows a box while you drag. Let go and see.")
  (wrong "draw cannot draw ovals" "It can: the oval is drawn when you let go.")
  (hint "What does draw show while you drag an oval?"))
```

---

## Patterns and filled rectangles

The pattern chosen along the bottom is used by two tools: the brush, and
the filled rectangle. A filled rectangle also gets a solid black
outline, so even a light pattern has a clear edge.

The line, the rectangle and the oval are always solid.

```figure
(define k (choice 'pat '("solid" "dots" "lines") "pattern"))
(define role (list-ref '(ink cool earth) k))
(rect 40 50 110 80 role)
(frame 40 50 110 80 'ink 2)
(text 95 150 "filled rectangle" 'centre 'small)
(frame 180 50 110 80 'ink 2)
(text 235 150 "rectangle" 'centre 'small)
(caption "The pattern fills only the filled rectangle.")
```

```check
(choice
  (ask "You pick a dotted pattern and draw an oval. What is inside it?")
  (right "Nothing: the oval is an outline")
  (wrong "The dotted pattern" "Only the brush and the filled rectangle use the pattern. The oval is an outline.")
  (wrong "Solid black" "The oval is only an outline. There is no filled oval in draw.")
  (hint "Which tools use the pattern?"))
```

---

## A filled oval?

draw has no fill tool, and no filled oval. To fill an oval, paint
inside it with the brush, or build the shape from filled rectangles.
Planning matters, since there is no undo.

```example
(example
  (problem "A grey square with a black outline, and a dark circle beside it.")
  (step "Pick a grey-looking pattern, then the filled rectangle tool, and drag a square: it gets its black outline by itself.")
  (step "Pick the oval tool and drag the circle's box.")
  (step (choice (ask "How do you make the circle dark inside?")
          (right "Pick a dark pattern and paint inside it with the brush")
          (wrong "Click inside it with a fill tool" "draw has no fill tool. The brush paints with the pattern.")))
  (answer "The filled rectangle outlines itself; the circle is filled by brush."))
```

---

```check
(choice
  (ask "Which of draw's tools use the pattern chosen along the bottom? Choose all that do.")
  (several)
  (right "The brush")
  (right "The filled rectangle")
  (wrong "The pencil" "The pencil draws single pixels, black or white.")
  (wrong "The oval" "The oval is an outline, always solid.")
  (hint "Two tools: one paints freely, one makes a box."))
```

---

```try
(try (app "draw")
  (task "Open draw from the dock. Pick a light pattern and the filled rectangle tool, and drag a box. Then draw an oval beside it. Then come back.")
  (check (choice
    (ask "What edge does the filled rectangle have?")
    (right "A solid black outline")
    (wrong "No edge: just the pattern" "draw outlines every filled rectangle in solid black, so a light pattern has an edge.")
    (wrong "A dotted outline in the pattern" "The outline is solid black, whatever the pattern."))))
```

So: draw's shapes are the line, the rectangle, the filled rectangle and
the oval; only the brush and the filled rectangle use the pattern.
