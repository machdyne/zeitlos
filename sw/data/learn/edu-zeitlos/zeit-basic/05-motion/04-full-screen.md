# Full screen and colour

```lesson
(objective "Run BASIC full screen, get back to the window, and draw in colour with INK and PAPER.")
(minutes 15)
(keywords "game mode")
```

In this lesson you give BASIC the whole screen, and some colour.

**F11** makes BASIC full screen: its 320 by 240 screen fills the whole
display, as on a home computer plugged into a television. This is
**game mode**. F11 again brings back the window.

A program can do the same: `SCREEN 1` goes full screen, `SCREEN 0`
back to the window. On a computer without game mode, `SCREEN 1` stops
with `NOT SUPPORTED`.

In BASIC, Esc stays BASIC's in game mode too: it stops a program, and
does not leave full screen. **F11 is the way back.**

```sources
(source "docs/basic_app.md, Using it; Beyond BASIC 1 (SCREEN)")
(source "docs/keys.md, Full screen (F11): Game mode")
```

```check
(choice
  (ask "BASIC is full screen and a program is running. You press Esc. What happens?")
  (right "The program stops, and BASIC stays full screen")
  (wrong "BASIC goes back to the window"
    "In most apps Esc leaves game mode, but BASIC keeps Esc for stopping programs. F11 goes back to the window.")
  (wrong "Nothing"
    "Esc stops a running program, full screen or not.")
  (hint "Esc means the same in BASIC wherever it is shown."))
```

---

```check
(choice
  (ask "BASIC is full screen. Which key brings back its window?")
  (right "F11")
  (wrong "Esc"
    "In BASIC, Esc erases a line or stops a program, even in game mode. F11 switches back.")
  (wrong "F1"
    "F1 shows BASIC's help. F11 switches between full screen and the window.")
  (hint "The same key that made it full screen."))
```

---

## Sixteen colours

BASIC has 16 colours, numbered 0 to 15. 0 is black and 1 white; the
others include 2 red, 5 green, 6 blue and 7 yellow.

- `INK c` draws, and prints, in colour c.
- `PAPER c` is the colour text is printed on, and the colour `CLS`
  clears to.

```basic
10 PAPER 6: INK 1: CLS
20 FOR C = 2 TO 15
30 INK C: BOX C * 20, 40, C * 20 + 15, 200, 1
40 NEXT C
50 INK 7: LOCATE 27, 1: PRINT "SIXTEEN COLOURS"
```

```check
(number
  (ask "How many coloured bars does this program draw?")
  (answer 14) (from "15 - 2 + 1")
  (wrong-if 16 "The loop starts at 2, not 0: colours 2 to 15.")
  (wrong-if 13 "Both ends count: 2, 3, ... 15 is 14 colours.")
  (hint "C goes from 2 to 15."))
```

---

## Where the colours show

The colours are shown full screen on a computer whose game mode has
colour, and in the window on computers that can show all sixteen
colours there. **F10** turns the colour off and on.

Otherwise BASIC shows them in black and white: the paper colour black,
and every other colour white. So text and drawings stay readable on any
computer.

```sources
(source "docs/basic_app.md, Colour")
```

```check
(choice
  (ask "On a computer that shows BASIC in black and white, a program sets PAPER 6 and INK 2 and prints text. How does it look?")
  (right "White text on black")
  (wrong "Red text on blue"
    "That is how a computer that shows colour draws it. This one shows none: the paper as black, and the ink as white.")
  (wrong "Nothing can be seen"
    "BASIC makes sure it can: the paper is shown black and every other colour white.")
  (hint "In black and white, the paper colour is shown black, and the rest white."))
```

---

```check
(choice
  (ask "Which line makes the drawings that follow it yellow?")
  (right "INK 7")
  (wrong "PAPER 7"
    "PAPER is the colour behind text and what CLS clears to. INK is the colour things are drawn in.")
  (wrong "COLOR 7"
    "COLOR takes only 0, 1 and 2, and COLOR 7 is OUT OF RANGE. Colours are chosen with INK.")
  (hint "Drawing is done in the ink colour."))
```

So: F11 or `SCREEN 1` gives BASIC the whole screen, F11 brings back the
window, and `INK` and `PAPER` choose colours where they can be shown.
