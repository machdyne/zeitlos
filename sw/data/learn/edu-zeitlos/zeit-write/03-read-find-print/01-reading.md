# Reading with read

```lesson
(objective "Open a document in read, page through it, and follow its links.")
(minutes 15)
```

`read` shows Markdown documents: manuals, notes, books. It opens a
document of any size, since it never holds the whole of it in memory
at once. There are three ways to open one:

- double-click a `.md` file in `files`;
- in `read`, press **Ctrl+O** (or the titlebar's open icon) and choose
  a file;
- in an app, press **F1**, then **Read docs** (there when the manuals
  are on the card): the app's page opens in `read`.

```check
(choice
  (ask "You are in read and want another document. Which key?")
  (right "Ctrl+O")
  (wrong "Ctrl+N"
    "read does not make new documents: it only shows them. Ctrl+O opens one.")
  (wrong "F3"
    "read has no search. Ctrl+O opens a document.")
  (hint "The same key as Open in text."))
```

```sources
(source "Zeitlos docs: read (docs/read_app.md), Using it; Keys")
```

---

## Paging

| Key | Does |
|---|---|
| Space, PageDown | the next page |
| Shift+Space, PageUp | the page before |
| Down, Up | one line |
| Home, End | the start, the end |
| mouse wheel | three lines a notch |

```check
(choice
  (ask "You read a page too fast. Which keys take you back a page?")
  (right "Shift+Space, or PageUp")
  (wrong "Backspace"
    "In read, Backspace goes back to the document you came from by a link, not back a page.")
  (wrong "Space"
    "Space goes forward a page. With Shift it goes back.")
  (hint "The key that goes forward, with Shift."))
```

---

## Links

**Tab** boxes the next link on the screen, and **Shift+Tab** the one
before. **Enter** follows the boxed link; a click follows a link too.
**Backspace** goes back, and again, up to 12 steps.

Tab only moves among the links you can see: it never scrolls the page.

```check
(choice
  (ask "The link you want is further down, off the screen. You press Tab. What happens?")
  (right "Tab boxes a link on the screen; scroll down first to reach the other one")
  (wrong "The page scrolls down to the link"
    "Tab never moves the page. Scroll down with Space or the arrows, then Tab to the link.")
  (hint "Tab moves only among the links you can see."))
```

---

## Documents from other systems

Open a `.docx`, `.odt`, `.rtf` or `.epub` with Ctrl+O and a Markdown
copy of it is made. The copy opens in `text` if it is small enough to
edit there, and in a `read` of its own if not. The original file is not
changed.

```check
(choice
  (ask "You open letter.docx in read. What happens to letter.docx?")
  (right "Nothing: a Markdown copy of it is made and opened")
  (wrong "It is turned into Markdown in its place"
    "The original is never changed. The Markdown copy is a new file, kept apart.")
  (hint "Is the original touched?"))
```

```sources
(source "Zeitlos docs: convert (docs/convert.md); files (docs/file_browser.md), Opening a file")
```

---

```try
(try (app "read")
  (task "Open read from the dock. Press F1, then Read docs, to open read's own page. Page down with Space, Tab to a link and press Enter, then press Backspace. Come back here.")
  (check (choice
    (ask "Where did Backspace take you?")
    (right "Back to read's page, where the link was")
    (wrong "Up one line"
      "Up goes up a line. Backspace goes back to where you followed the link from.")
    (wrong "Nowhere"
      "If you had not followed a link yet, there was nothing to go back to. Follow one with Enter, then press Backspace."))))
```

So: Space and Shift+Space page, Tab and Enter follow links, and
Backspace goes back.
