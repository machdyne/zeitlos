# First words

```lesson
(objective "Start text, type a few paragraphs, and correct mistakes as you type.")
(minutes 15)
(keywords "character (of text)" "caret" "paragraph (in text)" "wrapping (of lines)")
```

`text` is the app for writing: notes, letters, stories. Open it
from the dock. Its window starts empty, and its title says `untitled`
until you save.

A **character** is one letter, digit, space or mark. `text` has no
modes: every key that types a character puts that character in. Accented letters and the euro sign go in too, from any
keyboard layout.

The **caret** is the thin upright line in the text. It sits *between*
two characters, and shows where the next one you type will go.

```check
(choice
  (ask "You type a letter. Where does it go?")
  (right "Where the caret is")
  (wrong "Where the mouse pointer is"
    "The pointer only shows where the mouse is. Click to move the caret there first; then the letter goes in at the caret.")
  (wrong "At the end of the document"
    "Only if the caret is at the end. Wherever the caret is, the letter goes in there.")
  (hint "Look for the thin upright line."))
```

```sources
(source "Zeitlos docs: text (docs/text_editor.md), Using it; Drawing")
```

---

## Wrapping

Keep typing when a line is full. `text` moves the next word down to a
new line by itself: this is **wrapping**. A word too long for a whole
line is broken in the middle.

Press **Enter** only at the end of a **paragraph**: a group of
sentences that belong together. Enter starts a new paragraph on a new
line.

Move the slider to make the window narrower or wider. The lines wrap
again; only the place where Enter was pressed stays a new line.

```figure
(define cols (slider 'cols 16 38 30 "Width" 1))
(define w (+ 16 (* wr-cw cols)))
(define x (- 160 (/ w 2)))
(define p1 (wr-wrap '("Dear" "Sam," "thank" "you" "for" "the" "seeds." "The" "beans" "came" "up" "in" "a" "week," "and" "the" "peas" "are" "climbing" "the" "fence.") cols))
(define p2 (wr-wrap '("See" "you" "soon.") cols))
(wr-window x 16 w 176 "*letter.txt")
(wr-lines (+ x 8) 48 p1)
(wr-lines (+ x 8) (+ 48 (* wr-lh (length p1))) p2)
(caption "Enter was pressed once: after fence.")
```

```check
(choice
  (ask "You are typing a long paragraph and reach the right edge of the window. What do you do?")
  (right "Keep typing: the next word wraps to a new line by itself")
  (wrong "Press Enter to start the next line"
    "Enter ends the paragraph. Pressed in the middle of one, it splits it in two, and the break stays there even when the window is made wider.")
  (wrong "Make the window wider before going on"
    "You can, but you need not: text wraps the words to whatever width the window has.")
  (hint "Move the slider in the figure: who decides where the lines break?"))
```

---

## Correcting

| Key | Does |
|---|---|
| Backspace | deletes the character before the caret |
| Delete | deletes the character after the caret |
| Left, Right | move the caret one character |
| Up, Down | move it one line, keeping its column |

A click puts the caret where you click. A click below the last line
puts it at the very end.

```check
(choice
  (ask "You typed teh and the caret is just after the h. Which key deletes the h?")
  (right "Backspace")
  (wrong "Delete"
    "Delete removes the character after the caret. The h is before it, so Backspace is the key.")
  (wrong "Left"
    "Left moves the caret back over the h, but deletes nothing.")
  (hint "The h is on the left of the caret."))
```

---

## Tab

**Tab** puts in four spaces. (It does not put in a tab character.)

```check
(number
  (ask "How many spaces does one press of Tab put in?")
  (answer 4)
  (wrong-if 1 "Tab is not one space: it is a run of spaces, to indent a line.")
  (wrong-if 8 "Some editors use 8, but text puts in 4.")
  (hint "It is fewer than 8."))
```

---

```try
(try (app "text")
  (task "Open text from the dock. Type two short paragraphs, pressing Enter once between them. Then drag the window's corner to make it narrower, and wider again. Come back here when you have seen what happens.")
  (check (choice
    (ask "What happened to your paragraphs as the window got narrower?")
    (right "Their lines broke at new places, to fit")
    (wrong "The words ran past the edge of the window"
      "Look again: text wraps every paragraph to the window's width, so no word runs off the edge.")
    (wrong "The two paragraphs joined into one"
      "The Enter you pressed stays where it is. Only the lines inside each paragraph move."))))
```

---

## Help

Press **F1** in `text` for a box of its keys. Its **Read docs** button,
there when the manuals are on the card, opens the whole page about
`text`: [text](docs:text_editor).

```check
(choice
  (ask "You forget which key does something in text. Which key shows its keys?")
  (right "F1")
  (wrong "F3" "F3 finds the same words again: you will meet it in a later lesson. F1 is help, in every app.")
  (wrong "Esc" "Esc does not show help. F1 is help, in every app."))
```

So: type, let `text` wrap the lines, press Enter at the end of each
paragraph, and correct with Backspace and Delete.
