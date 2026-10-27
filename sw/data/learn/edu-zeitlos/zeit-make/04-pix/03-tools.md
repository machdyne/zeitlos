# Tools

```lesson
(objective "Draw with pix's pencil, line, rectangle, fill and pick, and move, flip and turn a selection.")
(minutes 20)
(keywords "flood fill")
```

pix's tools have a letter each:

| Key | Tool | |
|---|---|---|
| P | pencil | a pixel at a time; a drag draws a joined line |
| L | line | press, drag, let go |
| R | rectangle | R again: outline or filled |
| G | fill | fills an area (below) |
| K | pick | takes the colour under it as the colour to draw |
| S | select | drag a rectangle round part of the cell |

You can also click a tool in the side column. Whatever the tool, the
left button uses the first colour and the right button the second.

```sources
(source "pix (docs/pix_app.md), Tools")
```

```check
(choice
  (ask "A sprite has a shade of blue you want to use again, but you do not know which number it is. Which tool helps?")
  (right "K, pick: click the blue, and it becomes the colour you draw with")
  (wrong "G, fill" "Fill paints an area. Pick takes a colour from the art.")
  (wrong "S, select" "Select marks part of the cell to move. Pick takes a colour.")
  (hint "Which tool takes a colour from what is drawn?"))
```

---

## Fill

The fill tool, **G**, is a **flood fill**: it paints the pixel you click
and every pixel of the same colour joined to it, side by side, up to
whatever colour is different. Pixels that touch only at a corner are
not joined, so a line that steps from corner to corner still holds the
fill in.

```figure
(define gap (toggle 'gap "gap"))
(define fill (toggle 'fill "fill (G)"))
(define (wall x y)
  (and (not (and gap (= x 1) (= y 4)))
       (or (and (= y 8) (>= x 1) (<= x 8))
           (and (= x 1) (>= y 1) (<= y 8))
           (and (= x y) (>= x 1) (<= x 8)))))
(define (inside x y) (and (>= x 2) (<= x (- y 1)) (>= y 3) (<= y 7)))
(for-range 0 10
  (lambda (y)
    (for-range 0 10
      (lambda (x)
        (let ((px (+ 70 (* x 18))) (py (+ 20 (* y 18))))
          (cond ((wall x y) (rect px py 18 18 'ink))
                ((and fill (or gap (inside x y))) (rect px py 18 18 'cool)))
          (frame px py 18 18 'grey 0.5))))))
(ring (+ 70 (* 2 18) 9) (+ 20 (* 6 18) 9) 6 'ink 2)
(caption "The ring: where G is clicked")
```

```check
(choice
  (ask "Tick gap in the figure, then fill. Why does the fill run out over the whole cell?")
  (right "Through the gap, the inside is joined side by side to the outside")
  (wrong "Fill always fills the whole cell" "Without the gap it stays inside. It fills only what is joined to where you click.")
  (wrong "The outline is the wrong colour" "The outline is the same; a single missing pixel lets the fill through.")
  (hint "Compare the figure with and without the gap."))
```

---

## Selections

With **S**, drag a rectangle round part of the cell: that is the
selection. Drag inside it to move it, or move it with the arrow keys;
**Enter**, or a click outside it, puts it down.

| Key | Does to the selection |
|---|---|
| H | flips it across (left to right) |
| V | flips it down (top to bottom) |
| T | turns it a quarter, clockwise |
| Del | clears it |
| Ctrl+C, Ctrl+X, Ctrl+V | copy, cut, paste |

With nothing selected, H, V and T work on the whole cell.

```check
(choice
  (ask "Your sprite of a car faces right. You want one that faces left. What do you press, with nothing selected?")
  (right "H, to flip it across")
  (wrong "V, to flip it down" "That turns it upside down. Across, left to right, is H.")
  (wrong "T, to turn it" "A quarter turn makes it face down. Two turns would turn it upside down too.")
  (hint "Facing left is the mirror of facing right, side to side."))
```

---

## Without a mouse, and undo

The arrow keys move a cursor on the grid, and **Space** does what a left
click would there: once for a pixel, twice (start, then end) for a line,
a rectangle or a selection. **Esc** drops a line or rectangle under way.

**Ctrl+Z** undoes and **Ctrl+Y** redoes, up to 64 steps.

```check
(number
  (ask "With the line tool and the keyboard, how many times do you press Space to draw one line?")
  (answer 2)
  (wrong-if 1 "One Space starts the line. A second, after moving the cursor, ends it.")
  (hint "Once at the start, once at the end."))
```

---

```try
(try (app "pix")
  (task "Open pix from the Tools group. Press R twice for a filled rectangle and draw one. Press G and click outside the rectangle with the left button. Then press Ctrl+Z. Then come back.")
  (check (choice
    (ask "What did Ctrl+Z take back?")
    (right "The fill: the rectangle is still there")
    (wrong "The rectangle" "Ctrl+Z undoes the last change first, which was the fill.")
    (wrong "Both the fill and the rectangle" "One Ctrl+Z undoes one step: the last, the fill."))))
```

So: P, L, R, G, K and S are pix's tools; fill floods what is joined side
by side; a selection moves, flips with H and V and turns with T; Ctrl+Z
undoes.
