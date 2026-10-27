# Blocks of cells

```lesson
(objective "Name a block of cells, select it, and read its total, average and count from the edit bar.")
(minutes 15)
(keywords "range (of cells)" "selection (in a sheet)" "average (of numbers)")
```

In this lesson you learn to work with a block of cells at once, and to
total it without writing a formula.

A block of cells is a **range**. It is named by its top left cell and
its bottom right cell with a colon between them: `B2:D4` is the block
from B2 to D4, three columns across and three rows down.

Move the sliders to change the range's bottom right corner.

```figure
(define c (choice 'col '("B" "C" "D") "To column"))
(define r (slider 'row 2 5 4 "To row" 1))
(zs-bar 28 4 264 (string-append "B2:" (zs-ref (+ c 1) r)) "")
(zs-place 50 56 64 20 0 1)
(zs-fill 1 2 (+ c 1) r 'cool)
(zs-sheet '(("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "")))
(zs-box 1 2 (+ c 1) r)
(text 160 190 (string-append (fmt (* (+ c 1) (- r 1)) 0) " cells") 'body 'centre)
```

```check
(number
  (ask "How many cells are in the range A1:C4?")
  (answer 12) (from "3 * 4")
  (wrong-if 7 "That is 3 + 4. The range is 3 columns (A, B, C) by 4 rows: multiply.")
  (wrong-if 2 "A range names its two corners, but it holds every cell between them.")
  (hint "How many columns from A to C? How many rows from 1 to 4?")
  (hint "3 columns and 4 rows."))
```

---

## Selecting

To pick out a range, hold **Shift** and press the arrows, or drag across
it with the mouse. The cells picked are the **selection**: they are
shaded, and the reference box on the edit bar shows the range, like
`B3:C5`.

The cell where the cursor is stays plain inside the shading, so you can
still see where typing would go.

```check
(text
  (ask "The cursor is on A1. You hold Shift and press Down twice. What does the reference box show?")
  (accept "A1:A3")
  (wrong-if "A1:A2" "Each press of Down adds one row: A2, then A3.")
  (wrong-if "A3" "With Shift held, the arrows stretch the selection from A1, so the box shows a range.")
  (hint "The range starts at A1 and goes down two rows."))
```

---

## A total without a formula

When a range is selected, the edit bar shows the **Sum** of its
numbers, their **Avg** and their **Count**. The **average** of some
numbers is their total divided by how many there are.

Text and empty cells are left out: they are not counted as 0. Stretch
the selection and watch.

```figure
(define r (slider 'row 2 6 6 "Select to row" 1))
(define vals '(12 8 #f 17 #f))
(define (stats l i s n)
  (if (or (null? l) (> i r)) (list s n)
      (if (car l) (stats (cdr l) (+ i 1) (+ s (car l)) (+ n 1)) (stats (cdr l) (+ i 1) s n))))
(define st (stats vals 2 0 0))
(define s (car st))
(define n (cadr st))
(zs-bar 8 4 304 (string-append "B2:" (zs-ref 1 r))
  (if (= n 0) "" (string-append "Sum " (fmt s 0) "  Avg " (fmt (/ s n) 2) "  Count " (fmt n 0))))
(zs-place 50 56 64 20 0 1)
(zs-fill 1 2 1 r 'cool)
(zs-sheet '(("Day" "Km" "") ("Mon" "12" "") ("Tue" "8" "") ("Wed" "rest" "") ("Thu" "17" "") ("Fri" "" "")))
(zs-box 1 2 1 r)
```

```check
(number
  (ask "B2:B6 holds 12, 8, the text rest, 17 and an empty cell. What Avg does the edit bar show?")
  (answer 12.33) (within 0.01) (from "(12 + 8 + 17) / 3")
  (wrong-if 7.4 "That divides by 5, as if the text and the empty cell were zeros. Only the 3 numbers are counted.")
  (wrong-if 9.25 "That divides by 4. The text cell is left out, not counted as 0.")
  (hint "Add the numbers only, then divide by how many numbers there are.")
  (hint "The numbers are 12, 8 and 17."))
```

---

## Doing things to a range

Much of what you do to one cell, you can do to a whole selection.
**Delete** or **Backspace** empties every cell in it; **Ctrl+Z** brings
them all back.

Later lessons copy, format and sort selections in the same way.

```check
(choice
  (ask "You select B2:B20 and press Delete. What happens?")
  (right "All 19 cells are emptied, and Ctrl+Z would bring them back")
  (wrong "Only B2 is emptied" "Delete empties the whole selection, not just the cursor's cell.")
  (wrong "The rows are taken out of the sheet" "Delete empties the cells; the rows stay where they are. Taking rows out is a separate command.")
  (hint "Delete acts on everything selected."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type 12, 8, rest and 17 down B2 to B5. Select B2:B5 with Shift and the arrows, and look at the edit bar. Come back here.")
  (check (number (ask "What Count does the edit bar show?") (answer 3)
    (wrong-if 4 "The text rest is not a number, so it is not counted.")
    (hint "Count the numbers only."))))
```

---

```check
(number
  (ask "A selection holds 5, 10, an empty cell and 15. What Sum does the edit bar show?")
  (answer 30) (from "5 + 10 + 15")
  (hint "The empty cell adds nothing."))
```

A range is a block named by two corners, and selecting it shows its
sum, average and count at once.
