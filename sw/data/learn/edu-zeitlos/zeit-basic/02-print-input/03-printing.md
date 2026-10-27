# Printing

```lesson
(objective "Print numbers and text together, in columns, and at a chosen place on the screen.")
(minutes 20)
(keywords "print zone (in BASIC)")
```

In this lesson you control exactly what `PRINT` puts on the screen.

`PRINT` takes a list of items: numbers, expressions and text in quotes.
A **semicolon** between two items prints the second straight after the
first, with no space:

```basic
10 A = 6
20 PRINT "A IS "; A; " AND A * A IS "; A * A
```

prints `A IS 6 AND A * A IS 36`. The spaces come from inside the
quotes. BASIC adds none of its own: `PRINT 1; 2; 3` prints `123`.

Text can only be printed; it cannot be stored. Variables hold numbers
only.

```check
(choice
  (ask "What does 10 PRINT \"SUM\"; 2 + 2 print?")
  (right "SUM4")
  (wrong "SUM 4"
    "A semicolon adds no space. To get one, put it inside the quotes: \"SUM \".")
  (wrong "SUM2 + 2"
    "2 + 2 is outside the quotes, so it is an expression: BASIC works it out and prints 4.")
  (hint "BASIC adds nothing between the items, and calculates what is not in quotes."))
```

---

## Ending a line, or not

After its last item, `PRINT` moves to the start of the next line. A
semicolon at the very end stops that: the next `PRINT` carries on the
same line. `PRINT` alone prints an empty line.

```basic
10 PRINT "ON ";
20 PRINT "ONE LINE"
30 PRINT
40 PRINT "AFTER A GAP"
```

prints `ON ONE LINE`, an empty line, then `AFTER A GAP`.

```basic
10 PRINT "A";
20 PRINT "B";
30 PRINT "C";
```

```check
(number
  (ask "How many lines of the screen does this program print on?")
  (answer 1)
  (explain "The semicolons at the ends keep everything on one line: ABC.")
  (wrong-if 3 "Each PRINT that ends with a semicolon stays on its line, so the next one carries on: ABC on one line.")
  (hint "What does a semicolon at the end of a PRINT do?"))
```

---

## Columns

A **comma** moves to the next **print zone**: the screen's
40 columns are cut into zones 14 columns wide, starting at columns 0,
14 and 28. Commas line up tables:

```basic
10 PRINT "N", "N * N", "N * N * N"
20 PRINT 5, 5 * 5, 5 * 5 * 5
```

```text
N             N * N         N * N * N
5             25            125
```

```sources
(source "Machdyne BASIC 1 reference (sw/ext/basic/docs/basic1.md), section 5, PRINT")
```

```check
(number
  (ask "10 PRINT \"A\", \"B\" prints A in column 0. In which column is B, counting the first column as 0?")
  (answer 14)
  (wrong-if 2 "A comma is not a single space: it moves on to the start of the next print zone.")
  (wrong-if 1 "A semicolon would put B in column 1. A comma moves on to the next zone.")
  (hint "The zones start at columns 0, 14 and 28."))
```

---

## Clearing and placing

The screen holds 30 rows of 40 characters. Two statements give you
the whole of it:

- `CLS` clears the screen, and the next text starts at the top left.
- `LOCATE row, column` makes the next text start there. Rows are
  numbered 0 to 29 from the top, columns 0 to 39 from the left.

```basic
10 CLS
20 LOCATE 15, 15
30 PRINT "CENTRE"
```

prints CENTRE half way down, near the middle.

When text reaches the bottom row, everything on the screen moves up to
make room.

```sources
(source "docs/basic_app.md, The screen; Beyond BASIC 1")
```

```check
(choice
  (ask "Which line makes the next PRINT start in the top left corner of the screen, without clearing it?")
  (right "10 LOCATE 0, 0")
  (wrong "10 LOCATE 1, 1"
    "Rows and columns are counted from 0, so 1, 1 is one in from the corner each way.")
  (wrong "10 LOCATE 29, 39"
    "29, 39 is the last row and the last column: the bottom right corner.")
  (wrong "10 CLS"
    "CLS does put the next text in the top left, but it also clears the whole screen.")
  (hint "The first row and the first column are both numbered 0."))
```

---

```check
(choice
  (ask "What does 10 PRINT \"X = \"; -5; 3 print?")
  (right "X = -53")
  (wrong "X = -5 3"
    "BASIC adds no spaces between items joined by semicolons, so -5 and 3 run together.")
  (wrong "X = -2"
    "-5; 3 is two items, not a sum: a semicolon joins them without adding.")
  (hint "Print each item in turn, with nothing between them."))
```

---

```check
(number
  (ask "How many spaces does 10 PRINT 1, 2 print between the 1 and the 2?")
  (answer 13) (from "14 - 1")
  (wrong-if 14 "The 2 starts at column 14, but the 1 takes column 0. The spaces fill columns 1 to 13.")
  (wrong-if 1 "A comma does not print one space: it moves to the next print zone.")
  (hint "The 1 is in column 0 and the 2 in column 14. Count the columns in between."))
```

So: semicolons join, commas move to the next zone, a final semicolon
stays on the line, and `CLS` and `LOCATE` choose where text goes.
