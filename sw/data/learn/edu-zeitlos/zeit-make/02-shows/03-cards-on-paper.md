# Cards on paper

```lesson
(objective "Print a deck, or some of its cards, several to a sheet, or to a PDF file.")
(minutes 15)
(keywords "PDF")
```

A card prints as it is on the screen, drawn again at the printer's
finer dots: words and shapes are sharp at any size, and hidden things
are left out.

**Ctrl+P**, or the command `print`, prints the whole deck on the
printer set up for this computer, one card to a sheet. The print
service takes it from there in a window of its own, so you can go on
working; the status line says how it ended.

```sources
(source "slide (docs/slide_app.md), Printing")
```

```check
(choice
  (ask "You print a deck. Can you go on working in slide while it prints?")
  (right "Yes: the print service does the printing")
  (wrong "No: slide waits until the last sheet is out" "slide hands the deck to the print service, which works in a window of its own.")
  (wrong "Only if the deck has one card" "The size of the deck does not matter: slide goes on working either way.")
  (hint "Who does the printing, slide or something else?"))
```

---

## Which cards, how many

`print` takes words, in any order:

| Word | |
|---|---|
| this, 3, 2-5, all | which cards |
| 4up (from 1up to 16up) | how many cards to a sheet |
| cut | marks to cut along between them |
| copies 3 | how many of each |

So `print this copies 2` prints the card you are on, twice, and `print
6up` makes handouts, six cards to a sheet.

```check
(text
  (ask "What do you type after the / to print cards 2 to 4 only?")
  (accept "print 2-4") (accept "/print 2-4")
  (wrong-if "print 2 4" "Write a range with a dash between its ends: 2-4.")
  (hint "The command print, then the first and last card with a dash."))
```

---

## A PDF instead

A **PDF** is a file that holds pages, to send to someone or print
later, almost anywhere. `print pdf` writes the deck to one, a page for
each card, each the card's own shape. slide asks where to save it,
offering the deck's name with `.pdf`. `print talk.pdf` writes it
straight to that file, beside the deck.

```check
(choice
  (ask "You have no printer, and want to send your slides to a friend. What do you type after the /?")
  (right "print pdf")
  (wrong "save" "save writes the deck itself, a .zsl file, which needs slide to open.")
  (wrong "print 4up" "That still goes to a printer. pdf sends it to a file instead.")
  (hint "Which word sends it to a file?"))
```

---

## Index cards

A deck can be cards to cut out. `page 3x5` gives every card the shape
of an index card. Then `print setup 4up cut` sets the deck to print four
to a sheet with cut marks, and prints nothing yet.

Settings such as `4up` and `cut` are kept in the deck's file, so it
prints the same way every time. Which cards, how many copies, and `pdf`
are for that time only.

```check
(choice
  (ask "You used print setup 4up cut on a deck yesterday. Today you press Ctrl+P. How does it print?")
  (right "Four to a sheet, with cut marks")
  (wrong "One card to a sheet" "print setup keeps the settings in the deck, so they are used every time.")
  (wrong "It asks for the settings again" "They were kept in the deck's file: no need to ask.")
  (hint "Where were the settings kept?"))
```

---

```check
(number
  (ask "A deck of 12 cards is set to 4up. How many sheets of paper does it print on?")
  (answer 3) (from "12 / 4")
  (wrong-if 12 "That is one card a sheet. 4up puts four on each.")
  (wrong-if 48 "That multiplies. Four cards share each sheet, so divide.")
  (hint "Four cards to each sheet."))
```

So: Ctrl+P prints the deck; words after `print` choose which cards, how
many to a sheet and how many copies; `print pdf` makes a file instead.
