# A deck as an app

```lesson
(objective "Use a deck as an app with F7, make a deck open as one, and look inside the example decks.")
(minutes 15)
```

**F7** in slide switches between editing a deck and using it as an
app: the window shows just the card, with its controls working, and no
tools, bar or status line. F7 again goes back to editing. It works for
any deck.

Used as an app, a deck answers only what a reader needs: its controls,
the arrow keys and PageUp and PageDown for the cards before and after,
Home and End, F5 for a show, and F1 for help.

```sources
(source "slide (docs/slide_app.md), A deck as an app")
(source "Function keys (docs/keys.md): slide F7")
```

```check
(choice
  (ask "You are using a quiz deck as an app and want to fix a typing mistake on a card. What do you press?")
  (right "F7, to edit the deck")
  (wrong "F5" "F5 starts a show. F7 switches to editing.")
  (wrong "F2" "Used as an app, the deck does not edit. F7 first, then fix it.")
  (hint "The same key goes both ways."))
```

---

## A deck that opens as an app

The command `app on` marks the deck so that it opens as an app: a quiz,
a form, a small calculator. `app off` makes it open for editing again.
Save the deck afterwards, since the mark is kept in its file.

`loop on` makes going on from the last card go round to the first.

```check
(choice
  (ask "You type /app on but do not save, then close slide. How does the deck open next time?")
  (right "For editing, as before: the mark was not saved")
  (wrong "As an app" "The mark is kept in the deck's file, and the file was not saved.")
  (wrong "It cannot be opened" "Nothing is wrong with the file; it simply does not have the mark.")
  (hint "Where is the app mark kept?"))
```

---

## What it keeps

While a deck runs as an app, what people do to it, such as ticking a
box or typing in a field, is not saved in the deck, unless the deck is
edited and saved. Each time it opens, it starts as it was made.

(A deck can save its own records on purpose, with a script; the
next lesson begins scripts.)

```check
(choice
  (ask "Someone types their name in a field of a deck used as an app, then closes it. Is the name in the deck's file?")
  (right "No: values changed while it runs are not saved")
  (wrong "Yes: slide saves every change" "Not while it runs as an app. A script in the deck can save records on purpose.")
  (wrong "Only if they pressed Enter" "Enter runs the field's action; it does not save the deck.")
  (hint "What does slide keep from a deck used as an app?"))
```

---

## The example decks

In `/data/slide/examples` are decks made to be used. Each opens as an
app, and F7 shows how it is made:

| Deck | Is |
|---|---|
| quiz.zsl | three questions, with a score |
| calculator.zsl | a calculator of sixteen buttons |
| todo.zsl | a list of things to do |
| phonebook.zsl | names and numbers, kept in the deck |
| clock.zsl | a clock and a stopwatch |

```sources
(source "Scripting slide (docs/slide_scripting.md), Examples")
```

```check
(choice
  (ask "You open calculator.zsl and want to see how its buttons work. What do you do?")
  (right "Press F7, select a button with v, and type /action")
  (wrong "Open it in text" "It is a text file, but slide shows each button's action more simply: F7, select it, /action.")
  (wrong "Nothing: an app cannot be looked inside" "These decks are made to be looked inside. F7 shows how.")
  (hint "Which key shows how a deck is made?"))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Press Ctrl+O and open /data/slide/examples/quiz.zsl. Answer its three questions. Then press F7. Then come back.")
  (check (choice
    (ask "After F7, what did slide show?")
    (right "The tools, the bar and the status line: the deck to edit")
    (wrong "The next question" "F7 switches between using the deck and editing it.")
    (wrong "A show on the whole screen" "That is F5. F7 shows the deck to edit."))))
```

So: F7 switches between using a deck and editing it, `app on` makes a
deck open as an app, and the example decks show how theirs are made.
