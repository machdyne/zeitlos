# Commands and styles

```lesson
(objective "Change how things on a card look with the bar and with slide's commands, and choose the card's shape.")
(minutes 20)
(keywords "command line (in slide)" "fill (of a shape)" "font (in slide)" "pattern (of black and white)")
```

The bar under the card changes what is selected:

- its **fill**, what its inside is painted with: none, or
  one of 16 **patterns**, small repeating arrangements of black and white;
- its line: how thick its outline is;
- its **font**, the style of letters its words are drawn in, and
  **B**, **I** and **U** for bold, italic and underlined.

Select something, then click in the bar.

```sources
(source "slide (docs/slide_app.md), Using it")
```

```check
(choice
  (ask "You click a pattern in the bar, but nothing on the card changes. What is the likely reason?")
  (right "Nothing was selected")
  (wrong "The pattern was the wrong colour" "Patterns are black and white. The bar changes the selection, so something must be selected first.")
  (wrong "Patterns only work on text" "Shapes have fills too: a fill is the inside of a shape or a text box. The bar changes only what is selected.")
  (hint "What does the bar change?"))
```

---

## The command line

Everything else is a command. Press **/** or **F4**, and the status line
becomes the **command line**: type a command and press
**Enter**. **Esc** backs out. What slide has to say about it comes back
in the same line.

A command works on the selection, or on the card when nothing is
selected.

| Command | Does |
|---|---|
| fill 3 | pattern 3 (0 to 15); fill none for none |
| fill shade 8 | an even grey, 0 white to 16 black |
| line 2 | an outline 2 thick; line 0 for none |
| font serif 24 bold | a font, a size, and its style |
| size 30 | only the size |
| align center | words to the middle of their box |

```sources
(source "slide (docs/slide_app.md), Using it (the commands)")
```

```check
(text
  (ask "A text box is selected. After pressing /, what do you type to make its words size 30?")
  (accept "size 30") (accept "/size 30")
  (wrong-if "30" "A number alone is not a command. Say what the number is for: size.")
  (hint "One word for what to change, then the number."))
```

---

## Fonts

slide's fonts are by name: `sans`, `serif`, `heavy`, `script`, `gothic`
and `symbol`, which draw well at any size, and some fixed-size ones
such as `m9x15`, which are sharpest at their own size.

```example
(example
  (problem "Make a title in a large, bold serif.")
  (step "Select the text box with v and a click.")
  (step "Press / and type font serif 40 bold, then Enter.")
  (answer "The title is serif, size 40, bold."))
```

```check
(choice
  (ask "You type /font fancy 20 and slide says: No such font: fancy. What happened to the selection?")
  (right "Nothing: slide did not do the command")
  (wrong "It is now in a font called fancy" "slide said there is no such font, so it could not use it.")
  (wrong "It is now size 20 in its old font" "slide did not do the command at all, so the size did not change either.")
  (hint "Read what slide said in the status line."))
```

---

## The / menu

You need not remember every command. Press **/** a second time at the
start of the empty line (or F4 again), and the **/ menu** lists them
all over the card. Up and Down move; Enter picks one. A command that
needs nothing more runs at once; one that takes words goes on the line
for you to finish.

`/tour` opens the tour deck, which shows most of slide on its own cards.

```check
(choice
  (ask "From the card, which keys open the / menu?")
  (right "/ and then / again")
  (wrong "F1" "F1 is the help box. The menu is on the command line.")
  (wrong "Esc" "Esc backs out of the command line. It does not open anything.")
  (hint "The first / opens the line. What does a second / do at its start?"))
```

---

## The card's shape

Every card of a deck has the same shape, its **page**. Its long side is
always 512 units, and the short side follows the shape. A new deck is
`classic`, 512 x 342. Change it with `page`: `page 16:9`, `page a4
portrait`. What is on the cards is scaled to fit.

```figure
(define k (choice 'page '("classic" "4:3" "16:9" "a4") "page"))
(define h (list-ref '(342 384 288 362) k))
(define w 205)
(define hh (* h 0.4))
(rect 58 (- 110 (/ hh 2)) w hh 'grey)
(frame 58 (- 110 (/ hh 2)) w hh 'ink 1.5)
(text 160 (+ 110 (/ hh 2) 16) "512 units" 'centre 'small)
(text 286 114 (fmt h 0) 'centre 'small)
(caption "The long side is always 512 units.")
```

```sources
(source "slide (docs/slide_app.md), The deck")
```

```check
(number
  (ask "A deck's page is 16:9, landscape. How many units tall is a card?")
  (answer 288) (from "512 * 9 / 16")
  (wrong-if 512 "512 is the long side, across. The short side is 9/16 of it.")
  (wrong-if 342 "342 is the classic page. A 16:9 page is 512 across and 9/16 of that tall.")
  (hint "512 units across. Tall is 9/16 of 512."))
```

So: select something and use the bar, or press / for a command; / twice
lists them all; and `page` sets the shape of every card.
