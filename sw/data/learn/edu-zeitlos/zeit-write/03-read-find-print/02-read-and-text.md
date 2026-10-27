# Copying from read, writing in text

```lesson
(objective "Copy words out of read, and change a Markdown document in text while checking it in read.")
(minutes 15)
```

To copy from `read`, drag across the words to select them, then press
**Ctrl+C** or right-click. **Esc** takes the selection away, and so
does scrolling.

What goes on the clipboard is what you saw: the Markdown marks are
already gone.

```check
(choice
  (ask "In read you select and copy a line written in the file as `**Note:** bring a pen`. What do you paste into text?")
  (right "Note: bring a pen")
  (wrong "`**Note:** bring a pen`"
    "read copies what it shows, and it showed the line without the stars.")
  (hint "read copies what is on the screen."))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), Selection and copy; Keys")
```

---

## A press on a link

A press on a link does not follow it at once. If you let go without
moving, it is a click, and the link is followed. If you move first, you
select, so you can copy a link's words.

```check
(choice
  (ask "You want to copy the words of a link, not follow it. What do you do?")
  (right "Press on the link and drag across its words")
  (wrong "Click it, then press Ctrl+C"
    "A click with no movement follows the link. Drag across the words to select them.")
  (wrong "Tab to it and press Enter"
    "Enter follows the boxed link. Drag across the words to select them.")
  (hint "Moving while pressed selects."))
```

---

## read does not edit

`read` only shows a document. To change a `.md` file, open `text` from
the dock and open the file with Ctrl+O. Save it, then open it again in
`read` with Ctrl+O: `read` does not notice by itself that the file has
changed.

```check
(choice
  (ask "You fixed plan.md in text and saved it, but read still shows the old words. What do you do?")
  (right "Open plan.md again in read, with Ctrl+O")
  (wrong "Save again in text"
    "The file is saved already. read is showing what it read before: open the file again in read.")
  (wrong "Wait: read will catch up"
    "read does not watch the file. Open it again to see the change.")
  (hint "read does not notice changes by itself."))
```

---

## Writing and checking

```example
(example
  (problem "Add a list of jobs to plan.md and check how it looks.")
  (step "In text, Ctrl+O, and open /home/plan.md.")
  (step "Type the list, then save with Ctrl+S.")
  (step "In read, Ctrl+O, and open /home/plan.md again.")
  (step (choice (ask "read shows the jobs run together as one paragraph. What is the likely mistake?")
    (right "No space after each dash")
    (wrong "The file is not saved" "An unsaved list would not show at all. A list that shows as a paragraph means the items were not taken as items: check for - and a space.")))
  (answer "Fix the lines in text, save, and open the file again in read."))
```

---

```try
(try (app "read")
  (task "Open plan.md in read. Select a few words by dragging, press Ctrl+C, then open text from the dock and press Ctrl+V. Come back here.")
  (check (choice
    (ask "Did the pasted words have Markdown marks in them, such as `#` or `*`?")
    (right "No: only the words read showed")
    (wrong "Yes"
      "read copies what it shows, with the marks taken away. Perhaps the words were copied from text instead: try again from the read window."))))
```

So: copy from `read` with a drag and Ctrl+C, and change a document in
`text`, then open it again in `read`.
