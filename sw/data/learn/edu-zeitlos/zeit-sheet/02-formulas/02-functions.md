# Functions

```lesson
(objective "Total, average and count a range with SUM, AVERAGE, MIN, MAX and COUNT.")
(minutes 20)
(keywords "function (in a formula)" "argument (of a function)" "square root")
```

In this lesson you learn to work with a whole range at once, with
functions.

Adding forty cells with `+` would be a long formula. A **function** is
a ready-made calculation with a name: `=SUM(B2:B41)` adds every number
in the range B2:B41.

What goes inside the brackets is the function's **argument**: here, a
range. Upper or lower case both work: `=sum(b2:b41)` is the same.

```check
(number
  (ask "B2, B3 and B4 hold 3, 5 and 7. What does =SUM(B2:B4) show?")
  (answer 15) (from "3 + 5 + 7")
  (wrong-if 10 "That is only B2 and B4, the range's two ends. A range is every cell from B2 to B4.")
  (hint "Add every number from B2 down to B4."))
```

---

## Five functions for a range

| Function | Gives |
|---|---|
| SUM | the total |
| AVERAGE (or AVG) | the average |
| MIN | the smallest |
| MAX | the largest |
| COUNT | how many numbers |

```figure
(define k (choice 'fn '("SUM" "AVG" "MIN" "MAX" "COUNT") "Function"))
(define names '("SUM" "AVERAGE" "MIN" "MAX" "COUNT"))
(define answers '("62" "15.5" "9" "21" "4"))
(zs-bar 28 4 264 "B7" (string-append "=" (list-ref names k) "(B2:B5)"))
(zs-place 60 56 64 20 0 1)
(zs-fill 1 2 1 5 'cool)
(zs-sheet (list '("Day" "Km") '("Mon" "12") '("Tue" "9") '("Wed" "21") '("Thu" "20") '("" "")
                (list (list-ref names k) (list-ref answers k))))
(zs-box 1 7 1 7)
```

```check
(number
  (ask "B2:B5 holds 12, 9, 21 and 20. What does =MAX(B2:B5)-MIN(B2:B5) show?")
  (answer 12) (from "21 - 9")
  (wrong-if 3 "That is 12 - 9, the first number take away the smallest. MAX is the largest number in the range, wherever it is.")
  (hint "The largest is 21. The smallest is 9."))
```

---

## Text and empty cells

These functions skip text and empty cells: they do not count them as 0.
So `AVERAGE` gives the average of the numbers that are there, and
`COUNT` says how many numbers there are.

```check
(number
  (ask "B2:B5 holds 10, the text sick, 20 and an empty cell. What does =AVERAGE(B2:B5) show?")
  (answer 15) (from "(10 + 20)/2")
  (wrong-if 7.5 "That divides by 4, as if the text and the empty cell were zeros. AVERAGE skips them: 2 numbers.")
  (wrong-if 10 "That divides by 3. The text cell is skipped too, not counted as 0.")
  (hint "Only 10 and 20 are numbers."))
```

---

## More than one argument

Arguments are separated by commas, and each can be a range, a cell or a
number: `=SUM(B2:B4,D2,100)` adds the range, then D2, then 100.

Some functions take numbers rather than a range: `=SQRT(B2)` is the
**square root** of B2, the number that times itself gives B2 (`=SQRT(9)`
is 3), and `=POWER(B2,3)` is B2 to the power 3.

```check
(number
  (ask "B2:B4 holds 1, 2 and 3, and D2 holds 4. What does =SUM(B2:B4,D2,100) show?")
  (answer 110) (from "1 + 2 + 3 + 4 + 100")
  (wrong-if 10 "That leaves out the last argument, 100. SUM adds all of its arguments.")
  (hint "Add the range, then D2, then 100."))
```

---

## When a name is wrong

A function `sheet` does not know, such as `=SUMM(B2:B4)`, shows `#NAME`
in its cell. Press F2 and put the name right. **F1** in `sheet` lists
every function it has.

```check
(choice
  (ask "A cell shows #NAME. What is most likely wrong with its formula?")
  (right "A function's name is misspelt")
  (wrong "A cell it uses holds text" "Functions like SUM skip text. #NAME means sheet does not know a name in the formula.")
  (wrong "The number is too wide for the column" "A number too wide for its column shows as # signs, not #NAME."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type 12, 9, 21 and 20 down B2 to B5. In B7 type =AVERAGE(B2:B5), and in B8 =COUNT(B2:B5). Come back here.")
  (check (number (ask "What does B7 show?") (answer 15.5) (from "(12 + 9 + 21 + 20)/4")
    (wrong-if 62 "That is the total. AVERAGE divides it by how many numbers there are.")
    (hint "62 divided by 4."))))
```

---

```check
(number
  (ask "B2:B9 holds 5 numbers, 2 text cells and 1 empty cell. What does =COUNT(B2:B9) show?")
  (answer 5)
  (wrong-if 8 "That counts every cell in the range. COUNT counts the numbers only.")
  (wrong-if 7 "The 2 text cells are not counted either.")
  (hint "COUNT skips text and empty cells."))
```

`SUM`, `AVERAGE`, `MIN`, `MAX` and `COUNT` work over a whole range and
skip whatever is not a number.
