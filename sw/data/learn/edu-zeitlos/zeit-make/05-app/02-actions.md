# Actions

```lesson
(objective "Give a control an action in plain words, and give a card an action for when it is reached.")
(minutes 20)
(keywords "action (of a control)")
```

An **action** is what a click on it does. Select the
control, then type the command `action` and the words:

`action go next`

Now, with the browse tool or in a show, a click on that button goes to
the next card. `action` alone says what the action is now, and `action
none` takes it away.

```sources
(source "slide (docs/slide_app.md), Controls and actions")
```

```check
(text
  (ask "A button is selected. What command, after the /, makes a click on it go to the first card?")
  (accept "action go first") (accept "/action go first")
  (wrong-if "go first" "That goes to the first card now. To make it the button's, start with action.")
  (hint "action, then what the click should do."))
```

---

## The words of an action

| Words | Does |
|---|---|
| go next (or prev, first, last, back) | another card; back: the one visited before |
| go 3, go results | a card by number or by name |
| show NAME, hide NAME, toggle NAME | shows or hides an object by its name |
| set-text NAME words | puts words in an object |
| speak words | says them aloud |
| run clock | starts an app |
| open /data/slide/other.zsl | opens another deck |
| wait 500 | waits half a second before the rest |

Several steps go in one action, with `;` between them:
`action speak Well done; wait 1000; go next`.

```check
(choice
  (ask "An action is: hide answer; go next. What happens when its button is clicked?")
  (right "The object named answer is hidden, then the next card is shown")
  (wrong "Only the first step runs" "Steps between ; run one after another, so both happen.")
  (wrong "slide says Not an action" "hide and go are both words of an action, so it runs.")
  (hint "The ; separates two steps."))
```

---

## Names matter

`show`, `hide` and `set-text` find an object by its name, on the card or
its background. Name it first: select it and type the command
`name answer`. (For a line or a picture, F2 starts that command.)

```example
(example
  (problem "A card asks What is 6 times 7? and a button shows the answer.")
  (step "Make a text box with 42 in it. Select it and type /name answer.")
  (step "Select it and type /hide: it is hidden in the show.")
  (step (text (ask "Make a button Show. With it selected, what command shows the answer when it is clicked?")
          (accept "action show answer") (accept "/action show answer")
          (wrong-if "show answer" "Start with action, so it is what the button does.")
          (hint "action, then show and the object's name.")))
  (answer "action show answer"))
```

---

## A card's own action

With nothing selected, `action` sets what happens on arriving at the
card. `action speak Welcome` greets each time the card is reached, in a
show or with the browse tool.

A check box runs its action after it changes, a radio button when it is
chosen, a slider when it is let go, a list when an item is chosen.

```check
(choice
  (ask "With nothing selected, you type /action speak Hello. When is Hello said?")
  (right "Whenever the card is arrived at")
  (wrong "When the card is printed" "Actions run when the deck is used: browsing, in a show, or as an app.")
  (wrong "Never: nothing was selected" "With nothing selected, the action is the card's: it runs on arriving.")
  (hint "Whose action is it, with nothing selected?"))
```

---

```check
(choice
  (ask "You type /action jump 3 and slide says: Not an action: jump. Why?")
  (right "jump is not one of the action words; go 3 is")
  (wrong "Card 3 does not exist" "slide did not get as far as the card. The word jump is the trouble.")
  (wrong "Actions cannot have numbers" "go 3 has a number. The first word must be one of the action words.")
  (hint "Compare jump with the words in the table."))
```

So: `action` gives a control, or with nothing selected the card, what
to do, in plain words with `;` between steps.
