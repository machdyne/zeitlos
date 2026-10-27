# Links

```lesson
(objective "Link a Markdown document to another document and to a heading in it.")
(minutes 15)
(keywords "link (in Markdown)" "anchor (of a heading)")
```

A **link** is words that take the reader somewhere else when chosen.
In Markdown you write the words in square brackets and where they go in
round brackets, with nothing between: `[my plan](plan.md)`.

`read` underlines the words. Click them, or press Tab until they are
boxed and press Enter, to follow the link. **Backspace** goes back.

```check
(text
  (ask "Write a link with the words garden plan that goes to the file plan.md.")
  (accept "[garden plan](plan.md)")
  (wrong-if "(garden plan)[plan.md]" "The brackets are the other way round: square brackets for the words, round brackets for where they go.")
  (wrong-if "[garden plan] (plan.md)" "There must be nothing between `]` and `(`. With a space, read shows the brackets as text.")
  (hint "Square brackets, then round brackets.")
  (hint "`[words](file)`"))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), Links; sw/apps/read/read.c, follow() and slugify()")
```

---

## Where a link can go

- `plan.md`: that file, in the same folder as this document;
- `/home/plan.md`: that file, wherever this document is;
- `notes.txt`, a file that is not Markdown: it opens in its own app,
  here `text`;
- a page on another computer: nowhere. `read` says the link points off
  this machine.

```check
(choice
  (ask "/home/week.md has the link `[list](list.md)`. Which file does it open?")
  (right "/home/list.md")
  (wrong "list.md in the folder read was started from"
    "A name with no / in front is found in the folder of the document that holds the link: here /home.")
  (wrong "Nothing: the link needs the whole name"
    "A short name works: it is taken from the folder of the document the link is in.")
  (hint "Which folder is week.md in?"))
```

---

## Links to a heading

A link can go to a heading of the same document. Write `#` and the
heading's **anchor**: its words in small letters, each space made a
`-`, and anything that is not a letter, a digit or `-` left out.

So `## Shopping list` has the anchor `shopping-list`, and
`[the list](#shopping-list)` goes there.

```check
(text
  (ask "What does a link put in its round brackets to go to the heading ## Week 2: Jobs ?")
  (accept "#week-2-jobs")
  (case)
  (wrong-if "#Week-2-Jobs" "An anchor is all small letters: read compares it with the heading's words made small.")
  (wrong-if "#week-2:-jobs" "The colon is left out: only letters, digits and - stay.")
  (wrong-if "week-2-jobs" "Without # in front, read looks for a file called week-2-jobs. Put # first.")
  (hint "Small letters, spaces become -, and the colon goes.")
  (hint "# then week-2-..."))
```

---

## A heading in another file

Put the file and the anchor together: `[jobs](plan.md#jobs)` opens
`plan.md` at its heading `## Jobs`.

```check
(choice
  (ask "You follow a link to plan.md#jobs and want to get back. Which key?")
  (right "Backspace")
  (wrong "Esc"
    "In read, Esc clears a selection. Backspace goes back to where you were.")
  (wrong "Shift+Tab"
    "Shift+Tab moves to the link before, on the screen. Backspace goes back.")
  (hint "The same key as going back in many readers."))
```

```note
(note "A picture written `![a garden](garden.png)` is not shown in read: only its words, a garden, are, as a link to the picture's file.")
```

---

```try
(try (app "text")
  (task "In text, start a new document. Write a link `[my plan](plan.md)`, save it in /home as start.md, then open start.md in read from files. Follow the link, then press Backspace. Come back here.")
  (check (choice
    (ask "What did Backspace do?")
    (right "Went back to start.md")
    (wrong "Deleted a letter"
      "read does not edit, so Backspace there goes back. Was the read window in front when you pressed it?")
    (wrong "Closed read"
      "Backspace goes back one document. Close read with the close icon."))))
```

So: `[words](where)` makes a link, to a file, to `#anchor` of a
heading, or to both.
