# When a formula goes wrong

```lesson
(objective "Read sheet's error values, find their cause, and put them right.")
(minutes 15)
(keywords "error value" "circular reference")
```

In this lesson you learn what a cell is telling you when it shows
something starting with `#`, and how to put it right.

When a formula cannot give a right answer, its cell shows an
**error value** instead of a wrong number. The commonest is `#DIV0`: a division
by zero. `=B2/C2` with C2 empty or 0 shows `#DIV0`, because an empty
cell counts as 0 in a formula.

An error spreads: every formula that uses that cell shows it too. So
one mistake shows up wherever it matters, and is not hidden as a 0.

```check
(choice
  (ask "D2 holds =B2/C2 and shows #DIV0. E2 holds =D2*2. What does E2 show?")
  (right "#DIV0")
  (wrong "0" "An error is not turned into 0: it spreads to every formula that uses it, so you see it.")
  (wrong "An empty cell" "E2's formula is still worked out; it uses D2, and D2's error passes on.")
  (hint "Errors spread to the formulas that use them."))
```

---

## The error values

| Shows | Means |
|---|---|
| #DIV0 | a division by zero |
| #NAME | a function sheet does not know |
| #ERR | a formula sheet cannot read, like =2+*3 |
| #REF | a cell the grid does not have, like AA1 |
| #NUM | too large, or a bad argument: =SQRT(-4) |
| #CIRC | a formula that uses its own cell (next step) |

```check
(choice
  (ask "A cell holds =SQRT(B2), and B2 holds -9. What does it show?")
  (right "#NUM")
  (wrong "#DIV0" "Nothing is divided here. A negative number has no square root, so the argument makes no sense: #NUM.")
  (wrong "-3" "-3 times -3 is 9, not -9. No number times itself gives -9.")
  (wrong "#NAME" "SQRT is a function sheet knows. The trouble is its argument.")
  (hint "What number times itself is -9?"))
```

---

## A formula that uses itself

A formula that, step by step, comes back to its own cell is a
**circular reference**, and shows `#CIRC`. The easy way to make one: a
total at the foot of a column that takes in its own cell, like
`=SUM(B2:B9)` typed into B9.

```check
(text
  (ask "B2:B8 holds numbers. You want their total in B9. Which formula do you type into B9?")
  (accept "=SUM(B2:B8)") (accept "SUM(B2:B8)")
  (wrong-if "=SUM(B2:B9)" "That range takes in B9 itself, so the total would have to add itself: #CIRC. Stop at B8.")
  (hint "The range ends at the last number, above the total."))
```

---

## Not an error: ####

A row of `#` signs filling a cell is not an error. The number is too
wide for its column, and `sheet` will not show part of a number, since
part of a number would be a wrong number.

Widen the column with **Alt+Right** (Alt+Left narrows it), or drag the
right edge of its letter at the top.

```check
(choice
  (ask "A cell shows ########. What do you do?")
  (right "Widen its column, with Alt+Right")
  (wrong "Look for a mistake in the formula" "The number is fine; only the column is too narrow to show it whole.")
  (wrong "Press Ctrl+Z" "Nothing went wrong to undo. Widen the column and the number appears."))
```

---

## Heading off #DIV0

When a divisor may be 0, ask first with IF:
`=IF(C2=0,0,B2/C2)` gives 0 when C2 is 0, and divides otherwise.
`sheet` works out only the branch it takes, so the division by zero
never happens.

```check
(number
  (ask "B2 holds 50 and C2 is empty. What does =IF(C2=0,0,B2/C2) show?")
  (answer 0)
  (wrong-if 50 "IF gives its second argument, 0, because the empty C2 counts as 0 and C2=0 is true.")
  (hint "An empty cell counts as 0. Is C2=0 true?"))
```

---

```check
(choice
  (ask "Which of these mistakes gives #NAME?")
  (right "=AVERGE(B2:B9)")
  (wrong "=B2/0" "That divides by zero: #DIV0.")
  (wrong "=SUM(B2:B9) in B9" "A formula that takes in its own cell gives #CIRC.")
  (wrong "=(B2+B3" "A bracket left open makes a formula sheet cannot read: #ERR.")
  (hint "Look for a function name sheet would not know."))
```

An error value names what went wrong, spreads to every formula that
uses it, and is put right where it starts.
