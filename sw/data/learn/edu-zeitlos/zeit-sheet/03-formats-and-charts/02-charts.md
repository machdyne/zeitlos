# Making a chart

```lesson
(objective "Lay out a table so sheet can chart it, make the chart with F5, and switch its kind.")
(minutes 20)
(keywords "chart (of numbers)" "category (of a chart)" "series (of a chart)" "legend (of a chart)")
```

In this lesson you learn to turn a table of numbers into a picture of
them.

A **chart** shows numbers as shapes: bars, lines or slices, so you see
at a glance which is larger and how they change. Select a table and
press **F5**: `sheet` draws its chart in place of the grid. **Esc** (or
F5 again) goes back to the grid.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Charts")
```

```check
(choice
  (ask "You have selected a table. Which key draws its chart?")
  (right "F5")
  (wrong "F4" "F4 opens a menu of commands, which a later lesson shows. In sheet, F5 is the chart.")
  (wrong "Ctrl+1" "Ctrl+1 sets a number format."))
```

---

## How sheet reads a table

`sheet` reads the selection the way people lay out a table:

- the first column, if it holds text, gives the **categories**: the
  names along the bottom, like months;
- the first row, if it holds text, names the **series**: each other
  column is one series of numbers, up to six;
- the **legend**, at the right, shows which pattern stands for which
  series.

```figure
(zs-place 60 30 64 20 0 1)
(zs-fill 0 2 0 5 'cool)

(zs-sheet '(("Spent" "Food" "Fuel") ("Jan" "300" "120") ("Feb" "260" "90") ("Mar" "280" "150") ("Apr" "310" "60")))
(zs-box 1 1 2 1)
(text 160 156 "dotted: the categories" 'small 'centre)
(text 160 172 "framed: the series' names" 'small 'centre)
(text 160 200 "Select A1:C5, then F5." 'body 'centre)
```

```check
(number
  (ask "You chart A1:D13: months down A2:A13 and three names across B1:D1. How many series does the chart have?")
  (answer 3) (from "4 - 1")
  (wrong-if 12 "The 12 months are the categories. Each column of numbers is a series.")
  (wrong-if 4 "Column A holds the months' names, which are the categories, not a series.")
  (hint "Count the columns that hold numbers."))
```

---

## Four kinds

While the chart shows, a key changes its kind: **B** bars, **S** stacked
bars, **L** lines, **P** a pie. Series are told apart by patterns
(solid, hatching, grey, lines) and, on lines, by marker shapes, so a
chart reads on a one-colour screen and on paper.

```figure
(define k (choice 'kind '("B" "S" "L" "P") "Key"))
(zs-chart k '("Jan" "Feb" "Mar" "Apr") '(300 260 280 310) (if (= k 3) '() '(120 90 150 60)) (if (= k 1) 500 400))
(if (< k 3) (zs-legend k "Food" "Fuel"))
(caption (list-ref '("Bars side by side" "Each month's bars stacked: the total" "Lines, with markers" "A pie: the first series only") k))
```

```check
(choice
  (ask "A chart shows. You press L. What happens?")
  (right "The same numbers are drawn as lines")
  (wrong "A new chart is made from the cursor's cell" "B, S, L and P change only the kind of chart; the numbers stay the same.")
  (wrong "The chart is deleted" "L is for lines. Esc goes back to the grid, and the chart is kept."))
```

---

## A chart keeps up

The chart is saved with the sheet, and a sheet has one chart. **F5** with
nothing selected shows it again, drawn from the numbers as they are now:
change a number and the chart changes with it.

```check
(choice
  (ask "You charted your spending in March, and have since typed April's numbers into the same range. You press F5 with nothing selected. What do you see?")
  (right "The chart, with April's numbers in it")
  (wrong "The chart as it was in March" "The chart is drawn again from the cells each time, so it shows what they hold now.")
  (wrong "A message that there is no chart" "The chart was saved with the sheet. F5 with nothing selected shows it."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type the table in the figure above: Spent, Food, Fuel across A1:C1, Jan to Apr down A2:A5, and the numbers. Select A1:C5 and press F5. Press B, S, L and P in turn, then Esc. Come back here.")
  (check (choice (ask "In the stacked chart, how tall is March's stack?")
    (right "430" (true "280 + 150 = 430"))
    (wrong "280" "That is the Food part alone. A stack puts Fuel on top: 280 + 150.")
    (wrong "150" "That is the Fuel part alone. The stack is both together."))))
```

---

```check
(choice
  (ask "You select only the numbers, B2:C5, with no names, and press F5. What does the chart lack?")
  (right "The month names and the series names")
  (wrong "Nothing: it is the same chart" "The names come from the first column and the first row when they hold text. Without them, the chart has numbers only.")
  (wrong "It cannot be drawn" "sheet charts numbers without names too; it just has none to show."))
```

A chart, made with F5 from a selected table, shows its numbers as bars,
stacks, lines or a pie, and keeps up with them.
