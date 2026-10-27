# Actions

```lesson
(objective "Give a button an action in Scheme that reads and changes the card.")
(minutes 20)
(keywords "control (on a card)" "action (of a control)" "browse tool (in slide)")
```

A card can hold **controls** as well as drawing: buttons, check boxes,
radio buttons, fields to type into, sliders and lists. `/button Add`
puts a button labelled Add on the card.

What a click on a control does is its **action**. Choose the control
and give it one with `/action`. Simple actions are plain words:

```
/action go next
/action speak Hello
```

Anything that is not one of slide's plain words is Scheme, so an
action can compute:

```
/action (set-text "score" (+ 1 (string->number (get-text "score"))))
```

```sources
(source "Zeitlos docs: slide_app.md, Controls and actions")
```

```check
(choice
  (ask "A button's action is `go next`. Which card does a click show?")
  (right "The card after this one")
  (wrong "The card called next"
    "next is one of go's plain words: the card after this one. (go \"results\") goes to a card by name.")
  (wrong "The first card"
    "That is go first.")
  (hint "go next, go prev, go first, go last."))
```

---

## Clicking it

Controls work with the **browse tool**, the tool for using a deck
(press `b`), in a show (F5), and
in a deck run as an app (F7). With the other tools they are objects to
move and resize, and a click only chooses them.

So: give the button its action, press `b`, and click it. A mistake in
the action is said in the status line, after `Script:`, and the deck
goes on.

```sources
(source "Zeitlos docs: slide_scripting.md, When something goes wrong")
```

```check
(choice
  (ask "You click your button with the text tool chosen, and nothing counts. Why?")
  (right "Controls work only with the browse tool, in a show or in a deck run as an app")
  (wrong "The action has a mistake"
    "With a mistake, slide would say so in the status line. Here the action never ran.")
  (wrong "Buttons need a script for the whole deck first"
    "An action needs nothing else. It runs when the button is clicked in browse mode.")
  (hint "Which tool makes controls work?"))
```

---

## Controls' values

`get-value` reads a control: a check box gives `#t` or `#f`, a slider
its number, a list the item chosen. `set-value` changes one. `show`
and `hide` show and hide any named object:

```
/action (if (get-value "agree") (show "thanks") (hide "thanks"))
```

On a check box named agree, that action shows the text box thanks
when the box is ticked, and hides it when it is not.

```sources
(source "Zeitlos docs: slide_scripting.md, slide's procedures")
```

```check
(choice
  (ask "A slider named `size` is at 30. What is `(get-value \"size\")`?")
  (right "30")
  (wrong "#t"
    "#t and #f are what check boxes and radio buttons give. A slider gives its number.")
  (wrong "\"30\""
    "get-text gives text, a string. get-value gives a slider's value as a number.")
  (hint "A slider's value is a number."))
```

---

## Going to a card

`go` takes the same words in Scheme, with a quote, and a card's
number or name too:

| Scheme | goes to |
|---|---|
| (go 'next) | the card after this one |
| (go 'back) | the card visited before |
| (go 3) | card 3 |
| (go "results") | the card named results |

So an action can choose where to go: `(if (> (get-value "size") 50)
(go "big") (go "small"))`.

```check
(choice
  (ask "What does the action `(if (get-value \"agree\") (go 'next) (speak \"Tick the box first\"))` do when the box is not ticked?")
  (right "It says Tick the box first aloud, and stays on the card")
  (wrong "It goes to the next card"
    "(get-value \"agree\") is #f for a box not ticked, so if takes its second expression.")
  (wrong "Nothing at all"
    "if has a second expression for the false case: speak.")
  (hint "What does get-value give for a box that is not ticked?"))
```

---

## Practice

```check
(choice
  (ask "Which action makes a button add 5 to a text box named `total`?")
  (right "`(set-text \"total\" (+ 5 (string->number (get-text \"total\"))))`")
  (wrong "`(set-text \"total\" (+ 5 (get-text \"total\")))`"
    "get-text gives a string, and + needs numbers. Turn it into a number first.")
  (wrong "`(+ 5 (string->number (get-text \"total\")))`"
    "That works out the new total but never puts it on the card. set-text does that.")
  (hint "Read, turn into a number, add, and set."))
```

So: a control's action is plain words or Scheme, and runs when it is
clicked in browse mode, in a show or in a deck run as an app.
