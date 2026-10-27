# Animation

```lesson
(objective "Make a small animation in pix frame by frame, play it, and use onion skin to line each frame up with the one before.")
(minutes 20)
(keywords "animation" "frame (of an animation)" "onion skin")
```

An **animation** is a run of pictures shown one after another, quickly,
so that what they show seems to move. Each picture is a
**frame**.

In pix, each frame is a cell of a sprite sheet. **Ctrl+D** makes the
next frame: a copy of the cell you are on, put right after it and named
one up, so `walk1` makes `walk2`. Then change the copy a little.

```sources
(source "pix (docs/pix_app.md), Animation")
```

```check
(text
  (ask "You are on a cell named jump3 and press Ctrl+D. What is the new cell called?")
  (accept "jump4")
  (wrong-if "jump3" "Two cells cannot share a name. The copy is named one up.")
  (wrong-if "jump31" "The number at the end goes up by one; it does not get a digit added.")
  (hint "The copy is named one up from jump3."))
```

---

## Play: F5

**F5** plays the animation of the cell you are on, over and over, in
the preview at the bottom left. F5 again stops it. It goes on while you
draw, so you can see a change the next time that frame comes round.

A cell that is in no animation yet plays every cell of its size, in
order, each for 100 milliseconds.

In this lesson F5 is learn's hint key; in pix, it plays.

```check
(choice
  (ask "You press F5 in pix and the status line says: One frame: nothing to play. What do you do?")
  (right "Press Ctrl+D to make a second frame, and change it")
  (wrong "Press F5 again, harder" "An animation needs more than one frame. Ctrl+D makes the next.")
  (wrong "Make the sprite bigger" "Size does not matter. An animation needs more than one frame.")
  (hint "What does a one-frame animation lack?"))
```

---

## Onion skin: F6

To make a movement smooth, each frame should differ a little from the
one before. **Onion skin** shows the frame before under the cell you
draw, as dots, where the cell is clear. **F6** turns it on; F6 again
adds the frame after; a third time turns it off.

```figure
(define k (slider 'frame 1 4 2 "frame" 1))
(define onion (toggle 'onion "F6: onion skin" #t))
(define pos '((0 1) (2 3) (4 5) (6 3)))
(define (ball f) (list-ref pos (- f 1)))
(define (cells b) (let ((x (car b)) (y (cadr b))) (list (list x y) (list (+ x 1) y) (list x (+ y 1)) (list (+ x 1) (+ y 1)))))
(define (has l c) (and (pair? l) (or (equal? (car l) c) (has (cdr l) c))))
(define now (cells (ball k)))
(frame 80 20 160 160 'ink 1)
(for-range 1 8 (lambda (i) (line (+ 80 (* i 20)) 20 (+ 80 (* i 20)) 180 'grey 0.5) (line 80 (+ 20 (* i 20)) 240 (+ 20 (* i 20)) 'grey 0.5)))
(if (and onion (> k 1))
    (for-each (lambda (c) (if (not (has now c)) (dot (+ 80 (* (car c) 20) 10) (+ 20 (* (cadr c) 20) 10) 'ink)))
              (cells (ball (- k 1)))))
(for-each (lambda (c) (rect (+ 80 (* (car c) 20)) (+ 20 (* (cadr c) 20)) 20 20 'ink)) now)
(caption (string-append "frame " (fmt k 0) (if (and onion (> k 1)) "; dots: the frame before" "")))
```

```check
(choice
  (ask "With onion skin on, what are the dots in the figure?")
  (right "Where the ball was in the frame before")
  (wrong "Pixels of the frame that are not finished" "The dots are not part of this frame. They show the frame before, as a guide.")
  (wrong "Where the ball will be next" "On the first F6 the dots are the frame before. A second F6 adds the frame after.")
  (hint "Move the slider: where were the dots one frame ago?"))
```

---

## Naming an animation: Shift+F5

**Shift+F5** shows the animation as a line of words, for you to change:
its name, how many milliseconds each frame shows, and its cells in
order. A cell may come more than once:

`run 120 mouse_stand mouse_run1 mouse_run2 mouse_run1`

A sheet can hold several animations, each its own line. They are kept in
the sprite sheet's file.

```check
(number
  (ask "The animation walk 200 a b c d shows the cells a, b, c and d, a frame each. How long does it take to play once through, in milliseconds?")
  (answer 800) (from "4 * 200")
  (wrong-if 200 "200 is how long each frame shows. There are four of them.")
  (hint "Four cells, 200 milliseconds each."))
```

---

```example
(example
  (problem "Make a coin that spins, a frame in each of 4 cells.")
  (step "Draw the coin, round, in the cell coin1.")
  (step "Press Ctrl+D: coin2. Make it narrower.")
  (step "Ctrl+D again: coin3, narrower still; and coin4, a thin edge. F6 shows the frame before to keep each in the middle.")
  (step (choice (ask "Then how do you see it spin?")
          (right "Press F5: it plays in the preview")
          (wrong "Press F11" "F11 is the whole screen. F5 plays the animation.")))
  (answer "Four cells made with Ctrl+D, lined up with F6, played with F5."))
```

So: each frame is a cell, Ctrl+D makes the next, F5 plays them in the
preview, F6 shows the frame before as onion skin, and Shift+F5 names
the animation and its speed.
