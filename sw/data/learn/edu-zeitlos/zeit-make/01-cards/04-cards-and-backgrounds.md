# Cards and backgrounds

```lesson
(objective "Name cards, go to them, put them in order, and share a background between them.")
(minutes 20)
(keywords "background (of a card)" "sorter")
```

Every card has a name: `card 1`, `card 2` and so on, until you give it
one of your own. With nothing selected, the command `name intro` names
the card `intro`.

Names let you go straight to a card: `go intro`. `go 3` goes to the
third card, and `go first`, `go last`, `go next` and `go prev` do what
they say.

| Command | Does |
|---|---|
| new | a new card after this one (as Ctrl+M) |
| dup card | a copy of this card |
| delete card | this card (Ctrl+Z brings it back) |

```sources
(source "slide (docs/slide_app.md), Using it (the commands)")
```

```check
(text
  (ask "Your deck has a card named results. After pressing /, what do you type to go to it?")
  (accept "go results") (accept "/go results")
  (wrong-if "results" "A name alone is not a command. The command for another card is go.")
  (hint "The command is go, then where to."))
```

---

## A background

A **background** is a card of its own, drawn first, under
every card that names it. A title band or a footer that every card
shows goes on a background, so you draw it once.

- `background plain` gives this card the background `plain`, making
  it if it is new.
- `edit background` lets you draw on the background; the same command
  again goes back to the card.
- A new card shares the background of the card you made it from.

```figure
(mk-card 20 60 84 60 #t "Hello")
(mk-card 118 60 84 60 #t "Plans")
(mk-card 216 60 84 60 #t "Thanks")
(text 160 40 "background plain: the grey band" 'centre 'small)
(caption "One background, under three cards")
```

```sources
(source "slide (docs/slide_app.md), The deck")
```

```check
(choice
  (ask "Ten cards share the background plain. You change the band on it. How many cards show the change?")
  (right "All ten")
  (wrong "Only the card you were on" "The band is drawn on the background, and all ten cards are drawn over the same background.")
  (wrong "None, until you copy it to each card" "There is nothing to copy: each card shows the background it names.")
  (hint "How many backgrounds are there?"))
```

---

## All the cards: F6

**F6** shows every card small: the **sorter**. Arrow keys move between
them, a drag puts a card in another place, **Delete** removes one, and
**Enter** or **Esc** goes back to the card you chose.

In the sorter, **F5** starts the show from the chosen card (the next
unit is about shows).

```figure
(mk-card 16 40 66 46 #t "1")
(mk-card 92 40 66 46 #t "2")
(mk-card 168 40 66 46 #t "3")
(mk-card 244 40 66 46 #t "4")
(mk-card 16 110 66 46 #t "5")
(frame 164 36 74 54 'ink 2.5)
(caption "The sorter: card 3 is chosen")
```

```check
(choice
  (ask "Card 7 of a deck should be the second card. What is the easiest way?")
  (right "Press F6 and drag card 7 to the second place")
  (wrong "Delete card 7 and draw it again as card 2" "That is a lot of work, and the sorter can move a card with a drag.")
  (wrong "Rename card 7 to card 2" "A name does not set the order. The order is where a card is in the deck.")
  (hint "Which key shows all the cards at once?"))
```

---

## Naming things

Things on a card can have names too. **F2** edits the words of a text
box, or of a rectangle, rounded rectangle or oval, which can hold words
too. With anything else selected, such as a line or a picture, F2 puts
`name ` on the command line for you, to name it; with nothing selected,
to name the card. Names matter later, when a button shows or hides
something by its name.

```check
(choice
  (ask "A line is selected, and you press F2. What happens?")
  (right "The command line opens with name, ready for the line's name")
  (wrong "Its words open for typing" "A line holds no words. F2 types into a text box or a shape; for anything else it starts the name command.")
  (wrong "The card is renamed" "With something selected, a command works on the selection, not the card.")
  (hint "F2 means rename, or edit in place. What can a line hold?"))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Type /background plain and press Enter, then /edit background and Enter. Draw a rectangle across the top, then /edit background again. Press Ctrl+M twice. Then come back.")
  (check (choice
    (ask "What do the two new cards show?")
    (right "The same rectangle across the top")
    (wrong "Nothing: they are empty" "A new card shares the background of the card it was made from, and the rectangle is on that background.")
    (wrong "Only the second shows it" "Both new cards share the background, so both show it."))))
```

So: cards have names to go to; a background is drawn under every card
that names it; and F6 shows all the cards to put them in order.
