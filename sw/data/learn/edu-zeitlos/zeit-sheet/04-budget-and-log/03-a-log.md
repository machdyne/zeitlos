# A log

```lesson
(objective "Keep a log of dated records with a running total, summary figures, sorting and a line chart.")
(minutes 25)
(keywords "log (of records)" "running total (in a sheet)")
```

In this lesson you build a sheet that grows: a log, kept over weeks or
years.

A **log** is a record of things as they happen, one row each: a date,
then what you measured or did. A walking log, a reading log, a meter
reading each month: each new entry goes on the next empty row.

`sheet` has no dates as such, so type a date as text, year first:
`2026-03-07`. Written this way, with two digits for the month and the
day, dates sort into the right order as text.

```check
(choice
  (ask "Which way of writing dates keeps them in order when the log is sorted?")
  (right "2026-03-07, 2026-03-14, 2026-11-02")
  (wrong "7/3/2026, 14/3/2026, 2/11/2026" "Sorted as text, these go by the first digit: 14/3 comes before 2/11 and 7/3, whatever the year or the month.")
  (wrong "Mar 7, Mar 14, Nov 2" "Sorted as text, months go in the order of their letters: Mar before Nov is luck; Apr would come before Jan.")
  (hint "Text sorts by its first letter or digit, then the next."))
```

---

## A running total

A **running total** shows the total so far on each row. Column B holds
each walk in km; C2 holds `=B2`, and C3 holds `=C2+B3`: the total so
far, plus this row. Fill C3 down, and every row adds its own walk to the
row above.

```figure
(define r (slider 'r 3 7 5 "Cursor on row" 1))
(zs-bar 8 4 304 (zs-ref 2 r) (string-append "=C" (fmt (- r 1) 0) "+B" (fmt r 0)))
(zs-place 30 40 90 18 0 1)
(zs-fill 2 (- r 1) 2 (- r 1) 'cool)
(zs-fill 1 r 1 r 'cool)
(zs-sheet '(("Date" "Km" "Total") ("'2026-03-01" "4.5" "4.5") ("'2026-03-03" "6" "10.5")
            ("'2026-03-06" "3" "13.5") ("'2026-03-08" "7.5" "21") ("'2026-03-11" "5" "26") ("'2026-03-13" "4" "30")))
(zs-box 2 r 2 r)
```

```check
(number
  (ask "C4 shows 13.5. B5 holds 7.5, and C5 holds =C4+B5. What does C5 show?")
  (answer 21) (from "13.5 + 7.5")
  (wrong-if 7.5 "That is this row's walk alone. C5 adds it to the total so far in C4.")
  (hint "The total so far, plus this row."))
```

---

## Summing up the log

Below or beside the log, a few formulas sum it up:

| Formula | Gives |
|---|---|
| =COUNT(B2:B200) | how many walks |
| =SUM(B2:B200) | how far in all |
| =AVERAGE(B2:B200) | the average walk |
| =MAX(B2:B200) | the longest |

Take the range further down than the log goes: empty cells are skipped,
so new rows are counted as soon as you type them.

```check
(number
  (ask "The log above has 6 walks: 4.5, 6, 3, 7.5, 5 and 4 km. What does =AVERAGE(B2:B200) show?")
  (answer 5) (from "30 / 6")
  (wrong-if 0.15 "AVERAGE skips the empty cells: it divides by 6 walks, not 199 cells.")
  (hint "The total is 30 km."))
```

---

## Sorting and finding

To see the longest walks first, select the log's rows (A2:C7 here), end
the selection in column B, and **/ S D**. Note that the running total
then no longer runs in date order; sort by column A, **/ S A**, to put
the log back.

**F3** finds a date or a note: it asks what to find, then each F3 goes
to the next match.

```check
(choice
  (ask "You sort the log by km, longest first. Why does column C stop making sense?")
  (right "Each running total adds the row above, which is now a different walk")
  (wrong "Sorting deletes the formulas" "The formulas are still there. But each adds the row above, and the rows are no longer in date order.")
  (wrong "It still makes sense" "A running total adds up in the order of the rows. Out of date order, it is no longer the total up to that date.")
  (hint "What does =C2+B3 add, once the rows are in a new order?"))
```

---

## The log as a line chart

Select the dates and the km, A1:B7, press F5, then L: a line chart of
your walks over time. Each time you add a row, select the log again
with the new row and press F5.

```figure
(zs-chart 2 '("Mar 1" "3" "6" "8" "11" "13") '(4.5 6 3 7.5 5 4) '() 8)
(zs-legend 2 "Km" #f)
```

```check
(choice
  (ask "Why a line chart for this log, rather than a pie?")
  (right "The walks are in date order, and a line shows how they change over time")
  (wrong "A pie cannot show 6 numbers" "It could, but each slice would be a walk's share of the total, which says little. The question is how the walks change.")
  (wrong "A line chart shows the total" "A line chart shows each point in order. The total is in column C.")
  (hint "Which kind shows change over time?"))
```

---

```try
(try (app "sheet")
  (task "In sheet, start a log of your own: dates as text, year first, in column A and a number in column B, at least four rows. Add a running total in C, and =AVERAGE(B2:B200) below. Chart A1:B5 (or more) as lines with F5 and L. Save it as log.zss. Come back here.")
  (check (choice (ask "Did your running total's last row equal =SUM of column B?")
    (right "Yes")
    (wrong "No" "Check that C2 holds =B2, C3 holds =C2+B3, and C3 was filled down to the last row."))))
```

---

```check
(text
  (ask "Your log has a running total in column C, starting at C2. What formula goes in C10?")
  (accept "=C9+B10") (accept "C9+B10") (accept "=B10+C9") (accept "B10+C9")
  (wrong-if "=C10+B10" "That uses its own cell: #CIRC. It should add the total in the row above, C9.")
  (wrong-if "=SUM(B10)" "That is only this row's number. The running total adds the total so far, C9.")
  (hint "The total so far is in the row above."))
```

A log is a row for each record, with dates written year first, a running
total, a few summary formulas and a line chart.
