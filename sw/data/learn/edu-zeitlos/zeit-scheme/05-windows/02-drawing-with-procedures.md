# Drawing with procedures

```lesson
(objective "Draw many shapes with loops and procedures: a pattern, and a bar chart from a list.")
(minutes 20)
```

Drawing procedures are called like any others, so a loop can draw
many shapes. This draws a line every 20 pixels across the drawing
area:

```
(let loop ((x 0))
  (if (< x 196)
      (begin (line w x 0 x 104 1)
             (loop (+ x 20)))))
```

x is 0, 20, 40 and so on; each time round, a line from the top to the
bottom at that x.

```sources
(source "Zeitlos docs: repl.md, Windows and drawing")
```

```check
(number
  (ask "How many lines does that loop draw?")
  (answer 10)
  (true "180 < 196") (true "200 >= 196")
  (wrong-if 9 "Count x as 0 too: 0, 20, 40 ... 180 is ten values.")
  (wrong-if 196 "x goes up 20 at a time, not 1.")
  (hint "x takes the values 0, 20, 40 ... up to 180."))
```

---

## A bar chart

A list of numbers can become a bar chart. Each bar stands on the line
y = 80, and its top is 8 pixels higher for each 1 of its number:

```
(define (bars l x)
  (if (pair? l)
      (begin
        (box w x (- 80 (* 8 (car l)))
             (+ x 15) 80 1)
        (bars (cdr l) (+ x 20)))))
```

`(bars '(3 7 5 9 2) 10)` draws this:

```figure
(zs-area-win 196 105 "Bars")
(let loop ((l '(3 7 5 9 2)) (x 10))
  (if (pair? l)
      (begin (zs-box x (- 80 (* 8 (car l))) (+ x 15) 80 'ink)
             (loop (cdr l) (+ x 20)))))
(zs-line 0 81 195 81)
(caption "(bars '(3 7 5 9 2) 10)")
```

```check
(number
  (ask "In that chart, at what y is the top of the bar for 9?")
  (answer 8) (from "80 - 8 * 9")
  (wrong-if 72 "72 is how far the top is above y = 80. y counts down from the top of the area: 80 - 72.")
  (hint "The box's top is (- 80 (* 8 (car l)))."))
```

---

## Why y is taken away

On the screen, y grows downwards. A bar that grows upwards from y =
80 must start at a smaller y the taller it is: 80 minus 8 for each 1
of its number. Each bar is its own `box` from that top down to 80,
both included.

The bars move along 20 pixels at a time, and each is 16 wide (from x
to x + 15, both included), so there are 4 pixels between them.

```check
(number
  (ask "With x starting at 10, what is the left edge x of the third bar?")
  (answer 50) (from "10 + 2 * 20")
  (wrong-if 30 "That is the second bar. The first is at 10, then 30, then 50.")
  (wrong-if 60 "The bars start at 10, not 0: 10, 30, 50.")
  (hint "Add 20 for each bar before it."))
```

---

## Labels

`text` needs a string, so a number is turned into one first. This
writes each number under its bar:

```
(define (labels l x)
  (if (pair? l)
      (begin
        (text w x 72 (number->string (car l)) 1)
        (labels (cdr l) (+ x 20)))))
```

That y puts the text over the bars, though: `text` places its top
left corner at the point, and its letters are 12 pixels tall, so text
at y = 72 covers 72 to 83.

```check
(choice
  (ask "To write the labels under the bars, below y = 80, which y would you give text?")
  (right "82")
  (wrong "68"
    "68 is above the line the bars stand on: the text would sit on the bars.")
  (wrong "80"
    "The bars reach down to y = 80, so text there would touch them. Start a little below.")
  (hint "text's y is the top of its letters."))
```

---

## Practice

```check
(choice
  (ask "You call `(bars '(4 4) 10)` twice in the same window. What do you see?")
  (right "Two bars: the second call draws over the first in the same places")
  (wrong "Four bars"
    "The second call starts at x 10 again, so it draws exactly where the first did.")
  (wrong "Nothing: the second call rubs the first out"
    "Both calls draw in colour 1. Drawing the same box again changes nothing.")
  (hint "Where does the second call start?"))
```

```check
(number
  (ask "How many pixels tall is the bar for 3, drawn from y = 56 to y = 80, both included?")
  (answer 25) (from "80 - 56 + 1")
  (wrong-if 24 "Both ends are included: from 56 to 80 is 25 pixels.")
  (hint "Count from 56 to 80, both ends included."))
```

So: a loop or a recursion over a list can draw a whole picture, one
`box`, `line` or `text` at a time.
