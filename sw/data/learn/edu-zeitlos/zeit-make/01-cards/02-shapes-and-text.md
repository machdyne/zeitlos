# Shapes and words

```lesson
(objective "Draw shapes and text boxes on a card, then select, move, resize and delete them.")
(minutes 20)
(keywords "text box (in slide)" "selection (in slide)" "handle (of a selection)")
```

To put something on a card, pick a tool by its letter, then drag on
the card:

- **t** text, **r** rectangle, **u** rounded rectangle, **o** oval;
- **l** line, **a** arrow, **p** polygon, **f** freehand.

Hold **Shift** while you drag to keep a rectangle square, an oval round
or a line at 45 degrees.

```figure
(mk-slide-window 3 "")
(frame 90 66 110 70 'ink 2)
(caption "r is chosen: drag on the card for a rectangle")
```

```sources
(source "slide (docs/slide_app.md), Using it")
```

```check
(choice
  (ask "You want a perfect circle. What do you do?")
  (right "Press o, then hold Shift while you drag")
  (wrong "Press c, then drag" "No tool has the letter c. The oval tool is o; Shift keeps it round.")
  (wrong "Press o and drag carefully" "A drag by hand is almost never exactly round. Shift makes it so.")
  (hint "Circles are ovals with equal sides. Which key keeps sides equal while you drag?"))
```

---

## Text

With **t**, drag out a box, or just click for one of the usual size,
and type straight away. A box that holds words is a **text box**.

Later, to change its words: double-click the text box, or select it
and press **Enter**. **Esc** ends the typing.

While you type in a text box, letters go into the box, not to the
tools.

```check
(choice
  (ask "You are typing in a text box and press r. What happens?")
  (right "An r is typed in the box")
  (wrong "The rectangle tool is picked"
    "While you type in a text box, every letter is text. Press Esc first to end the typing.")
  (wrong "The box becomes a rectangle"
    "The letter is just typed. Tools are picked only when you are not typing.")
  (hint "What does Esc do while you type in a box?"))
```

---

## A polygon

With **p**, click once for each corner. Press **Enter**, or
double-click, to finish. If you end on the first corner, the shape is
closed.

**f**, the freehand tool, draws where the pointer goes, smoothed into
a curve.

```check
(number
  (ask "You draw a triangle with the polygon tool, ending on the first corner. How many clicks is that, counting the last?")
  (answer 4) (from "3 + 1")
  (wrong-if 3 "Three clicks make the three corners. To close it, the last click goes back on the first corner: one more.")
  (hint "One click a corner, then one more on the first corner."))
```

---

## Choosing things

Pick the select tool, **v**. Then:

- a click on an object selects it; **Shift+click** adds another (a
  Shift+click on one already selected takes it out again);
- a drag on an empty part of the card selects all inside it.

What you have selected is the **selection**. It shows small
squares at its edges, its **handles**: drag one to
change the size. Drag the selection itself to move it, or press the
arrow keys to move it a little (with Shift, ten times as far). **Delete** removes it.

```sources
(source "slide (docs/slide_app.md), Using it")
```

```check
(choice
  (ask "A rectangle is selected. You want it wider but in the same place. What do you drag?")
  (right "A handle on its left or right side")
  (wrong "The middle of the rectangle" "Dragging the middle moves the whole thing. A handle changes its size.")
  (wrong "An empty part of the card" "That draws a band to select things, and leaves the rectangle as it was.")
  (hint "The small squares at the edges of a selection are for its size."))
```

---

## Undo

**Ctrl+Z** undoes the last change, up to 64 changes back, and
**Ctrl+Y** does it again. **Ctrl+D** makes a copy of the selection.

```example
(example
  (problem "You delete a text box by mistake.")
  (step "Press Ctrl+Z once: the text box comes back.")
  (step "Changed your mind? Ctrl+Y deletes it again.")
  (answer "Ctrl+Z undoes, and Ctrl+Y redoes."))
```

```check
(choice
  (ask "You made three changes and want to go back to before all of them. What do you press?")
  (right "Ctrl+Z three times")
  (wrong "Ctrl+Y three times" "Ctrl+Y redoes what was undone. Going back is Ctrl+Z.")
  (wrong "Ctrl+Z once" "Each Ctrl+Z undoes one change, so three changes need three.")
  (hint "One Ctrl+Z, one change."))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Press r and drag a rectangle; press o and Shift+drag a circle. Press v, click the rectangle, then Shift+click the circle, and press Delete. Then come back.")
  (check (choice
    (ask "How many of the two shapes you drew are left on the card?")
    (right "None")
    (wrong "One" "The click selected the rectangle and the Shift+click added the circle, so both were selected, and Delete removes all of a selection.")
    (wrong "Two" "Delete removes what is selected. Were both selected?"))))
```

So: a letter picks a tool, a drag draws, v selects, the handles resize,
Delete removes, and Ctrl+Z undoes.
