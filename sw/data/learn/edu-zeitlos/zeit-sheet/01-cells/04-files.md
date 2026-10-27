# Keeping a sheet

```lesson
(objective "Save a sheet to a file, open it again, and know what a .zss file and a CSV file keep.")
(minutes 15)
(keywords "CSV (file)")
```

In this lesson you learn to keep your work: to save a sheet in a file
and open it again.

**Ctrl+S** saves. The first time, `sheet` asks for a name: it suggests
`sheet.zss` and starts in the folder `/home`. Give the file a name that
says what it holds, like `accounts.zss`. After that, Ctrl+S saves to the
same file without asking.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Using it; The file format")
```

```check
(choice
  (ask "You saved accounts.zss yesterday and changed a cell today. What does Ctrl+S do?")
  (right "Saves to accounts.zss again, without asking")
  (wrong "Asks for a new name" "It asks only the first time, while the sheet has no file yet.")
  (wrong "Makes a second file beside the first" "It writes the changes into the same file, accounts.zss."))
```

---

## Opening and starting afresh

**Ctrl+O** opens a sheet from a file, and **Ctrl+N** starts a new, empty
one. If the sheet you have open holds changes not yet saved, `sheet`
asks first whether to save them.

You can also open a sheet from `files`: open the file, and it opens in
`sheet`.

```check
(choice
  (ask "Which keys start a new, empty sheet?")
  (right "Ctrl+N")
  (wrong "Ctrl+O" "Ctrl+O opens a sheet you saved before. N is for new.")
  (wrong "Ctrl+S" "Ctrl+S saves the sheet you have open."))
```

---

## What a .zss file holds

A `.zss` file is plain text, which `text` can open too: a line for each cell that is not empty, its
reference, a space, then exactly what you typed. Formulas are kept as
formulas.

```
zsheet 1
A1 Item
B1 Cost
A2 pens
B2 12.5
A3 paper
B3 8
B4 =B2+B3
```

Empty cells take no room at all, so a sheet with a few numbers in it
makes a small file.

```check
(choice
  (ask "In this file, which line keeps the total in B4?")
  (right "B4 =B2+B3")
  (wrong "B4 20.5" "A .zss file keeps what you typed, and you typed the formula. The answer, 20.5, is worked out again when the sheet opens.")
  (hint "The file keeps exactly what you typed into each cell."))
```

---

## CSV

A **CSV** file ("comma-separated values") is a table as plain text, a
row a line, with commas between the cells. Most spreadsheets and many
other programs read and write it, so it is how tables travel between
machines. A `.csv` file opens in `sheet`.

But a CSV file keeps only values: a sheet saved as CSV keeps the
**answers** of its formulas, not the formulas. Keep your own work as
`.zss`.

```check
(choice
  (ask "You save a sheet whose B4 holds =B2+B3 as a CSV file. What does the file keep for B4?")
  (right "The answer, such as 20.5")
  (wrong "The formula =B2+B3" "A CSV file keeps values only. That is why your own work is best kept as .zss, which keeps formulas.")
  (wrong "Nothing" "B4 is not empty: the file keeps its answer, but not its formula."))
```

---

```try
(try (app "sheet")
  (task "In sheet, with the small table from the last lessons (or a few numbers of your own), press Ctrl+S and save it as first.zss in /home. Press Ctrl+N, then Ctrl+O, and open first.zss again. Come back here.")
  (check (choice (ask "Was your table back, formulas and all?")
    (right "Yes")
    (wrong "No" "Check that you pressed Ctrl+S and gave the name before Ctrl+N. If sheet asked whether to save, the file was not saved yet."))))
```

---

```check
(choice
  (ask "You want to send a table of numbers to someone whose computer does not run Zeitlos. Which kind of file is most likely to open there?")
  (right "A .csv file")
  (wrong "A .zss file" "The .zss format is sheet's own. A CSV file is read by most spreadsheets.")
  (hint "Which format do most programs that handle tables read?"))
```

Ctrl+S keeps a sheet in a `.zss` file, formulas and all; a CSV file
keeps values only, for sharing.
