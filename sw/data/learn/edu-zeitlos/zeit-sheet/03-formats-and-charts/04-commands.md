# The command line

```lesson
(objective "Use sheet's command line to insert and delete rows and columns, sort a table and find a value.")
(minutes 20)
(keywords "command line (everyday word)" "ascending" "descending")
```

In this lesson you learn to give `sheet` commands: to make room in a
table, take rows out, put rows in order and find things.

`sheet` has no drop-down menus. Instead, **/** (or **F4**) turns the
edit bar into the **command line**, a row of commands:

```
/ Insert Delete Number Width Sort Extend Undo Redo Goto Find Chart Help
```

Press a command's first letter (or move with Left and Right and press
Enter). **Esc** goes back a level, and out.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Using it")
(source "Zeitlos docs, function keys (docs/keys.md), The prompt (F4) and the / menu")
```

```check
(choice
  (ask "You are in the middle of typing a formula, =B2/C2, and press /. What happens?")
  (right "A / is typed into the formula")
  (wrong "The command line opens" "/ opens the command line only when you are not typing an entry. While you type, it is the divide sign.")
  (hint "Is / something you type in a formula?"))
```

---

## Inserting and deleting

**/ I** (Insert) asks Rows or Columns, and puts empty ones in before the
cursor: as many as the selection spans. **/ D** (Delete) takes them out.

Formulas follow: after inserting a row above row 5, a formula that used
B5 uses B6, where that cell now is. A formula that used a cell that was
deleted shows `#REF`.

```check
(text
  (ask "B10 holds =SUM(B2:B9). With the cursor in row 4, you insert one row. The total moves down to B11. What formula does it hold now?")
  (accept "=SUM(B2:B10)") (accept "SUM(B2:B10)")
  (wrong-if "=SUM(B2:B9)" "The rows from 4 down moved down one, so the range's end moved too: B9 is now B10. The new row is inside the range.")
  (hint "Every row from 4 down moves down by one, the total's own row too."))
```

---

## Sorting

To put a table in order by one column:

1. select the table's rows, all of their columns (not the headings);
2. end the selection in the column to sort by: the cursor is the cell
   where a selection ends, and its column decides the order;
3. **/ S** (Sort), then **A** for **ascending**, smallest first, or **D**
   for **descending**, largest first.

Each row moves whole. Numbers come before text, and empty cells go last.

```check
(choice
  (ask "A table has names in A2:A9 and scores in B2:B9. You want the highest score at the top. What do you select, and how do you sort?")
  (right "Select A2:B9 with the cursor in column B, then / S D")
  (wrong "Select B2:B9 only, then / S D" "Then only the scores move, and each name is left beside the wrong score. Select every column of the rows.")
  (wrong "Select A2:B9 with the cursor in column B, then / S A" "Ascending puts the smallest first. The highest at the top is descending.")
  (hint "Rows must move whole, and highest first is descending."))
```

---

## Other commands

| Command | Does | Key |
|---|---|---|
| Number | a number format | Ctrl+1 |
| Width | the column's width, typed | Alt+Left, Alt+Right |
| Extend | fill Down or Right | Ctrl+D, Ctrl+R |
| Undo, Redo | | Ctrl+Z, Ctrl+Y |
| Goto | go to a cell | Ctrl+G |
| Find | find text or a number | F3, Ctrl+F |
| Chart | the chart | F5 |
| Help | the help box | F1 |

**F3** finds: the first time it asks what to find; after that, each
press of F3 goes to the next cell that holds it.

```check
(choice
  (ask "You found the first cell holding Smith. How do you go to the next one?")
  (right "Press F3 again")
  (wrong "Press Ctrl+G" "Ctrl+G goes to a cell by its reference. F3 goes to the next match.")
  (wrong "Press / G" "Goto asks for a cell's reference. F3 finds the next match."))
```

---

```try
(try (app "sheet")
  (task "In sheet, type four names down A2:A5 and four scores down B2:B5. Select A2:B5, move the cursor into column B (Shift+Right), and press / S D. Come back here.")
  (check (choice (ask "Which row now holds the highest score?")
    (right "Row 2")
    (wrong "Row 5" "That is where an ascending sort puts it. / S D sorts descending, highest first.")
    (wrong "It has not moved" "Check that the selection covered A2:B5 with the cursor in column B, then press / S D again."))))
```

---

```check
(choice
  (ask "You delete row 3. C9 held =B3*2. What does C8, where it now is, show?")
  (right "#REF")
  (wrong "The old answer" "The cell B3 that the formula used is gone, so its reference becomes #REF.")
  (wrong "0" "sheet does not quietly turn a lost cell into 0; it shows #REF so you notice."))
```

The command line, opened with / or F4, inserts, deletes, sorts and finds;
most of its commands also have a key of their own.
