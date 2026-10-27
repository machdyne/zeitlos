# Opening files

```lesson
(objective "Open a file from files, and tell from its name which app will open it.")
(minutes 15)
(keywords "extension (of a file name)")
```

This lesson is about what happens when you open a file.

Most file names end in a dot and a few letters: `notes.txt`,
`map.png`, `scores.zss`. Those last letters are the name's
**extension**. They say what kind of file it is, and so which app opens
it.

When you double-click a file in files (or choose it and press Enter),
files looks at the extension and starts that app with the file in it.

```sources
(source "files (docs/file_browser.md), Opening a file")
```

```check
(text
  (ask "What is the extension of the file name holiday.png?")
  (accept "png") (accept ".png")
  (wrong-if "holiday" "That is the name before the dot. The extension is the letters after it.")
  (hint "The letters after the dot."))
```

---

## Which app

| Extension | Opens in |
|---|---|
| `.txt` | text, to read and change |
| `.md` | read, as a document |
| `.zss` | sheet, a table of numbers |
| `.zsl` | slide |
| `.png`, `.jpg`, `.gif`, `.bmp` | view, to look at a picture |
| `.html` | web |

A file with no extension is taken to be a program, and runs if it
really is one. For an extension no app claims, files says so in a box
rather than doing nothing.

Documents from other computers, such as `.docx`, are copied and
changed into a form Zeitlos can open; the file you started from is
left as it was.

```sources
(source "files (docs/file_browser.md), Opening a file")
(source "sw/common/ztype.c, the table of extensions")
```

```check
(choice
  (ask "You double-click shopping.txt in files. Which app opens?")
  (right "text")
  (wrong "read" "read opens .md documents. A .txt file opens in text, where you can change it.")
  (wrong "sheet" "sheet opens .zss tables. shopping.txt opens in text.")
  (hint "txt is short for text."))
```

---

```try
(try (app "files")
  (task "Press Super+2 and open files from the dock. Go into the folder docs and double-click the file welcome.txt. Read its first lines, close its window with its close box, then press Super+1 to come back here.")
  (check (choice (ask "Which app showed welcome.txt?")
    (right "text")
    (wrong "read" "read is for .md documents. welcome.txt ends in .txt, so text opened it.")
    (wrong "files" "files does not show what is inside a file: it starts the app the extension names.")
    (hint "Look at the extension: .txt."))))
```

---

```check
(choice
  (ask "You double-click photo.jpg. Which app do you expect?")
  (right "view")
  (wrong "draw" "draw opens pictures in its own format. A .jpg photo opens in view.")
  (wrong "text" "text opens .txt files. Pictures such as .jpg open in view.")
  (hint "The app for looking at pictures."))
```

```check
(choice
  (ask "You open report.docx, a document from another computer. What happens to report.docx itself?")
  (right "Nothing: a copy is made in a form Zeitlos can open")
  (wrong "It is changed for good" "The file you started from is left as it was. Zeitlos opens a copy made from it.")
  (wrong "It is deleted" "Opening never deletes anything. A copy is made, and the original stays.")
  (hint "The lesson said the file you started from is left ..."))
```

So the extension -- the letters after the dot -- tells files which app
opens a file, and a double-click or Enter opens it there.
