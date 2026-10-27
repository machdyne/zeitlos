# Giving a show

```lesson
(objective "Show a deck on the whole screen and move through it with the keys and the mouse.")
(minutes 15)
(keywords "slide show")
```

A **slide show**, or show for short, puts the cards on the whole screen, one at a
time, as slides. In slide:

- **F5** starts the show from the card you are on;
- **Shift+F5** starts it from the first card.

These are slide's keys. In this lesson, F5 still gives you a hint: each
app has its own meaning for F5 to F8.

```sources
(source "slide (docs/slide_app.md), Showing a deck")
(source "Function keys (docs/keys.md)")
```

```check
(choice
  (ask "You are editing card 6 of a talk and want to start the show from the very beginning. What do you press?")
  (right "Shift+F5")
  (wrong "F5" "F5 starts from the card you are on, card 6. With Shift, the show starts from the first.")
  (wrong "F6" "F6 shows all the cards small, the sorter. It is not a show.")
  (hint "F5 with one more key held."))
```

---

## Going on and back

| To | Press |
|---|---|
| the next card | Space, Right, Down, PageDown, Enter, or a left click |
| the card before | Left, Up, PageUp, Backspace, or a right click |
| the first, the last | Home, End |
| card 12 | type 12, then Enter |

The mouse wheel goes both ways.

```check
(choice
  (ask "During a show, you type 4 and then press Enter. What happens?")
  (right "The show goes to card 4")
  (wrong "The show goes on by four cards" "A number and Enter go to that card, counted from the first.")
  (wrong "Only the next card is shown" "Enter alone goes on one card. With a number typed first, it goes to that card.")
  (hint "A number and Enter: that card."))
```

---

## While you talk

- **B** blanks the screen black, and **W** white; the same key again
  brings the card back. Useful when you want people to look at you.
- **S** shows the card at its own size, not stretched to fill the
  screen.
- **Esc** or **Q** ends the show. You are back in the editor, on the
  card the show ended on.

If anything goes wrong, **Super+Esc** always takes the screen back to
the desktop.

```sources
(source "slide (docs/slide_app.md), Showing a deck")
```

```check
(choice
  (ask "You end a show with Esc while card 9 is on the screen. Where are you in slide?")
  (right "Editing card 9")
  (wrong "Editing card 1" "The editor is left on the card the show ended on, so you can fix it at once.")
  (wrong "On the desktop, with slide closed" "Esc ends the show, not slide.")
  (hint "The show ends where it is."))
```

---

## Fast cards

The next card is drawn while this one is on the screen, a part at a
time, so going on is quick even for a card that takes long to draw. A
key or a click during a change of card finishes it at once.

```check
(number
  (ask "A show is on card 3 of 10. You press End, then Left. Which card is on the screen?")
  (answer 9) (from "10 - 1")
  (wrong-if 2 "End goes to the last card first. Left then goes back one from there.")
  (wrong-if 10 "End goes to card 10, and Left goes back one.")
  (hint "End: the last card. Left: one back."))
```

---

```try
(try (app "slide")
  (task "Open slide from the dock and type /tour then Enter: the tour deck. Press Shift+F5 for its show. Go on with Space a few times, press B twice, then Esc. Then come back.")
  (check (choice
    (ask "What did the first B do?")
    (right "It made the screen black")
    (wrong "It went back a card" "Going back is Left, Up, PageUp or Backspace. B blanks the screen.")
    (wrong "It ended the show" "Esc or Q ends the show. B blanks it, and B again brings the card back."))))
```

So: F5 shows from here and Shift+F5 from the start; Space goes on,
Left goes back, B blanks, and Esc ends.
