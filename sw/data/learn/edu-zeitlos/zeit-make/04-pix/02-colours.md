# Colours

```lesson
(objective "Choose and swap pix's two drawing colours, and see the art in colour and with fewer colours.")
(minutes 20)
(keywords "palette" "see-through (pixel)")
```

A sprite sheet has a **palette**: the list of colours its pixels can
be, 2, 4, 16, 64 or 256 of them, each with a number.

You draw with two of them at once. The **left** mouse button draws the
**first colour**, and the **right** button the **second**. Click a
colour in the side column with the left button to make it the first,
or with the right button to make it the second.

```sources
(source "pix (docs/pix_app.md), Colour")
```

```check
(choice
  (ask "The first colour is black and the second is yellow. You draw with the right button. Which colour do you get?")
  (right "Yellow")
  (wrong "Black" "The left button draws the first colour. The right draws the second.")
  (wrong "Both, mixed" "Each button draws one colour: right is the second.")
  (hint "Left: first. Right: second."))
```

---

## Keys for colours

The side column shows a page of 16 colours. The keys **0** to **9** and
**A** to **F** pick one of them as the first colour: 0 is the first of
the page, F the sixteenth. **X** swaps the first and the second.

In a sprite sheet, **.** (the full stop) picks a **see-through** pixel:
a pixel with no colour at all, where whatever is behind the sprite shows
in a game. pix draws it as a checker. In a new sheet the second colour
starts as see-through, so the right button rubs out.

```check
(choice
  (ask "You want the right button to draw red, which is your first colour now. What is quickest?")
  (right "Press X")
  (wrong "Press the full stop" "That picks see-through as the first colour. X swaps the first and second.")
  (wrong "Press F" "F picks the sixteenth colour of the page as the first colour, not the second.")
  (hint "One key swaps the two colours."))
```

---

## In the window, and on the whole screen

The desktop's windows are black and white. So in its window, unless
the computer gives it colour, pix gives each colour
a pattern of its own while you draw, so two colours never look the
same; the preview and the strip show each colour as the grey it would
be.

- **F11** puts pix on the whole screen, which shows the art in colour
  on a computer whose screen can show it.
- **F10** turns colour on and off in the window, on computers that can
  show colour there.

```sources
(source "pix (docs/pix_app.md), Colour; pix's help (F1)")
(source "Function keys (docs/keys.md): F10 colour, F11 full screen")
```

```check
(choice
  (ask "In pix's window, two colours of your sprite have different patterns. Will they look different in the game?")
  (right "Yes: the patterns stand for different colours")
  (wrong "No: patterns are not colours" "Each pattern stands for one colour of the palette. Different patterns, different colours.")
  (wrong "Only if they have the same pattern" "The same pattern would mean the same colour.")
  (hint "Why does pix give each colour its own pattern?"))
```

---

## Fewer colours: F7

Not every computer shows as many colours. Each colour of a sheet says
what stands in for it on a screen with fewer: one at 64, at 16, at 4,
and a grey at 2.

**F7** shows the sheet as 64, 16, 4 and 2 colours in turn, then as it
is again. Only what you see changes; the art is not touched.

```check
(choice
  (ask "You press F7 until the sprite shows in 2 colours. Has your art lost its colours?")
  (right "No: F7 changes only how it is shown")
  (wrong "Yes, until you undo" "F7 never changes the art, only the view of it.")
  (wrong "Yes, once you save" "What you save is the art, which F7 does not touch.")
  (hint "What does F7 change: the art or the view?"))
```

---

```try
(try (app "pix")
  (task "Open pix from the Tools group in the dock. Press 4, then draw on the canvas with the left button; then draw with the right button. Press X and draw with the right button again. Then come back.")
  (check (choice
    (ask "After X, what did the right button draw?")
    (right "Colour 4, the one the left button drew before")
    (wrong "See-through, as before" "X swapped the two colours, so the right button now draws what the first colour was.")
    (wrong "Nothing at all" "The right button always draws the second colour; after X that is the old first."))))
```

So: the left button draws the first colour and the right the second;
0-9 and A-F pick, X swaps; patterns stand for colours in the window,
and F7 shows the art with fewer.
