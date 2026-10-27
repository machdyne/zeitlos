# The grid

```lesson
(objective "Name any cell of a sheet by its column and row, and move the cursor to it.")
(minutes 15)
(keywords "spreadsheet (program)" "cell (of a sheet)" "column (of a sheet)" "row (of a sheet)" "cell reference" "cursor (in a sheet)")
```

In this lesson you learn to find your way around a sheet: how each box
in it is named, and how to move from one to another.

`sheet` is the **spreadsheet** of Zeitlos: a program that keeps numbers
and text in a grid, and works things out from them. Open it from the
dock, as you open any app.

The grid is made of boxes called **cells**. Each holds one thing: a
number, some text, or a sum that the sheet works out.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md)")
```

```check
(choice
  (ask "What does one cell of a sheet hold?")
  (right "One thing: a number, some text, or a sum the sheet works out")
  (wrong "A whole table of numbers"
    "A table is many cells side by side. Each cell holds just one entry.")
  (wrong "A file"
    "The whole sheet is kept in a file; a cell is one box in the grid inside it."))
```

---

## Columns and rows

The cells stand in **columns**, up and down, each named by a letter
along the top: A, B, C and so on to Z. They also lie in **rows**,
across, each numbered down the left side: 1, 2, 3 and so on to 999.

So a sheet has 26 columns and 999 rows.

```figure
(zs-place 50 40 64 20 0 1)
(zs-fill 1 1 1 5 'cool)
(zs-fill 0 3 3 3 'grey)
(zs-sheet '(("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "")))
(text 114 160 "column B (dotted)" 'small 'centre)
(text 160 190 "row 3 (grey)" 'small 'centre)
(caption "Letters name the columns; numbers name the rows.")
```

```check
(number
  (ask "A sheet has 26 columns and 999 rows. How many cells is that?")
  (answer 25974) (from "26 * 999")
  (wrong-if 1025 "That is 26 + 999. Every row has 26 cells, so multiply.")
  (hint "Every one of the 999 rows has one cell in each of the 26 columns.")
  (hint "Multiply 26 by 999."))
```

---

## A cell's name

A cell is named by its column's letter, then its row's number: the cell
in column B, row 3 is **B3**. This name is the cell's
**cell reference**. The letter always comes first.

Move the sliders to pick a cell. Its reference shows in the box at the
top, where `sheet` shows it.

```figure
(define c (choice 'col '("A" "B" "C" "D") "Column"))
(define r (slider 'row 1 5 3 "Row" 1))
(zs-bar 28 4 264 (zs-ref c r) "")
(zs-place 50 56 64 20 0 1)
(zs-sheet '(("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "") ("" "" "" "")))
(zs-box c r c r)
```

```check
(text
  (ask "What is the reference of the cell in column D, row 12?")
  (accept "D12")
  (wrong-if "12D" "The column's letter comes first, then the row's number: D12.")
  (hint "Letter first, then number."))
```

---

## The cursor

One cell has a heavy frame round it: the **cursor**. What you type goes
into that cell. `sheet` also shows the cursor's column letter and row
number reversed, white on black, so you can find it at a glance.

Move it with the keys:

| Key | Moves |
|---|---|
| arrows | one cell |
| Tab, Shift+Tab | right, left |
| Enter | down |
| PageUp, PageDown | a screen up or down |
| Home | to column A |
| Ctrl+Home | to A1 |

```check
(choice
  (ask "The cursor is on C5. You press Enter. Where is it now?")
  (right "C6")
  (wrong "D5" "Enter moves down, to the next row. Tab moves right.")
  (wrong "C4" "Rows are numbered downwards, and Enter moves down: the number grows.")
  (hint "Enter moves down one row. Which part of the reference changes?"))
```

---

## Going straight to a cell

**Ctrl+G** asks for a cell (like `B12`) and puts the cursor there. It is
the quickest way to a cell far down the sheet.

```check
(choice
  (ask "You want to reach cell M500. What is quickest?")
  (right "Press Ctrl+G and type M500")
  (wrong "Press Ctrl+Home" "Ctrl+Home goes to A1, the top left corner.")
  (wrong "Press Enter 500 times" "That works for the row, but Ctrl+G gets there at once, column and all."))
```

---

```try
(try (app "sheet")
  (task "Open sheet from the dock. Press Ctrl+G and go to cell Z999, the last cell of the grid. Then press Ctrl+Home. Come back here.")
  (check (text (ask "Which cell is the cursor on after Ctrl+Home?") (accept "A1")
    (wrong-if "Z1" "Ctrl+Home goes to the first column as well as the first row.")
    (hint "The top left corner of the grid."))))
```

---

## Practice

```check
(choice
  (ask "Which of these is a cell reference that sheet has?")
  (right "Z999")
  (wrong "AA1" "sheet has 26 columns, A to Z, each one letter. There is no column AA.")
  (wrong "B1000" "Rows go from 1 to 999.")
  (wrong "0A" "The letter comes first, and rows start at 1."))
```

---

```check
(number
  (ask "How many cells are there from A1 to A9, down column A?")
  (answer 9) (from "9 - 1 + 1")
  (wrong-if 8 "Count both ends: A1, A2 and so on to A9 is nine cells.")
  (hint "Rows 1 to 9."))
```

Each cell has a reference, its column letter and row number, and the
cursor shows where your typing will go.
