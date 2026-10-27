# Copying formulas

```lesson
(objective "Copy a formula down a column or across a row, and predict how its references move.")
(minutes 20)
(keywords "relative reference" "fill down")
```

In this lesson you learn to write a formula once and copy it to every
row that needs it.

**Ctrl+C** copies the selection, and **Ctrl+V** pastes it at the
cursor. When what you copy holds a formula, its references move with
it: `=B2*C2` in D2, pasted into D3, becomes `=B3*C3`. It moved down one
row, and so did its references.

A reference that moves like this is a **relative reference**: it means
"the cell so many columns and rows from here", not one fixed cell.

```figure
(define r (slider 'r 2 6 3 "Paste into row" 1))
(define rows '(("pens" "4" "2.5") ("paper" "2" "6") ("glue" "3" "1.2") ("tape" "5" "2") ("ink" "1" "9")))
(define (row i) (let ((x (list-ref rows (- i 2))))
  (append x (list (if (<= i r) (zs-g (* (string->number (cadr x)) (string->number (caddr x)))) "")))))
(zs-bar 28 4 264 (zs-ref 3 r) (string-append "=B" (fmt r 0) "*C" (fmt r 0)))
(zs-place 50 56 64 20 0 1)
(zs-fill 1 r 2 r 'cool)
(zs-sheet (list '("Item" "Qty" "Price" "Cost") (row 2) (row 3) (row 4) (row 5) (row 6)))
(zs-box 3 r 3 r)
(caption "Each copy multiplies B and C of its own row.")
```

```check
(text
  (ask "D2 holds =B2*C2. You copy D2 and paste it into D5. What formula does D5 hold?")
  (accept "=B5*C5") (accept "B5*C5")
  (wrong-if "=B2*C2" "The references move with the formula. D5 is 3 rows below D2, so B2 becomes B5 and C2 becomes C5.")
  (wrong-if "=B3*C3" "That is the copy one row down, in D3. D5 is 3 rows down.")
  (hint "D5 is 3 rows below D2. Move every reference down 3 rows."))
```

---

## Across, too

Pasted to the right, a formula's column letters move instead: `=A1+1`
in B1, pasted into C1, becomes `=B1+1`. Pasted down and right at once,
both move.

```check
(text
  (ask "B1 holds =A1*2. You copy it and paste it into D1. What formula does D1 hold?")
  (accept "=C1*2") (accept "C1*2")
  (wrong-if "=A1*2" "The reference moves with the formula: D1 is 2 columns right of B1, so A1 becomes C1.")
  (wrong-if "=A3*2" "Pasting to the right moves the column letters, not the row numbers.")
  (hint "D is 2 columns right of B. Move A1 2 columns right."))
```

---

## Filling down

To copy one row's formula into many rows, select from the formula down
to the last row, then press **Ctrl+D**. That is **fill down**: the
first row of the selection is copied into every row below it in the
selection, each copy with its references moved. **Ctrl+R** fills right,
from the first column.

```example
(example
  (problem "D2 holds =B2*C2. Fill it down to row 30.")
  (step "Put the cursor on D2.")
  (step "Hold Shift and press Down (or PageDown) until the selection reaches D30.")
  (step "Press Ctrl+D. D3 now holds =B3*C3, and so on down to D30, which holds =B30*C30.")
  (answer "One formula, typed once, in 29 rows."))
```

```check
(number
  (ask "You select D2:D30 with a formula in D2 and press Ctrl+D. Into how many cells is the formula copied?")
  (answer 28) (from "30 - 2")
  (wrong-if 29 "D2 already holds the formula; it is copied into D3 to D30. Count those.")
  (hint "The cells below D2, down to D30."))
```

---

## Pasting into a larger selection

Ctrl+V pastes into a selection, too. If the selection is a whole
number of copies of what you copied, every copy is pasted: copy D2,
select D3:D9, press Ctrl+V, and each of the 7 cells gets the formula,
moved to its own row.

```check
(choice
  (ask "You copy D2 (=B2*C2), select D3:D6 and press Ctrl+V. What does D6 hold?")
  (right "=B6*C6")
  (wrong "=B3*C3" "Each copy is moved to its own row. D6 is 4 rows below D2.")
  (wrong "Nothing: only D3 is pasted" "A selection a whole number of copies in size gets a copy in every place: here, all 4 cells."))
```

---

```try
(try (app "sheet")
  (task "In sheet, make a little table: Qty in B, Price in C, for 4 rows (B2:C5), and =B2*C2 in D2. Select D2:D5 and press Ctrl+D. Move to D5 and look at the edit bar. Come back here.")
  (check (text (ask "What formula does D5 hold?") (accept "=B5*C5") (accept "B5*C5")
    (wrong-if "=B2*C2" "If D5 shows this, it was not filled. Select D2:D5, D2 first, then Ctrl+D.")
    (hint "Its references point at its own row."))))
```

---

```check
(text
  (ask "E3 holds =SUM(B3:D3). You fill it down to E4. What formula does E4 hold?")
  (accept "=SUM(B4:D4)") (accept "SUM(B4:D4)") (accept "=sum(b4:d4)")
  (wrong-if "=SUM(B3:D3)" "A range moves like any other reference: both of its corners go down one row.")
  (hint "Both corners of the range move down one row."))
```

Relative references move with a copied formula, so one formula, filled
down or pasted, serves every row.
