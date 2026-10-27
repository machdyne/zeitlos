# A budget

```lesson
(objective "Build a monthly budget with totals, what is left over, and each item's share of income.")
(minutes 25)
(keywords "budget (of money)")
```

In this lesson you put the course together in a sheet of your own: a
monthly budget.

A **budget** is a plan for money: what comes in each month, what goes
out, and what is left. A sheet suits it, because when one amount
changes, every total follows.

Lay it out with names down column A and amounts in column B:

```figure
(zs-place 50 30 80 18 0 1)
(zs-sheet '(("Income" "2000" "") ("" "" "") ("Rent" "900" "45.0%") ("Food" "300" "15.0%")
            ("Fuel" "120" "6.0%") ("Fun" "180" "9.0%") ("Spent" "1500" "75.0%") ("Left" "500" "")))
(zs-box 1 7 1 8)
(caption "B7 holds =SUM(B3:B6); B8 holds =B1-B7; C3 holds =B3/$B$1.")
```

```check
(choice
  (ask "Which formula in B7 totals the spending in B3:B6?")
  (right "=SUM(B3:B6)")
  (wrong "=SUM(B1:B6)" "That takes in the income in B1 as well, as if it were spent.")
  (wrong "=B3+B6" "That adds only the first and the last item. SUM(B3:B6) adds all four.")
  (hint "Only the spending rows, 3 to 6."))
```

---

## What is left

B8, what is left, is the income take away the spending: `=B1-B7`.
Change any amount, and the total and what is left follow at once.

```figure
(define fun (slider 'fun 0 600 180 "B6 (Fun)" 20))
(define spent (+ 900 300 120 fun))
(zs-bar 28 4 264 "B8" "=B1-B7")
(zs-place 50 40 80 18 0 1)
(zs-sheet (list '("Income" "2000") '("" "") '("Rent" "900") '("Food" "300") '("Fuel" "120")
                (list "Fun" (fmt fun 0)) (list "Spent" (fmt spent 0)) (list "Left" (fmt (- 2000 spent) 0))))
(zs-box 1 8 1 8)
```

```check
(number
  (ask "Income is 2000 and the spending is 900, 300, 120 and 180. Rent goes up to 950. What does B8 show?")
  (answer 450) (from "2000 - (950 + 300 + 120 + 180)")
  (wrong-if 500 "That is what was left before. B7 adds the new rent, so B8 drops by 50.")
  (hint "The spending is now 950 + 300 + 120 + 180."))
```

---

## Each item's share

How much of the income does each item take? C3 holds `=B3/$B$1`, the
rent divided by the income. The `$` pins B1, so the formula can be
filled down from C3 to C7: each row divides its own amount by the same
income.

Then select C3:C7 and give it the format `%1`.

```check
(text
  (ask "C3 holds =B3/$B$1 and is filled down to C6. What formula does C6 hold?")
  (accept "=B6/$B$1") (accept "B6/$B$1")
  (wrong-if "=B6/$B$4" "$B$1 is pinned and does not move; only B3 moves to B6.")
  (wrong-if "=B6/B4" "The $ signs stay with the copy, and keep it on B1.")
  (hint "The relative part moves; the pinned part stays."))
```

---

## Showing money

Select B1:B8 and give it the format `$2`: 2000 shows as `$2,000.00`.
Widen column B with Alt+Right if it shows `####`.

```check
(choice
  (ask "You give B1:B8 the format $2. What does B8, holding 500, show?")
  (right "$500.00")
  (wrong "$5.00" "A format never changes the number. 500 is shown with a dollar sign and 2 places.")
  (wrong "500" "The $2 format adds the dollar sign and 2 places.")
  (hint "A dollar sign, then the number with 2 places."))
```

---

## A picture of the spending

Select A3:B6, the spending and its names, press F5, then P: a pie of
where the money goes. Esc goes back to the grid.

```check
(choice
  (ask "Why select A3:B6 for the pie, and not A1:B8?")
  (right "A1:B8 would put the income, the total and what is left in the pie as if they were spending")
  (wrong "A pie cannot have more than four slices" "A pie can have more. The trouble is what the slices would be: income and totals are not items of spending.")
  (wrong "There is no difference" "Every row of the selection becomes a slice, so the income and the totals would be slices too.")
  (hint "Every row selected becomes a slice."))
```

---

```try
(try (app "sheet")
  (task "In sheet, build the budget from this lesson, with your own items and amounts if you like: income in B1, items in A3:B6, =SUM(B3:B6) in B7, =B1-B7 in B8. Format B1:B8 as $2, and save it as budget.zss. Come back here.")
  (check (choice (ask "Did B8 show what is left, as money?")
    (right "Yes")
    (wrong "No, it shows ####" "Column B is too narrow for the format. Widen it with Alt+Right."))))
```

---

```check
(number
  (ask "In the budget above (income 2000), what share of the income is spent, as a percentage?")
  (answer 75) (unit "%") (from "1500 / 2000 * 100")
  (wrong-if 25 "That is the share left over. Spent is 1500 out of 2000.")
  (hint "Spent divided by income: 1500 / 2000."))
```

A budget is a few totals and differences over a list of amounts, and
the sheet keeps them right whenever an amount changes.
