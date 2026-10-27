# A deck of cards

```lesson
(objective "Open slide, find your way around its window, and add, move between and save cards.")
(minutes 15)
(keywords "deck" "card (of a deck)" "status line (in slide)")
```

You can make cards, slides and pages in `slide`. They are all the same
thing to it: a **card** is one page of drawing and words,
and a **deck** is a stack of cards kept in one file.

A card is called a slide when it is shown on the whole screen, and a
page when it is printed. It is one drawing, shown at any size.

`slide` is in the dock. Started with no file, it makes a new deck of one
card that says "A new deck".

```sources
(source "slide (docs/slide_app.md), the opening")
```

```check
(choice
  (ask "You make a talk of ten slides in slide. What have you made?")
  (right "One deck of ten cards, in one file")
  (wrong "Ten files, one for each slide"
    "A deck keeps all of its cards together in one file. You open the deck, not each card.")
  (wrong "Ten cards that can only be printed"
    "The same cards can be shown on the screen or printed. A slide is a card shown on the whole screen.")
  (hint "A deck is a stack of cards. How many files does a stack need?"))
```

---

## The window

The figure is a plan of slide's window.

- Down the left are the **tools**, one letter each. The letter picks
  the tool.
- In the middle is the card, as large as fits (never more than its own
  size).
- Under the card, a bar sets how things look.
- At the foot is the **status line**. It says which card you
  are on, as `Card 1/3`, and anything slide has to tell you.

```figure
(mk-slide-window -1 "the card")
(caption "slide's window, as a plan")
```

```sources
(source "slide (docs/slide_app.md), Using it")
```

```check
(choice
  (ask "The status line says Card 2/5. What does that mean?")
  (right "You are on the second card of a deck of five")
  (wrong "The deck has 2 cards and 5 are hidden"
    "The first number is the card you are on, and the second is how many cards the deck has.")
  (wrong "The card is at 2/5 of its size"
    "The card is shown as large as fits. 2/5 is about where you are in the deck.")
  (hint "Read it as card 2 of 5."))
```

---

## More cards

- **Ctrl+M** adds a new, empty card after the one you are on.
- **PageDown** goes to the next card, and **PageUp** to the one before.
  The mouse wheel does the same.

Each new card gets a name, such as `card 2`, which the status line
shows after the card's number.

```sources
(source "slide (docs/slide_app.md), the keys")
```

```check
(choice
  (ask "You are on card 1 of 1 and press Ctrl+M twice. Where are you now?")
  (right "On card 3 of 3")
  (wrong "On card 1 of 3"
    "Each new card is added after the one you are on, and you go to it. After two, you are on the last of three.")
  (wrong "On card 2 of 2"
    "Ctrl+M was pressed twice, so there are two new cards: three in all.")
  (hint "Each Ctrl+M makes one more card and takes you to it."))
```

---

## Saving

**Ctrl+S** saves the deck. The first time, slide asks for a name and a
folder. A deck's file name ends in `.zsl`; slide adds the ending if you
leave it off.

The window's title shows the file's name. **Ctrl+O** opens another deck,
and **Ctrl+N** starts a new one. These three are also the icons in the
title bar.

```sources
(source "slide (docs/slide_app.md), the keys; The file")
```

```check
(text
  (ask "You save a new deck and type the name talk. What is the file called?")
  (accept "talk.zsl")
  (wrong-if "talk" "slide adds the ending for a deck when you leave it off.")
  (hint "A deck's file name ends in .zsl."))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Press Ctrl+M twice, then PageUp once. Read the status line, then come back.")
  (check (number
    (ask "Which card number does the status line show now?")
    (answer 2) (from "3 - 1")
    (wrong-if 3 "Ctrl+M twice takes you to card 3, and PageUp then goes back one.")
    (wrong-if 1 "PageUp goes back one card, not to the first.")
    (hint "Three cards; you were on the last; PageUp is one back."))))
```

---

```check
(choice
  (ask "Which key adds a new card?")
  (right "Ctrl+M")
  (wrong "Ctrl+N" "Ctrl+N starts a whole new deck. Ctrl+M adds a card to this one.")
  (wrong "PageDown" "PageDown goes to the next card that is already there.")
  (hint "It is Ctrl with a letter, and it changes this deck, not which deck is open."))
```

So: slide keeps cards in a deck, a `.zsl` file; Ctrl+M adds a card,
PageUp and PageDown move between them, and Ctrl+S saves.
