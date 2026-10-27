# Controls on a card

```lesson
(objective "Put buttons, check boxes, radio buttons, fields, sliders and lists on a card, and use them with the browse tool.")
(minutes 20)
(keywords "control (on a card)" "browse tool (in slide)")
```

A card can hold more than drawing. A **control** is
something to click or type into. Each is placed by a command, in the
middle of the card, to move and size like any other object:

| Command | Places |
|---|---|
| button Next | a button with the words Next |
| check Sound | a check box, ticked or not |
| radio Red | a radio button: one of a set, only one chosen |
| field | a field to type into |
| slider 0 10 | a slider, from 0 to 10 (0 to 100 if not given) |
| list Tea, Coffee, Milk | a list to choose one from |

```sources
(source "slide (docs/slide_app.md), Controls and actions")
```

```check
(choice
  (ask "A form asks Which size? Small, Medium or Large, and only one may be chosen. Which controls fit?")
  (right "Three radio buttons")
  (wrong "Three check boxes" "Check boxes can all be ticked at once. Radio buttons allow only one.")
  (wrong "Three fields" "A field is for typing words. Here the person only chooses.")
  (hint "Which control is one of a set, only one chosen?"))
```

---

## The browse tool

With the select tool, a control is an object like any other: you move
it, size it and style it. To use it, pick the **browse tool (b)**: a
click on a button presses it, a click on a check box ticks it, and you
can type into a field.

With the browse tool, a click anywhere else on the card goes to the
next card, and Shift+click to the one before.

Controls also work in a show, and in a deck used as an app (a later
lesson).

```check
(choice
  (ask "You click a button and it is only selected, with handles. Why does it not press?")
  (right "The select tool is chosen: press b for browse")
  (wrong "The button has no words" "Words or not, a button only works with the browse tool, in a show, or in a deck used as an app.")
  (wrong "Buttons only work when printed" "A printed button cannot be pressed at all. It works with the browse tool.")
  (hint "With which tool do controls work?"))
```

---

## Radios that go together

On a card, all its radio buttons go together: choosing one un-chooses
the others. For two sets on one card, give each set a group name: select
its radios and type `group size`, then the other set's `group colour`.

A button's words, or a field's, are changed as a text box's are: F2,
Enter or a double-click. A check box's or a radio's words are changed
with the command `text`, such as `text Loud sound`.

```check
(choice
  (ask "A card has radios for size and radios for colour, with no groups. You choose Large, then Red. What happens to Large?")
  (right "It is un-chosen: all the card's radios are one set")
  (wrong "It stays chosen" "Without group names, all the radios on a card exclude each other.")
  (wrong "Both are chosen and the card goes on" "Radios never allow two chosen in one set.")
  (hint "With no groups, how many sets of radios does a card have?"))
```

---

## Fields, sliders and lists

- In a **field**, type in browse mode. In a field one line high, Enter
  does what the field is set to do (the next lesson); in a taller one, Enter starts a new
  line. Tab goes to the next field.
- A **slider**'s range is set with `range 1 5`, and its value with
  `value 3`.
- A **list**'s items are changed with `text Tea, Coffee, Milk`.

```check
(text
  (ask "A slider is selected. What command, after the /, makes it go from 1 to 12?")
  (accept "range 1 12") (accept "/range 1 12")
  (wrong-if "slider 1 12" "That places a new slider. To change the one selected, use range.")
  (wrong-if "value 12" "value sets where the slider is now. Its ends are its range.")
  (hint "The command for a slider's ends."))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock. Type /check Sound and Enter, then /slider 0 10 and Enter, then drag the slider apart from the check box with v. Press b, and click the check box, then drag the slider's knob. Then come back.")
  (check (choice
    (ask "What did the click on the check box do with the browse tool?")
    (right "It ticked the box")
    (wrong "It went to the next card" "A click on a control is the control's. A click elsewhere goes on.")
    (wrong "It selected it, with handles" "That is the select tool, v. With b, a click uses the control."))))
```

So: controls are placed by commands; the select tool arranges them,
and the browse tool, a show or a deck used as an app makes them work.
