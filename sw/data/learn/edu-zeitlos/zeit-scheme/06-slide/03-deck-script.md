# The deck's script

```lesson
(objective "Keep definitions in a deck's script, and run Scheme when a card is shown or a control clicked.")
(minutes 20)
(keywords "deck's script (in slide)")
```

An action is one line. For more, a deck has scripts. The
**deck's script** holds definitions for every card,
and runs when the deck starts being used: with the browse tool, in a
show, or as an app (F7).

`/script deck` writes the script to the file `/tmp/deck.scm` and opens
it in `text`. Each time you save it back to that same file, slide
takes it -- once it has checked that its brackets close and its
strings end -- and says so in the status line. (For a file in `/tmp`,
text's Ctrl+S asks where to save, starting in `/home`: type
`/tmp/deck.scm` as the name, to save the same file again.)

```sources
(source "Zeitlos docs: slide_scripting.md, Where scripts live")
(source "Zeitlos docs: text_editor.md, Save As; Zeitlos source: sw/common/zdialog.c, a typed full path")
```

```check
(choice
  (ask "You save the deck's script with a bracket left open. What does slide do?")
  (right "It does not take it, so a half-written script never reaches the deck")
  (wrong "It runs the script up to the open bracket"
    "slide checks that the brackets close before it takes a script at all.")
  (wrong "It closes the bracket for you"
    "slide does not guess where the bracket should go. Fix it in text and save again.")
  (hint "slide checks the script before taking it."))
```

---

## Definitions for every card

In the deck's script, define what the actions share:

```
(define score 0)
(define (bump n)
  (set! score (+ score n))
  (set-text "score" score))
```

Now a button's action can be just `(bump 10)`, and score is one
variable for the whole deck. Every card's actions see it.

```check
(number
  (ask "Starting from score 0, a button with the action `(bump 10)` is clicked twice and one with `(bump 5)` once. What does the box show?")
  (answer 25) (from "10 + 10 + 5")
  (hint "Each bump adds its n to score."))
```

---

## when-clicked

`(when-clicked "name" ...)` in the deck's script gives a control an
action from the script, on whichever card it is:

```
(when-clicked "again"
  (set! score 0)
  (go 'first))
```

A click on a control named again starts over. This keeps all of a
deck's workings in one place, to read and change together.

```sources
(source "Zeitlos docs: slide_scripting.md, Short forms")
```

```check
(choice
  (ask "A button named `again` has its own action as well. Which runs first when it is clicked?")
  (right "The when-clicked forms, then the button's own action")
  (wrong "Only the button's own action"
    "Both run: when-clicked's forms come before the control's own action.")
  (wrong "Only the when-clicked forms"
    "The button's own action still runs, after them.")
  (hint "The docs say when-clicked runs before the control's own action."))
```

---

## A card's script

A card can have a script too, which runs each time the card is
shown. `/script card` (or `/script` with nothing chosen) edits it. The
last card of a quiz can show the result as it arrives:

```
(set-text "result"
  (string-append "You scored "
                 (number->string score) " of 3."))
```

The example decks in `/data/slide/examples` work this way. Open
`quiz.zsl`: it opens as an app, and F7 shows how it is made.

```try
(try (app "slide")
  (task "In slide, open /data/slide/examples/quiz.zsl (Ctrl+O). Take the quiz, then press F7 and type /script deck to read its script in text. Come back here.")
  (check (choice (ask "Where does the quiz keep its score?")
    (right "In a variable, score, defined in the deck's script")
    (wrong "In a text box on each card" "The cards show the result, but the count itself is the variable score in the deck's script.")
    (wrong "In a file" "Nothing is written to a file: score is a variable while the deck is in use.")
    (hint "Look for define in the deck's script."))))
```

```check
(choice
  (ask "When does a card's script run?")
  (right "Each time the card is shown")
  (wrong "Once, when the deck is opened"
    "That is the deck's script. A card's script runs on arriving at its card.")
  (wrong "When a control on the card is clicked"
    "That is an action. A card's script runs as the card arrives.")
  (hint "It belongs to the card."))
```

So: the deck's script holds definitions and `when-clicked` handlers for
all the cards, and a card's script runs each time it is shown.
