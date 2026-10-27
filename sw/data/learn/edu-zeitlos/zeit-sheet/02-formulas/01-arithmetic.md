# Formulas that calculate

```lesson
(objective "Write formulas with + - * / and ^, in the right order, from the numbers in other cells.")
(minutes 20)
(keywords "operator (in a formula)" "recalculation")
```

In this lesson you learn to write formulas that do arithmetic with the
numbers in other cells.

A formula starts with `=`. Between its numbers and references go
**operators**, the signs for arithmetic:

| Operator | Means | Example |
|---|---|---|
| + | add | =B2+B3 |
| - | take away | =B2-B3 |
| * | times | =B2*C2 |
| / | divided by | =B2/4 |
| ^ | to the power of | =2^10 |

A computer keyboard has no times or divide sign, so `*` and `/` stand
for them.

```check
(number
  (ask "B2 holds 4 (pens bought) and C2 holds 2.5 (the price of one). What does =B2*C2 show?")
  (answer 10) (from "4 * 2.5")
  (wrong-if 6.5 "That is B2 + C2. The * means times.")
  (hint "* means times: 4 times 2.5."))
```

---

## Which comes first

When a formula has several operators, `sheet` works them out in the
usual order of arithmetic:

1. `^` first;
2. then `*` and `/`, from left to right;
3. then `+` and `-`, from left to right.

So `=2+3*4` is 2 + 12, which is 14.

```check
(number
  (ask "What does =10-4/2 show?")
  (answer 8) (from "10 - 4/2")
  (wrong-if 3 "That works from left to right: 10 - 4 first. But / comes before -, so 4/2 is worked out first.")
  (hint "Divide before you take away.")
  (hint "4/2 is 2. Then 10 - 2."))
```

---

## Brackets

To change the order, use brackets: what is inside them is worked out
first. `=(2+3)*4` is 5 times 4, which is 20.

```example
(example
  (problem "A1, A2 and A3 hold three test marks. Write a formula for their average.")
  (step "The average is the total divided by how many there are: 3.")
  (step "Without brackets, =A1+A2+A3/3 would divide only A3 by 3.")
  (step "So add first, in brackets, then divide.")
  (answer "=(A1+A2+A3)/3"))
```

```check
(number
  (ask "A1 holds 6 and A2 holds 4. What does =(A1+A2)/2 show?")
  (answer 5) (from "(6 + 4)/2")
  (wrong-if 8 "That is A1 + A2/2: without the brackets. The brackets make the adding come first.")
  (hint "Work out the brackets first: 6 + 4."))
```

---

## Change a number, the answer follows

A formula that uses references works with whatever the cells hold now.
Change B2, and every formula that uses B2 shows a new answer at once.
This **recalculation** is the whole point of a sheet: you never redo the
arithmetic yourself.

```figure
(define q (slider 'q 0 12 4 "B2 (pens)" 1))
(zs-bar 28 4 264 "D2" "=B2*C2")
(zs-place 50 56 64 20 0 1)
(zs-sheet (list (list "Item" "Qty" "Price" "Cost")
                (list "pens" (fmt q 0) "2.5" (zs-g (* q 2.5)))))
(zs-box 3 2 3 2)
(caption "Move the slider to change B2. D2 follows.")
```

```check
(number
  (ask "Here B2 is 4, C2 is 2.5 and D2 holds =B2*C2. You type 6 into B2. What does D2 show now?")
  (answer 15) (from "6 * 2.5")
  (wrong-if 10 "That is the old answer. D2 is worked out again as soon as B2 changes.")
  (hint "D2 is always B2 times C2, with what they hold now."))
```

---

## Powers

`^` raises to a power: `=2^10` is 2 multiplied by itself 10 times,
1024. In `sheet` the power must be a whole number. `=4^0.5` gives an
error; the next lesson shows what to use instead.

```check
(number
  (ask "What does =10^3 show?")
  (answer 1000) (from "10^3")
  (wrong-if 30 "That is 10 times 3. ^ multiplies 10 by itself 3 times.")
  (hint "10 times 10 times 10."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type Qty in B1, Price in C1 and Cost in D1. In row 2, type 4 and 2.5 in B2 and C2, and =B2*C2 in D2. Then change B2 to 7. Come back here.")
  (check (number (ask "What does D2 show now?") (answer 17.5) (from "7 * 2.5")
    (wrong-if 10 "That is 4 times 2.5. Check that you typed 7 into B2 and pressed Enter.")
    (hint "7 times 2.5."))))
```

---

```check
(number
  (ask "What does =2*(3+4)^2 show?")
  (answer 98) (from "2*(3+4)^2")
  (wrong-if 196 "That squares 2 times 7. The ^ is done before the *: square the 7, then double it.")
  (wrong-if 22 "That is 2*3 + 4^2: the brackets were left out. First 3 + 4, then square, then times 2.")
  (hint "Brackets first, then ^, then *.")
  (hint "3 + 4 is 7, and 7^2 is 49."))
```

Formulas follow the order of arithmetic, brackets first, and work again
whenever a cell they use changes.
