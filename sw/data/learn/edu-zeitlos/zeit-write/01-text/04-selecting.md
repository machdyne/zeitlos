# Copy, cut and undo

```lesson
(objective "Select text, copy or move it, and undo and redo changes.")
(minutes 20)
(keywords "selection (of text)" "clipboard" "undo (a change)")
```

A **selection** is a piece of text you have marked, to do something
with it. `text` shows it light on dark.

| To select | Do |
|---|---|
| with the keys | hold Shift and move the caret |
| with the mouse | drag across the text |
| up to a place | Shift+click there |
| everything | Ctrl+A |

Typing while there is a selection replaces it with what you type.

```check
(choice
  (ask "You select the word dog and type cat. What happens?")
  (right "dog is replaced by cat")
  (wrong "cat goes in after dog"
    "Typing over a selection replaces it. Click first, to put the caret in without a selection, if you want to add.")
  (wrong "Nothing: you have to delete dog first"
    "Typing replaces the selection, so there is no need to delete it first.")
  (hint "The selection is what the typing acts on."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Using it; Selection; Undo")
```

---

## The clipboard

The **clipboard** holds the last text you copied or cut, ready to put
in somewhere else, in this app or another.

| Key | Does |
|---|---|
| Ctrl+C | copies the selection to the clipboard |
| Ctrl+X | cuts: copies it, then takes it out of the document |
| Ctrl+V | pastes: puts the clipboard's text in at the caret |

A right-click copies the selection too.

```check
(choice
  (ask "You want to move a sentence to the end of the document. Which do you use first?")
  (right "Ctrl+X, to cut it")
  (wrong "Ctrl+C, to copy it"
    "Copying leaves the sentence where it was, so after pasting it would be there twice. Cut takes it out.")
  (wrong "Ctrl+V, to paste it"
    "Paste puts in what is on the clipboard. The sentence has to get there first, by cutting it.")
  (hint "Moving means it should not stay where it was."))
```

---

## Moving a paragraph

```example
(example
  (problem "Move the second paragraph to the end of the document.")
  (step "Click at the start of the paragraph, then Shift+click at its end: it is selected.")
  (step "Press Ctrl+X. The paragraph is cut out, and is now on the clipboard.")
  (step "Click below the last line: the caret goes to the very end.")
  (step (choice (ask "Which key puts the paragraph in at the caret?")
    (right "Ctrl+V")
    (wrong "Ctrl+C" "Ctrl+C copies a selection. To put the clipboard's text in, paste with Ctrl+V.")))
  (answer "The paragraph is now at the end of the document."))
```

---

## Undo and redo

**Ctrl+Z** is **undo**: it takes back your last change. **Ctrl+Y** (or
Ctrl+Shift+Z) is redo: it puts back what undo took back.

One undo takes back a group of changes:

- typing, up to and including an Enter: so undo goes back a line at a
  time;
- a run of Backspace, or of Delete;
- one cut, or one paste.

```check
(choice
  (ask "You type two lines, pressing Enter after each, then press Ctrl+Z once. What is taken back?")
  (right "The second line")
  (wrong "Only the last letter"
    "Undo takes back a group: typing up to and including an Enter. That is the whole second line.")
  (wrong "Both lines"
    "Each Enter ends a group. One undo takes back one group: the second line.")
  (hint "Typing up to an Enter is one group."))
```

---

## How far back

Undo keeps your recent changes, not all of them: very old ones, and a
very large paste, cannot be undone. Opening another document starts
over.

When undo brings the document back to exactly what was last saved, the
star leaves the title.

```check
(choice
  (ask "You save, type one word, then press Ctrl+Z. What does the title show?")
  (right "The name, with no star")
  (wrong "The name, with a star"
    "Undo took the word back, so the document is again exactly what was saved. text knows, and takes the star away.")
  (hint "Is the document now different from what was saved?"))
```

---

```try
(try (app "text")
  (task "In text, type three short lines. Select the first line by dragging across it, cut it with Ctrl+X, click below the last line and paste it with Ctrl+V. Then press Ctrl+Z, and come back here.")
  (check (choice
    (ask "What did Ctrl+Z take back?")
    (right "The paste: the line left the end again")
    (wrong "The cut, so the line came back at the start"
      "Undo takes back the last change first, which was the paste. A second Ctrl+Z takes back the cut.")
    (wrong "Everything you typed"
      "Undo goes back one group at a time, starting with the last: the paste."))))
```

So: select, then cut or copy, and paste; and Ctrl+Z takes back the last
change, a group at a time.
