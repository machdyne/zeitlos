# Typing into cells

```lesson
(objective "Enter numbers, text and a first formula, and correct or undo an entry.")
(minutes 20)
(keywords "formula (in a sheet)" "edit bar" "undo (a change)")
```

In this lesson you learn to put numbers and text into cells, and to put
right what you typed.

To fill a cell, move the cursor to it and type. Press **Enter** to keep
what you typed and move down; **Tab** keeps it and moves right.

Typing into a cell that already holds something **replaces** it: the
first key starts a fresh entry.

```check
(choice
  (ask "B2 holds 40. With the cursor on B2, you type 55 and press Enter. What does B2 hold now?")
  (right "55")
  (wrong "4055" "Typing does not add to what is there. The first key starts a fresh entry, which replaces 40.")
  (wrong "95" "sheet does not add the old number and the new one. The new entry replaces the old.")
  (hint "The first key you press starts a fresh entry."))
```

---

## Numbers and text

`sheet` looks at what you typed and decides, by one rule, what it is:

- it starts with `=`: a sum to work out (next step);
- it starts with `'`: text;
- the whole of it is a number, like `12.5` or `-3`: a number;
- anything else: text.

Numbers sit at the right of their cell, text at the left. So `1,250`,
`$12` and `3 apples` are all **text**: none of them is a number from
end to end. Type `1250` and `12`; a later lesson shows them with commas
and dollar signs.

```figure
(zs-place 50 40 64 20 0 1)
(zs-sheet '(("Item" "Cost" "") ("pens" "12.5" "") ("paper" "8" "") ("'$12" "'1,250" "-3")))
(caption "Numbers to the right; text, like $12 and 1,250, to the left.")
```

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), What a cell holds")
```

```check
(choice
  (ask "Which of these entries does sheet take as a number?")
  (right "-4.75")
  (wrong "$12" "The dollar sign is not part of a number, so this is text. Type 12; a later lesson shows how to show it as money.")
  (wrong "1,250" "A comma is not part of a number here, so this is text. Type 1250.")
  (wrong "12%" "The per cent sign makes it text. Type 0.12; a later lesson shows how to show it as a percentage.")
  (hint "Digits, at most one point, and a minus sign at the front: nothing else makes a number."))
```

---

## Text that looks like a number

Sometimes you want text that looks like a number: a code like `007`,
which as a number would be just 7. Start it with an apostrophe, `'007`.
The apostrophe makes it text and is not shown in the cell.

To type text that starts with `/`, start it with `'` too: on its own,
`/` opens a menu of commands, which a later lesson explains.

```check
(text
  (ask "What do you type to keep the code 0042 as text, with its zeros?")
  (accept "'0042")
  (wrong-if "0042" "That is a number from end to end, so sheet keeps it as 42. Start it with an apostrophe.")
  (wrong-if "\"0042\"" "Quotes do not make text here; they would be part of the entry. Start it with an apostrophe.")
  (hint "One mark at the front makes anything text."))
```

---

## A first formula

An entry that starts with `=` is a **formula**: a sum that `sheet` works
out. `=B2+B3` adds the numbers in B2 and B3, and the cell shows the
answer.

The line above the grid is the **edit bar**. On its left is the
cursor's reference; beside it is what the cell really holds. For a
formula that is the formula itself, while the cell shows its answer.
Choose a cell to see both.

```figure
(define k (choice 'cell '("B2" "B3" "B4") "Cursor on"))
(zs-bar 28 4 264 (list-ref '("B2" "B3" "B4") k) (list-ref '("12.5" "8" "=B2+B3") k))
(zs-place 50 56 64 20 0 1)
(zs-sheet '(("Item" "Cost" "") ("pens" "12.5" "") ("paper" "8" "") ("Total" "20.5" "")))
(zs-box 1 (+ k 2) 1 (+ k 2))
```

```check
(number
  (ask "B2 holds 30 and B3 holds 45. What does a cell holding =B2+B3 show?")
  (answer 75) (from "30 + 45")
  (wrong-if 3045 "A formula works the sum out; it does not put the two numbers side by side.")
  (hint "The formula adds what is in B2 and what is in B3."))
```

---

## Putting it right

- **Esc**, while you type, throws the entry away; the cell keeps what it
  had.
- **F2**, or a click on the edit bar, opens what the cell holds, to
  change it without typing it all again. Left and Right move inside it.
- **Delete** or **Backspace**, when you are not typing, empties the cell.

While you type, Up and Down keep the entry and move, so you can type
down a column without pressing Enter each time.

```check
(choice
  (ask "C4 holds =C2*C3+15, but you meant +16. What is the quickest way to change it?")
  (right "Press F2, and change the 5 to a 6")
  (wrong "Press Delete" "Delete empties the cell; you would then have to type the whole formula again.")
  (wrong "Press Esc" "Esc throws away an entry you are typing. Here nothing is being typed yet.")
  (hint "One function key edits what a cell already holds."))
```

---

## Undo

**Ctrl+Z** is **undo**: it takes back the last change, and again the one
before, as far back as the machine has room for. **Ctrl+Y** puts back what you
undid. Every change counts: typing, clearing, pasting, formats.

```sources
(source "Zeitlos docs, sheet (docs/sheet_app.md), Undo")
```

```check
(choice
  (ask "You press Delete on a cell by mistake. What brings its contents back?")
  (right "Ctrl+Z")
  (wrong "Ctrl+Y" "Ctrl+Y puts back what you undid. First you need to undo the clearing: Ctrl+Z.")
  (wrong "Esc" "Esc only throws away an entry while you are typing it. The clearing is already done."))
```

---

```try
(try (app "sheet")
  (task "Open sheet from the dock. In A1 type Item, in B1 Cost. In A2 and A3 type pens and paper; in B2 and B3 type 12.5 and 8. In B4 type =B2+B3. Come back here.")
  (check (number (ask "What does B4 show?") (answer 20.5) (from "12.5 + 8")
    (wrong-if 0 "Check that B2 and B3 hold numbers, not text: they should sit at the right of their cells.")
    (hint "It adds 12.5 and 8."))))
```

---

```check
(choice
  (ask "You type Total into A4 but have not pressed Enter yet. You change your mind. What do you press?")
  (right "Esc")
  (wrong "Delete" "While you are typing, Delete removes at most one letter, not the whole entry. Esc throws the entry away.")
  (wrong "Ctrl+Y" "Ctrl+Y redoes something you undid. Esc throws away what you are typing."))
```

A number fills its cell from the right, text from the left, a formula
starts with `=`, and Esc, F2 and Ctrl+Z put mistakes right.
