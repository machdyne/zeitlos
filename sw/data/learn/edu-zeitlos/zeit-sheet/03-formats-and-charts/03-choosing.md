# Choosing a chart

```lesson
(objective "Choose the kind of chart that answers a question about a table.")
(minutes 15)
(keywords "bar chart (in sheet)" "stacked bar chart (in sheet)" "line chart (in sheet)" "pie chart (in sheet)")
```

In this lesson you learn which kind of chart to choose for which
question. Each kind answers a different one.

A **bar chart** (B) answers "which is larger?": bars side by side, each
as tall as its number, so differences are easy to see and to compare.

```figure
(zs-chart 0 '("Jan" "Feb" "Mar" "Apr") '(300 260 280 310) '(120 90 150 60) 400)
(zs-legend 0 "Food" "Fuel")
```

```check
(choice
  (ask "You want to compare what four shops charge for the same item. Which chart suits it best?")
  (right "A bar chart")
  (wrong "A pie (P)" "A pie shows parts of one whole. Four prices are not parts of anything; bars compare them directly.")
  (wrong "Lines (L)" "Lines join points in order, which suggests a change over time. The shops have no order.")
  (hint "Which is cheapest? Which is dearest?"))
```

---

## Stacked bars

A **stacked bar chart** (S) puts each category's numbers one on top of
another, so the height of a stack is their total, and each piece shows
its share of it.

```figure
(zs-chart 1 '("Jan" "Feb" "Mar" "Apr") '(300 260 280 310) '(120 90 150 60) 500)
(zs-legend 1 "Food" "Fuel")
```

```check
(number
  (ask "In this stacked chart, Food was 260 and Fuel 90 in February. How tall is February's stack?")
  (answer 350) (from "260 + 90")
  (wrong-if 260 "That is the lower piece alone. The stack is the two pieces together.")
  (hint "A stack's height is the total of its pieces."))
```

---

## Lines

A **line chart** (L) joins each series' points in order. It answers
"how does it change?", so it suits numbers taken over time: a month, a
day or a year a category.

```figure
(zs-chart 2 '("Jan" "Feb" "Mar" "Apr") '(300 260 280 310) '(120 90 150 60) 400)
(zs-legend 2 "Food" "Fuel")
```

```check
(choice
  (ask "You have weighed a puppy every week for 10 weeks. Which chart shows how it is growing?")
  (right "A line chart")
  (wrong "A pie" "The weights are not parts of one whole, and a pie hides their order.")
  (wrong "A stacked bar chart" "Stacking adds the weights together, which means nothing here.")
  (hint "Numbers over time, in order."))
```

---

## Pies

A **pie chart** (P) answers "what share of the whole?": each slice is
its number's share of the total. `sheet` draws a pie from the first
series only, and a negative number counts as nothing in it.

```figure
(zs-chart 3 '("Rent" "Food" "Fuel" "Fun") '(900 300 120 180) '() 1)
```

```check
(number
  (ask "Spending is Rent 900, Food 300, Fuel 120 and Fun 180. What percentage of the pie is Rent?")
  (answer 60) (unit "%") (from "900 / (900 + 300 + 120 + 180) * 100")
  (wrong-if 90 "Rent's share is 900 out of the total, 1500, not out of 1000.")
  (hint "First add all four to find the whole.")
  (hint "900 out of 1500."))
```

---

```check
(choice
  (ask "A table has three series: what you spent on food, fuel and fun each month. You press P. What does the pie show?")
  (right "Only the first series, food, one slice a month")
  (wrong "All three series" "A pie has one whole to cut up. sheet draws it from the first series only.")
  (wrong "The three series' totals" "sheet does not add the series up for a pie; it slices the first series by category."))
```

Bars compare, stacks show totals and their parts, lines show change in
order, and a pie shows shares of one whole.
