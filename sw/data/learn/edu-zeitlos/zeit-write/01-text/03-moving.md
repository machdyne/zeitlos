# Moving around

```lesson
(objective "Move through a long document by line, by page and by line number, and count its words.")
(minutes 15)
(keywords "display line" "font")
```

A long paragraph is wrapped over several lines on the screen. Each of
these is a **display line**: a line as it is shown, not a line of the
file.

| Key | Moves the caret |
|---|---|
| Home | to the start of its display line |
| End | to the end of its display line |
| Up, Down | one display line, keeping the column |

```check
(choice
  (ask "The caret is in the middle of a paragraph wrapped over four display lines. You press Home. Where does the caret go?")
  (right "To the start of the display line it is on")
  (wrong "To the start of the paragraph"
    "Home goes to the start of the line you see, not of the whole paragraph. From the third display line, it goes to the start of the third.")
  (wrong "To the start of the document"
    "Home stays on the caret's own display line.")
  (hint "Home and End work on the line as it is shown."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Using it; Go to line")
```

---

## Pages and the wheel

**PageUp** and **PageDown** move a screen at a time. The **mouse
wheel** moves the view three lines a notch, but leaves the caret where
it was. When you type, the view goes back to the caret.

```check
(choice
  (ask "You scroll down with the wheel, past the caret, and type a word. Where does it go?")
  (right "Where the caret was, and the view moves back to it")
  (wrong "At the top of the lines you scrolled to"
    "The wheel moves only the view. The caret stays where it was, and typing goes there.")
  (wrong "At the end of the document"
    "The wheel does not move the caret, so typing goes where the caret was.")
  (hint "Does the wheel move the caret?"))
```

---

## Go to line, and counting

**Ctrl+L** opens Go to line. It says where you are and how much there
is: "Line 12 of 340", then the words and the characters. Type a line
number and press Enter to go there; a number past the end goes to the
last line.

Here a line is a line of the file. A paragraph is one line, however
many display lines it wraps over.

```check
(number
  (ask "A paragraph of 60 words is wrapped over 5 display lines. How many lines does Go to line count it as?")
  (answer 1)
  (wrong-if 5 "5 is its display lines. Go to line counts lines of the file, and a paragraph is one line until Enter ends it.")
  (wrong-if 60 "60 is its words. A line of the file ends where Enter was pressed.")
  (hint "Only Enter ends a line of the file."))
```

---

## What counts as a word

Go to line counts a word as any run of characters between spaces or
line ends. Punctuation that stands alone, like `--`, counts as a word.

```check
(number
  (ask "How many words does Go to line count in: Hello, world -- again")
  (answer 4)
  (wrong-if 3 "The -- has spaces on both sides, so it is a run of its own, and counts as a word.")
  (wrong-if 5 "Hello, with its comma is one run: the comma has no space before it.")
  (hint "Count the runs between spaces: Hello, then world, then ..."))
```

---

## The size of the letters

A **font** is a set of letter shapes, in one size. The titlebar's
font button switches `text` between two fonts, a small one and a
larger one. The lines wrap again, and the caret stays on the same
character.

```check
(choice
  (ask "You switch to the larger letters. What happens to the lines?")
  (right "Fewer letters fit on a line, so the paragraphs wrap again")
  (wrong "The text runs past the right edge"
    "text always wraps to the window. Bigger letters only mean fewer of them on a line.")
  (wrong "The document is changed"
    "Only the view changes. The file and the caret's place in it stay as they were.")
  (hint "The window is the same width; the letters are wider."))
```

---

```try
(try (app "text")
  (task "In text, start a new document with Ctrl+N. Type three short lines, pressing Enter after each one. Press Ctrl+L and read the box, then press Esc and come back here.")
  (check (number
    (ask "Which line did Go to line say the caret is on?")
    (answer 4)
    (wrong-if 3 "Each Enter starts a new line. After the third Enter the caret is on a fourth, empty line.")
    (hint "Where is the caret after the last Enter?"))))
```

So: Home and End work on display lines, PageUp and PageDown on screens,
and Ctrl+L goes to a line of the file and counts its words.
