# The mouse

```lesson
(objective "Point, click, double-click, drag and scroll with the mouse.")
(minutes 15)
(keywords "pointer (of the mouse)" "click (with the mouse)" "double-click" "drag (with the mouse)" "scroll wheel")
```

This lesson is about the mouse: the four things you do with it.

As you move the mouse on the table, a small shape moves on the screen
in the same direction. That shape is the **pointer**. On Zeitlos it is
a small X. While the computer is still starting up it is a Z instead,
to say "wait".

The pointer is drawn so that it shows up on any background: each dot of
it is the opposite of the dot under it, lit on dark and dark on lit.

```sources
(source "The hardware cursor (docs/cursor.md)")
(source "sw/apps/wm/wm.c: the busy pointer is set while the desktop starts")
```

```check
(choice
  (ask "The pointer is a Z, not an X. What does that tell you?")
  (right "The computer is busy starting up: wait a moment")
  (wrong "The mouse is broken" "A broken mouse would not move the pointer at all. The Z is the computer saying it is still starting up.")
  (wrong "You are pointing at something you can type in" "The Z has nothing to do with where the pointer is. It shows while the desktop is still starting.")
  (hint "Z is the shape that says \"wait\"."))
```

---

## Click and double-click

To **click** is to press the mouse's left button once and let it go,
with the pointer over something. A click chooses or presses what is
under the pointer: a button, an icon on the dock, a window.

To **double-click** is to click twice quickly, without moving the
mouse between the two clicks. A double-click usually opens what you
point at.

Press and let go briskly. The two clicks of a double-click must come
close together in time and in place, or they count as two separate
clicks.

```sources
(source "Welcome to Zeitlos (docs/welcome.md), Using the mouse")
(source "Zeitlos Window Manager (docs/window_manager.md), Maximize and shade: double clicks")
```

```check
(choice
  (ask "You click once on something, wait a few seconds, and click it again. What did you do?")
  (right "Two separate clicks")
  (wrong "A double-click" "A double-click is two clicks close together in time. With a few seconds between them, they are two separate clicks.")
  (wrong "One click" "Each press and let-go is a click, so you made two. With seconds between them they are two separate clicks.")
  (hint "How close together must the two clicks of a double-click be?"))
```

---

## Drag

To **drag** is to press the button, keep it held down while you move
the mouse, and let go where you want to stop. You drag to move things:
a window, a slider, a point in a figure.

Try it in this figure: point at the ring, press the button, move the
mouse with the button held, and let go. The box follows the ring.

```figure
(define p (drag 'box 80 90))
(define bx (clamp (car p) 30 250))
(define by (clamp (cadr p) 20 170))
(frame (- bx 30) (- by 20) 100 60 'ink 1.5)
(text (+ bx 20) (+ by 15) "drag me" 'small 'centre)
(caption "Hold the button down while you move.")
```

```check
(choice
  (ask "You press the button over the ring, move the mouse, and let go. What did you just do?")
  (right "A drag")
  (wrong "A click" "A click lets go of the button without moving. Holding it down while you move is a drag.")
  (wrong "A double-click" "A double-click is two quick presses with no movement. This was one press, held while moving.")
  (hint "Was the button held down while the mouse moved?"))
```

---

## The scroll wheel

Many mice have a small wheel between the two buttons: the
**scroll wheel**. Turning it moves the view in the window under the
pointer: a long page, a list, a terminal's history. Each notch of the
wheel moves most apps three lines.

The wheel moves the *view*, not the place where you are typing: you
can scroll away to read and type again where you were.

Try it here: turn the wheel to scroll this step, if it is longer than
the screen.

```sources
(source "Zeitlos User Input (docs/user_input.md), Scroll wheel")
(source "Welcome to Zeitlos (docs/welcome.md), Using the mouse")
```

```check
(number
  (ask "In most apps, how many lines does one notch of the scroll wheel move the view?")
  (answer 3)
  (wrong-if 1 "One line a notch would be slow to scroll a long page. Zeitlos apps move three lines a notch.")
  (hint "More than one, fewer than five."))
```

---

```example
(example
  (problem "You want to move a box across the screen with the mouse.")
  (step "Point at the box.")
  (step "Press the left button and keep it down.")
  (step "Move the mouse: the box follows.")
  (step (choice (ask "The box is where you want it. What now?")
          (right "Let go of the button")
          (wrong "Click again" "The button is still held down. Letting go of it ends the drag; a click would start something new.")))
  (answer "Press, move with the button held, let go: a drag."))
```

```check
(choice
  (ask "Which one usually opens the thing you point at?")
  (right "A double-click")
  (wrong "Turning the scroll wheel" "The wheel moves the view of what is under the pointer; it does not open anything.")
  (wrong "Moving the mouse" "Moving the mouse only moves the pointer. To open something, you double-click it.")
  (hint "Two quick presses of the button."))
```

So the mouse moves the pointer; you click to choose, double-click to
open, drag to move, and turn the wheel to scroll.
