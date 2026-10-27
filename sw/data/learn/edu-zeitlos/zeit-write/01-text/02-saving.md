# Saving and opening

```lesson
(objective "Save a document under a name, open it again, and know when work is not yet saved.")
(minutes 20)
(keywords "dialog (box)" "ending (of a file name)" "encoding (of text)" "UTF-8" "Latin-9")
```

What you type is kept only inside `text` until you save it in a file.
The title shows the file's name. A `*` in front of the name means there
are changes that are not saved yet.

Tick the box under the figure to save, and watch the title.

```figure
(define saved (toggle 'saved "Saved"))
(wr-window 40 30 240 120 (if saved "letter.txt" "*letter.txt"))
(wr-lines 50 62 '("Dear Sam," "" "thank you for the seeds."))
(caption (if saved "Saved: no star." "Changed since the last save: a star."))
```

```check
(choice
  (ask "The title says `*letter.txt`. What does the star tell you?")
  (right "It has changes not saved yet")
  (wrong "It cannot be changed"
    "The star is not a lock. It says the window holds changes the file on disk does not have yet.")
  (wrong "It is a new file"
    "A new document that was never saved is called untitled. The star only says there are unsaved changes.")
  (hint "Save, and the star goes away."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Using it; Save As")
```

---

## Saving

Press **Ctrl+S** to save. The first time, `text` opens the Save As
**dialog**: a small window that asks you something before going on.
This one shows the files and folders of a folder, and a field for the name. It starts in
`/home`, the folder for your own files.

Type a name and press **Enter** (or click **Save**). **Esc** or
**Cancel** saves nothing. After that, Ctrl+S saves to the same file
without asking.

```check
(choice
  (ask "You save a new document for the first time. Where does the Save As dialog start?")
  (right "In /home, your own folder")
  (wrong "In the folder of the app text"
    "Apps live elsewhere. The dialog starts in /home, where your own files go.")
  (wrong "On the desktop"
    "The dialog starts in the folder /home.")
  (hint "It is the folder meant for your own files."))
```

---

## The ending of a name

The part of a file name after the last dot is its **ending**: `.txt`
in `letter.txt`. Give every document one. `files` uses it to choose the
app that opens the file:

| Ending | Opens in |
|---|---|
| .txt | text |
| .md | read |

`.md` is for the documents of the next unit, which `read` shows.

```check
(choice
  (ask "You want files to open your shopping notes in text when you double-click it. Which name do you give it?")
  (right "shopping.txt")
  (wrong "shopping.md"
    "files opens a .md file in read, which shows a document but does not edit it.")
  (wrong "shopping"
    "With no ending, files cannot tell which app the file is for.")
  (hint "Which ending opens in text?"))
```

---

## New, open and close

The titlebar has an icon for each of these, and a key:

| Icon | Key | Does |
|---|---|---|
| new | Ctrl+N | starts an empty document |
| open | Ctrl+O | opens a file, through the Open dialog |
| save | Ctrl+S | saves |
| close | Ctrl+Q | closes text |

If one of them would lose unsaved changes, `text` asks first: "Save
changes before continuing?" **Yes** saves, then goes on. **No** goes on
without saving: the changes are lost. **Cancel** does nothing.

```check
(choice
  (ask "You press Ctrl+Q with unsaved changes, and answer No. What happens?")
  (right "text closes, and the changes are lost")
  (wrong "text saves the changes, then closes"
    "That is Yes. No means: go on without saving.")
  (wrong "Nothing: text stays open"
    "That is Cancel. No means: go on without saving.")
  (hint "Yes saves, No does not save, Cancel stops."))
```

---

## Save As

**Ctrl+Shift+S** is Save As: it asks for a name again, so you can keep
a copy under a new name or in another folder. From then on the window
belongs to the new name. If a file of that name is there already,
`text` asks before replacing it.

```check
(choice
  (ask "You save letter.txt as letter2.txt with Ctrl+Shift+S. Then you type more and press Ctrl+S. Which file gets the new words?")
  (right "letter2.txt")
  (wrong "letter.txt"
    "After Save As the window belongs to the new name. letter.txt stays as it was before Save As.")
  (wrong "Both"
    "Ctrl+S saves to one file: the name in the title, which is now letter2.txt.")
  (hint "Look at the title after Save As."))
```

---

## Files from other computers

Letters are kept in a file as numbers. An **encoding** says which
number stands for which character. `text` saves new documents in
**UTF-8**, the encoding that holds every character there is. A file
from an older system may use another encoding, such as **Latin-9**
(Western European letters only) or Windows-1252; then the title says
so, as `(Latin-9)`, and `text` saves it the same way.

If you add a character that encoding cannot hold, `text` asks when you
save. **Yes** saves in UTF-8 from then on. **No** keeps the old
encoding, with `?` in place of each such character.

```check
(choice
  (ask "An old file's title says (Latin-9). You add a Japanese word and save, answering Yes. What happens?")
  (right "The file is saved in UTF-8, with the Japanese word in it")
  (wrong "The Japanese word is saved as question marks"
    "That is what No does: it keeps Latin-9, which has no Japanese.")
  (wrong "Nothing is saved"
    "That is Cancel. Yes saves, in UTF-8, which holds every character.")
  (hint "UTF-8 holds every character."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Encodings; Text encoding (docs/text_encoding.md)")
```

---

```try
(try (app "text")
  (task "In text, start a new document (Ctrl+N) and type a line. Press Ctrl+S, type the name first.txt, and press Enter. Close text, open it again from the dock, and open first.txt with Ctrl+O. Then come back here.")
  (check (choice
    (ask "Just after you saved, what did the title show?")
    (right "first.txt, with no star")
    (wrong "`*first.txt`"
      "Saving takes the star away. If you still see it, the save did not happen: press Ctrl+S again.")
    (wrong "untitled"
      "A saved document takes the file's name. If it still says untitled, the save was cancelled: press Ctrl+S again."))))
```

So: Ctrl+S saves (with a name and an ending the first time), a star
means unsaved changes, and `text` asks before anything would lose them.
