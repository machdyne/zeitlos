# How a lesson goes

```lesson
(objective "Move through a lesson's steps and know what the parts of the screen are.")
(minutes 5)
```

A lesson is a few **steps**. Each step teaches one idea, and then asks
you something about it.

The dots at the top right of the screen show the steps: a filled dot
for each step done, a big dot for the step you are on.

Press **Space** or **Enter** to go to the next step, and **Backspace**
to go back one.

---

## When a step is long

If a step does not fit on the screen, Space shows the rest of it
first. You can also scroll with the arrow keys or the mouse wheel.

```note
(note "Nothing is timed. Take as long as you like on a step.")
```

---

```check
(choice
  (ask "Which key goes to the next step?")
  (right "Space (or Enter)")
  (wrong "Backspace" "Backspace goes back one step.")
  (wrong "Esc" "Esc leaves the lesson. Your place is kept, so you can come back to it.")
  (hint "It is the biggest key on the keyboard."))
```
