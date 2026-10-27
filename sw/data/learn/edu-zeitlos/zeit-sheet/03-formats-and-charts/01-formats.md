# Number formats

```lesson
(objective "Show numbers as money, with thousands commas, as percentages or with a fixed number of places, and set column widths.")
(minutes 20)
(keywords "number format" "decimal places (of a number)")
```

In this lesson you learn to choose how numbers look, without changing
what they are.

Select some cells and press **Ctrl+1**. `sheet` asks for a
**number format**: how to show the numbers in the selection. Its box
already holds the format the cell has now (`G` at first): erase that
with Backspace, then type one of these and press Enter:

```
Type   Shows 1234.5 as
G      1234.5 (general, the default: as short as it can be)
F2     1234.50
,2     1,234.50
$2     $1,234.50
%1     123450.0%
```

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Number formats")
```

```check
(choice
  (ask "You want 2500 to show as $2,500.00. Which format do you type?")
  (right "$2")
  (wrong ",2" "That gives the commas and the 2 places, 2,500.00, but no dollar sign.")
  (wrong "F2" "F2 gives 2 places, 2500.00, but no dollar sign or commas.")
  (hint "The format's first sign is what it adds in front."))
```

---

## The digit

The digit after the letter or sign is the number of **decimal places**:
how many digits are shown after the point, from 0 to 8. `$0` shows
whole dollars; `F3` shows three places. Left out, it is 2.

Choose a format and see how three numbers look in it.

```figure
(define k (choice 'f '("G" "F2" ",2" "$2" "%1") "Format"))
(define shown '(("1234.5" "0.25" "-3") ("1234.50" "0.25" "-3.00") ("1,234.50" "0.25" "-3.00")
                ("$1,234.50" "$0.25" "-$3.00") ("123450.0%" "25.0%" "-300.0%")))
(define s (list-ref shown k))
(zs-place 40 50 84 20 0 1)
(zs-sheet (list (list "Holds" "Shows")
                (list "'1234.5" (car s)) (list "'0.25" (cadr s)) (list "'-3" (caddr s))))
(caption "Column A holds the numbers as typed; B shows them in the format.")
```

```check
(text
  (ask "What does 0.4 look like in the format %0?")
  (accept "40%")
  (wrong-if "0.4%" "% multiplies by 100 to show a percentage: 0.4 is 40%.")
  (wrong-if "40.0%" "The 0 after % asks for no places after the point.")
  (hint "A percentage is the number times 100."))
```

---

## A format changes the look, not the number

A format only changes how a number is shown. The cell keeps the whole
number, and formulas use the whole number.

So with the format `F0`, two cells holding 2.6 each show 3, but their
sum is 5.2, shown as 5. It looks like 3 + 3 = 5; it is right.

```check
(number
  (ask "A1 holds 1.4 and A2 holds 1.4, both shown with F0. A3 holds =A1+A2, shown with F1. What does A3 show?")
  (answer 2.8) (from "1.4 + 1.4")
  (wrong-if 2 "That adds the numbers as they look, 1 + 1. Formulas use what the cells hold: 1.4 each.")
  (hint "The format of A1 and A2 does not change what they hold."))
```

---

## Too wide

A number that does not fit its column in its format shows as `####`.
Make the column wider:

Widths are counted in places: one place holds one letter, digit, sign or
space.

- **Alt+Right** widens the cursor's column by one place, and
  **Alt+Left** narrows it;
- or drag the right edge of the column's letter with the mouse.

A column starts 9 places wide.

```check
(number
  (ask "How many places wide must a column be to show $12,345.67 whole?")
  (answer 10)
  (wrong-if 9 "Count everything shown: the $, the comma and the point take a place each too.")
  (hint "Count $ 1 2 , 3 4 5 . 6 7."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type 1234.5 in A1. With the cursor on A1, press Ctrl+1, press Backspace to erase the G, type $2 and press Enter. Then press Ctrl+1 again, erase the $2, type %0 and press Enter. Come back here.")
  (check (text (ask "What does A1 show now?") (accept "123450%")
    (wrong-if "####" "The column is too narrow. Widen it with Alt+Right.")
    (wrong-if "$1,234.50" "That was the first format. The second, %0, replaced it.")
    (hint "1234.5 times 100, with no places."))))
```

---

```check
(choice
  (ask "You type 0.075 into a cell and give it the format %1. What does a formula =A1*200 give?")
  (right "15" (true "0.075*200 = 15"))
  (wrong "1500" "The cell shows 7.5%, but holds 0.075. The formula uses 0.075: 0.075 times 200.")
  (wrong "7.5" "A format does not change the number; 0.075 times 200 is 15.")
  (hint "The format shows 7.5%, but the cell holds 0.075."))
```

A number format, chosen with Ctrl+1, changes how numbers look, never
what they are.
