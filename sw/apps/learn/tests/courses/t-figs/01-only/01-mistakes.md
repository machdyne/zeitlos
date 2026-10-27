# Mistakes

The course library works.

```figure
(unit-bar 20 20 4)
```

---

A colour that is not one.

```figure
(rect 10 10 50 50 'purple)
```

---

A slider at its end divides by zero.

```figure
(define n (slider 'n 0 10 5))
(text 20 20 (fmt (/ 10 n) 2))
(rect 0 0 (/ 100 n) 10)
```

---

Too big for the figure.

```figure
(circle 300 200 80 'warm)
```

---

A moving figure that breaks later.

```figure
(if (> (seconds) 5) (line 0 0 'x))
```

---

A point that, dragged, divides by zero.

```figure
(define p (drag 'p 100 100))
(define q (drag 'q 200 100))
(define d (- (car q) (car p)))
(line 0 0 (/ 100 d) 10)
```

---

Words too long for the figure.

```figure
(text 160 120 "Each bar is one whole, cut into 4 equal pieces, and more." 'centre 'small)
```
