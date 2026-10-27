# Figures

```lesson
(objective "Read a figure, change one with its controls, and drag a point in one.")
(minutes 8)
```

Many steps have a **figure** beside the words: a picture that learn
draws while you read.

This one shows two fractions as bars of the same length. The shaded
part of each bar is the fraction. Its question works like those in
[Answering questions](lesson:start/questions).

```figure
(frac-bar 0 4 3 'warm "3/4")
(frac-bar 1 4 1 'cool "1/4")
(caption "Each bar: one whole, in 4 equal pieces.")
```

```check
(choice
  (ask "Which fraction is greater?")
  (right "3/4" (true "3/4 > 1/4"))
  (wrong "1/4" (false "1/4 > 3/4") "Look at the bars: 3/4 has three pieces shaded, 1/4 has only one."))
```

---

## Controls

Some figures can be changed. Under this one is a **slider**: press
**Left** or **Right**, or drag it with the mouse, and the figure changes.

```figure
(define n (slider 'shaded 0 8 3 "Shaded" 1))
(frac-bar-at 40 60 240 40 8 n 'warm)
(math 160 160 (string-append (fmt n 0) "/8") 28)
(caption "The bar is cut into 8 equal pieces: eighths.")
```

```check
(number
  (ask "Move the slider until half of the bar is shaded. How many eighths is one half?")
  (answer 4)
  (wrong-if 2 "Two eighths is a quarter of the bar. Half the bar is more.")
  (hint "Half of 8 pieces."))
```

---

## Moving figures

A figure can also move. Press **P** to stop it, and **P** again to
start it.

This clock keeps time. Its thin second hand ticks once a second and
goes once around in a minute; each time it does, the minute hand moves
on by one mark.

```figure
(still (clock-dial 160 120 100))
(clock-ticking 160 120 100 3 0)
```

```check
(choice
  (ask "While the minute hand goes once all the way around, how long passes?")
  (right "One hour" (true "60 = 60"))
  (wrong "One minute" (false "60 = 1") "Once around is 60 minutes. The second hand goes around in one minute.")
  (wrong "Twelve hours" (false "60 = 720") "That is the hour hand going all the way around."))
```

---

## Points to drag

In some figures you can **drag a point** with the mouse. A ring shows
which points can move.

Drag the corners of this triangle. Its angles change, but their total
does not.

```figure
(define a (drag 'a 50 200))
(define b (drag 'b 270 200))
(define c (drag 'c 140 50))
(define (angle-at p q r)
  (let* ((ux (- (car q) (car p))) (uy (- (cadr q) (cadr p)))
         (vx (- (car r) (car p))) (vy (- (cadr r) (cadr p)))
         (d (* (sqrt (+ (* ux ux) (* uy uy))) (sqrt (+ (* vx vx) (* vy vy))))))
    (if (< d 0.0001) 0 (rad->deg (acos (clamp (/ (+ (* ux vx) (* uy vy)) d) -1 1))))))
(define A (angle-at a b c))
(define B (angle-at b c a))
(define C (angle-at c a b))
(poly (append a b c) 'sky)
(path (append a b c a) 'ink 1.5)
(text (car a) (+ (cadr a) 16) (string-append (fmt A 0) "°") 'centre 'small)
(text (car b) (+ (cadr b) 16) (string-append (fmt B 0) "°") 'centre 'small)
(text (car c) (- (cadr c) 12) (string-append (fmt C 0) "°") 'centre 'small)
(text 8 16 (string-append "Together: " (fmt (+ A B C) 0) "°") 'small 'bold)
(text 8 30 "(each rounded to a whole degree)" 'tiny)
```

```check
(number
  (ask "How many degrees do the three angles of a triangle add up to?")
  (answer 180)
  (wrong-if 360 "360 degrees is a whole turn, or the angles of a four-sided shape. A triangle's make half of that.")
  (hint "Look at the total in the figure while you drag."))
```
