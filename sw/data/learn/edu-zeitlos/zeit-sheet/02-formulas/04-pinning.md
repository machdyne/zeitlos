# Pinned references

```lesson
(objective "Use $ to keep a reference from moving when its formula is copied.")
(minutes 20)
(keywords "absolute reference" "mixed reference")
```

In this lesson you learn to copy a formula that must keep pointing at
one cell.

Here B1 holds a tax rate, 0.2, and C3 works out the tax on B3 with
`=B3*B1`. Copy it down, and every reference moves, B1 too: C4 gets
`=B4*B2`, and B2 holds the text Price, which counts as 0. C5 gets `=B5*B3`: the
price of pens, not the rate.

Turn on the `$` to see the fix, then move the slider.

```figure
(define pin (toggle 'pin "Pin B1 with $" #f))
(define r (slider 'r 3 5 4 "Copy in row" 1))
(define prices '(10 40 25))
; without the $, row i multiplies by B(i-2): B1 0.2, B2 the text Price (0), B3 10
(define (tax i) (zs-g (* (list-ref prices (- i 3)) (if pin 0.2 (list-ref '(0.2 0 10) (- i 3))))))
(define (row i) (list (list-ref '("pens" "ink" "paper") (- i 3)) (fmt (list-ref prices (- i 3)) 0) (if (<= i r) (tax i) "")))
(zs-bar 28 4 264 (zs-ref 2 r)
  (string-append "=B" (fmt r 0) "*" (if pin "$B$1" (string-append "B" (fmt (- r 2) 0)))))
(zs-place 60 56 64 20 0 1)
(zs-fill 1 (if pin 1 (- r 2)) 1 (if pin 1 (- r 2)) 'cool)
(zs-sheet (list '("Rate" "0.2" "") '("Item" "Price" "Tax") (row 3) (row 4) (row 5)))
(zs-box 2 r 2 r)
```

```check
(choice
  (ask "Without the $, why does the copy in C4 show 0?")
  (right "Its reference to the rate moved down too, to B2, which holds text")
  (wrong "The rate in B1 changed" "B1 still holds 0.2. The copy no longer looks at B1: its reference moved to B2.")
  (wrong "Tax can only be worked out in one row" "The same formula works in every row once the rate's reference is pinned.")
  (hint "Look at the formula in the edit bar for row 4."))
```

---

## The dollar sign

A `$` in a reference pins the part after it. `$B$1` pins both the
column and the row, so it never moves when copied. A reference that
never moves is an **absolute reference**.

So C3 should hold `=B3*$B$1`. Filled down, C4 holds `=B4*$B$1`: the
relative B3 moved, the pinned rate did not.

```check
(text
  (ask "C3 holds =B3*$B$1. You fill it down to C7. What formula does C7 hold?")
  (accept "=B7*$B$1") (accept "B7*$B$1")
  (wrong-if "=B7*$B$5" "The $ in front of the 1 pins the row: $B$1 stays exactly as it is.")
  (wrong-if "=B3*$B$1" "B3 has no $, so it moves down with the copy: to B7.")
  (hint "Only the reference without $ moves."))
```

---

## Pinning one part

A `$` can pin just one part. In `$A1` the column is pinned and the row
moves; in `A$1` the row is pinned and the column moves. A reference with
one part pinned is a **mixed reference**.

```
Copied:            A1    $A1    A$1    $A$1
one row down       A2    $A2    A$1    $A$1
one column right   B1    $A1    B$1    $A$1
```

```check
(text
  (ask "B2 holds =$A2+1. You paste it into C3 (one column right, one row down). What formula does C3 hold?")
  (accept "=$A3+1") (accept "$A3+1")
  (wrong-if "=$B3+1" "The $ in front of A pins the column: it stays A. Only the row moves.")
  (wrong-if "=$A2+1" "The row has no $, so it moves down one: A2 becomes A3.")
  (hint "The column is pinned. What happens to the row?"))
```

---

## A times table

Mixed references make a times table from one formula. Row 1 holds 1 to
5 from B1 across; column A holds 1 to 5 from A2 down. B2 holds
`=$A2*B$1`: always column A's number in its own row, times row 1's
number in its own column. Fill it right and down, and every cell is
right.

```figure
(define c (+ 1 (choice 'c '("B" "C" "D" "E" "F") "Column")))
(define r (slider 'r 2 6 4 "Row" 1))
(zs-bar 8 4 304 (zs-ref c r) (string-append "=$A" (fmt r 0) "*" (zs-letter c) "$1"))
(zs-place 40 56 44 18 0 1)
(zs-fill 0 r 0 r 'cool)
(zs-fill c 1 c 1 'cool)
(zs-sheet (list '("" "1" "2" "3" "4" "5")
  '("1" "1" "2" "3" "4" "5") '("2" "2" "4" "6" "8" "10") '("3" "3" "6" "9" "12" "15")
  '("4" "4" "8" "12" "16" "20") '("5" "5" "10" "15" "20" "25")))
(zs-box c r c r)
```

```check
(text
  (ask "In this table, what formula does D4 hold?")
  (accept "=$A4*D$1") (accept "$A4*D$1")
  (wrong-if "=$A2*B$1" "That is B2's formula. Filled to D4, the relative parts move: the row of $A2 and the column of B$1.")
  (wrong-if "=$D4*D$1" "The $ before A pins the column: every cell uses column A.")
  (hint "Column A of its own row, times row 1 of its own column."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type 1 to 5 across B1:F1 and down A2:A6. In B2 type =$A2*B$1. Select B2:F2 and press Ctrl+R; then select B2:F6 and press Ctrl+D. Come back here.")
  (check (number (ask "What does F6 show?") (answer 25) (from "5 * 5")
    (wrong-if 0 "If some cells show 0, check the $ signs: =$A2*B$1, with the $ before A and before 1.")
    (hint "Row 6 holds 5 in column A, and column F holds 5 in row 1."))))
```

---

```check
(choice
  (ask "A rate is in H1, and every row's formula uses it. How should the formulas refer to it?")
  (right "$H$1")
  (wrong "H1" "H1 would move when the formula is filled down, and point at H2, H3 and so on.")
  (wrong "H$1" "That works when filling down, since the row is pinned; but copied across, the column would move. $H$1 is safe both ways.")
  (hint "The reference must not move, down or across."))
```

A `$` pins the part of a reference after it, so a copied formula can
keep pointing at one cell, one column or one row.
