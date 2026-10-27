# Notes, transitions and timing

```lesson
(objective "Give cards notes, transitions and timings, and make a show that runs by itself.")
(minutes 15)
(keywords "speaker's notes" "transition (between cards)" "millisecond")
```

Each card can have **speaker's notes**: words for you, the speaker,
that are not drawn on the card. With nothing selected, type the command
`notes` and the words:

`notes Thank everyone for coming.`

In a show, **T** has the notes of each card read aloud as it arrives,
through the speech service; T again stops it.

```sources
(source "slide (docs/slide_app.md), the commands; Showing a deck")
```

```check
(choice
  (ask "Who sees a card's notes during a show?")
  (right "Nobody on the screen: they can be read aloud with T")
  (wrong "Everyone, at the foot of the card" "Notes are not drawn on the card. In a show they can be spoken, with T.")
  (wrong "Only the first card's notes are shown" "Each card has its own notes, and none of them are drawn on the card.")
  (hint "Notes are for the speaker, not part of the drawing."))
```

---

## Transitions

A **transition** is how the show goes from one card to
the next. `transition wipe` sets it for the card you are on: the way
the show arrives at it.

| Transition | Looks like |
|---|---|
| cut | the new card at once |
| wipe | the new card uncovered from left to right |
| push | the new card pushes the old one off to the left |
| iris | a circle that opens |
| dissolve | the new card appears in small squares, scattered |

A number after it is how long it takes, in **milliseconds**
(thousandths of a second): `transition iris 500` takes half a second.

```check
(number
  (ask "You want a dissolve that takes one and a half seconds. What number goes after transition dissolve?")
  (answer 1500) (from "1.5 * 1000")
  (wrong-if 1.5 "The number is in milliseconds: thousandths of a second.")
  (wrong-if 150 "150 milliseconds is less than a fifth of a second. A second is 1000.")
  (hint "1 second is 1000 milliseconds."))
```

---

## A show that runs itself

- `advance 5` makes the show go on from this card by itself after 5
  seconds; `advance 0` waits for you again.
- `loop on` makes the show go round: after the last card, the first.
- `hide`, with something selected, keeps it out of the show. In the
  editor it is still there, screened, and `unhide` brings it back.

A deck with `advance` on every card and `loop on` runs by itself, for a
screen in a window or at a stand.

```sources
(source "slide (docs/slide_app.md), the commands; A deck as an app")
```

```check
(choice
  (ask "A deck has 4 cards, each with advance 10, and loop on. What does its show do after a minute?")
  (right "It is going round again: each card shows for 10 seconds")
  (wrong "It has stopped on the last card" "With loop on, the show goes from the last card back to the first.")
  (wrong "It is still on the first card" "advance 10 makes each card go on by itself after 10 seconds.")
  (hint "4 cards of 10 seconds each take 40 seconds. Then what does loop do?"))
```

---

```example
(example
  (problem "Card 2 should come in with a 300-millisecond wipe and stay for 8 seconds.")
  (step "Go to card 2, with nothing selected.")
  (step "Type /transition wipe 300 and Enter.")
  (step (text (ask "Then what command makes it go on after 8 seconds?")
          (accept "advance 8") (accept "/advance 8")
          (wrong-if "advance 8000" "advance takes seconds, not milliseconds.")
          (hint "The command is advance, and its number is seconds.")))
  (answer "transition wipe 300, then advance 8."))
```

---

```check
(choice
  (ask "A note to yourself is drawn as a text box at the foot of a card, and you do not want people to see it. What are two ways? Choose both.")
  (several)
  (right "Put the words in the card's notes with the notes command, and delete the box")
  (right "Select the box and type /hide")
  (wrong "Make the text box's words size 4" "Small words are still drawn on the card for everyone to see.")
  (wrong "Move it to the next card" "Then it is drawn on the next card instead.")
  (hint "One keeps the words off the card; the other keeps the box out of the show."))
```

So: notes are for the speaker, a transition is how a card arrives,
`advance` and `loop` make a show run itself, and `hide` keeps a thing
out of it.
