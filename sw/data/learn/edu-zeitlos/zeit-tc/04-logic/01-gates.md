# Logic gates

```lesson
(objective "Say what NOT, AND and OR gates output for any inputs, and read a truth table.")
(minutes 20)
(keywords "logic gate" "truth table" "NOT gate" "inverter" "AND gate" "OR gate" "logic analyser" "GPIO")
```

A **logic gate** is a small circuit, built from transistors, with one
or more inputs and one output. Each input and the output is a bit: 1
for high, 0 for low.

The simplest is the **NOT gate** or **inverter**. It has one input,
and its output is the opposite: 1 in gives 0 out, and 0 in gives 1
out. It is the transistor circuit that turns a high input into a low
output.

Tick A to set the input to 1. A thick wire is a 1, a thin wire a 0, and
the lamp is lit when the output is 1.

```figure
(define (il-out g a b)
  (cond ((eq? g 'not) (- 1 a))
        ((eq? g 'and) (* a b))
        ((eq? g 'or) (max a b))
        ((eq? g 'xor) (if (= a b) 0 1))
        ((eq? g 'nand) (- 1 (* a b)))
        ((eq? g 'nor) (- 1 (max a b)))
        (else (if (= a b) 1 0))))
(define (il-move x y pts)
  (if (null? pts) '()
      (cons (+ x (car pts)) (cons (+ y (cadr pts)) (il-move x y (cddr pts))))))
(define il-or-shape '(0 0 20 0 35 5 45 12 50 20 45 28 35 35 20 40 0 40 6 30 8 20 6 10 0 0))
(define il-back '(-6 40 0 30 2 20 0 10 -6 0))
(define (il-gate g x y)
  (let ((bub (memq g '(not nand nor xnor))))
    (cond ((eq? g 'not) (path (il-move x y '(0 4 40 20 0 36 0 4)) 'ink 1.5))
          ((memq g '(and nand))
           (path (il-move x y '(30 0 0 0 0 40 30 40)) 'ink 1.5)
           (arc (+ x 30) (+ y 20) 20 -90 90 'ink 1.5))
          (else (path (il-move x y il-or-shape) 'ink 1.5)
                (if (memq g '(xor xnor)) (path (il-move x y il-back) 'ink 1.5))))
    (if bub (begin (ring (+ x 54) (+ y 20) 4 'ink 1.5) (+ x 58)) (+ x 50))))
(define (il-wire x0 y0 x1 y1 v)
  (line x0 y0 x1 y1 'ink (if (= v 1) 3 1)))
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define (demo g a b)
  (let* ((one (eq? g 'not)) (o (il-out g a b))
         (we (if (memq g '(or nor)) 76 70)))
    (text 18 (if one 65 55) "A" 'body 'bold)
    (il-wire 32 (if one 60 50) we (if one 60 50) a)
    (if (not one) (begin (text 18 75 "B" 'body 'bold) (il-wire 32 70 we 70 b)))
    (il-wire (il-gate g 70 40) 60 150 60 o)
    (il-lamp 162 60 o)
    (text 162 100 (if (= o 1) "1" "0") 'centre 'big)
    (text 240 30 (if one "A  out" "A B  out") 'centre 'small 'bold)
    (for-range 0 (if one 2 4)
      (lambda (r)
        (let ((ra (if one r (floor (/ r 2)))) (rb (modulo r 2)) (ty (+ 52 (* r 20))))
          (text 240 ty (string-append (fmt ra 0) (if one "" (string-append " " (fmt rb 0))) "   "
                                      (fmt (il-out g ra rb) 0)) 'centre 'body)
          (if (and (= ra a) (or one (= rb b))) (frame 204 (- ty 15) 72 20 'ink 1)))))))
(demo 'not (if (toggle 'a "A") 1 0) 0)
(caption "A NOT gate: the output is the opposite of A.")
```

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Logic Gates")
```

```check
(choice
  (ask "The input of a NOT gate is high. What is its output?")
  (right "Low (0)")
  (wrong "High (1)"
    "That would pass the input straight through, as a plain wire does. A NOT gate gives the opposite of its input.")
  (hint "NOT turns each value into the other one."))
```

---

## AND

An **AND gate** has two inputs, A and B. Its output is 1 only when A
**and** B are both 1.

A list of the output for every combination of inputs is a
**truth table**. The table beside the gate has a row for each of the four
patterns of A and B; the framed row is the one the toggles set now.
Try all four.

```figure
(define (il-out g a b)
  (cond ((eq? g 'not) (- 1 a))
        ((eq? g 'and) (* a b))
        ((eq? g 'or) (max a b))
        ((eq? g 'xor) (if (= a b) 0 1))
        ((eq? g 'nand) (- 1 (* a b)))
        ((eq? g 'nor) (- 1 (max a b)))
        (else (if (= a b) 1 0))))
(define (il-move x y pts)
  (if (null? pts) '()
      (cons (+ x (car pts)) (cons (+ y (cadr pts)) (il-move x y (cddr pts))))))
(define il-or-shape '(0 0 20 0 35 5 45 12 50 20 45 28 35 35 20 40 0 40 6 30 8 20 6 10 0 0))
(define il-back '(-6 40 0 30 2 20 0 10 -6 0))
(define (il-gate g x y)
  (let ((bub (memq g '(not nand nor xnor))))
    (cond ((eq? g 'not) (path (il-move x y '(0 4 40 20 0 36 0 4)) 'ink 1.5))
          ((memq g '(and nand))
           (path (il-move x y '(30 0 0 0 0 40 30 40)) 'ink 1.5)
           (arc (+ x 30) (+ y 20) 20 -90 90 'ink 1.5))
          (else (path (il-move x y il-or-shape) 'ink 1.5)
                (if (memq g '(xor xnor)) (path (il-move x y il-back) 'ink 1.5))))
    (if bub (begin (ring (+ x 54) (+ y 20) 4 'ink 1.5) (+ x 58)) (+ x 50))))
(define (il-wire x0 y0 x1 y1 v)
  (line x0 y0 x1 y1 'ink (if (= v 1) 3 1)))
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define (demo g a b)
  (let* ((one (eq? g 'not)) (o (il-out g a b))
         (we (if (memq g '(or nor)) 76 70)))
    (text 18 (if one 65 55) "A" 'body 'bold)
    (il-wire 32 (if one 60 50) we (if one 60 50) a)
    (if (not one) (begin (text 18 75 "B" 'body 'bold) (il-wire 32 70 we 70 b)))
    (il-wire (il-gate g 70 40) 60 150 60 o)
    (il-lamp 162 60 o)
    (text 162 100 (if (= o 1) "1" "0") 'centre 'big)
    (text 240 30 (if one "A  out" "A B  out") 'centre 'small 'bold)
    (for-range 0 (if one 2 4)
      (lambda (r)
        (let ((ra (if one r (floor (/ r 2)))) (rb (modulo r 2)) (ty (+ 52 (* r 20))))
          (text 240 ty (string-append (fmt ra 0) (if one "" (string-append " " (fmt rb 0))) "   "
                                      (fmt (il-out g ra rb) 0)) 'centre 'body)
          (if (and (= ra a) (or one (= rb b))) (frame 204 (- ty 15) 72 20 'ink 1)))))))
(demo 'and (if (toggle 'a "A") 1 0) (if (toggle 'b "B") 1 0))
(caption "An AND gate: 1 only when A and B are both 1.")
```

```check
(choice
  (ask "An AND gate has A = 1 and B = 0. What is its output?")
  (right "0")
  (wrong "1"
    "One input being 1 is not enough for AND. Both must be 1 for the output to be 1.")
  (hint "Find the row 1 0 in the truth table."))
```

---

## OR

An **OR gate** outputs 1 when A **or** B is 1, and also when both are.
Its output is 0 only when both inputs are 0.

```figure
(define (il-out g a b)
  (cond ((eq? g 'not) (- 1 a))
        ((eq? g 'and) (* a b))
        ((eq? g 'or) (max a b))
        ((eq? g 'xor) (if (= a b) 0 1))
        ((eq? g 'nand) (- 1 (* a b)))
        ((eq? g 'nor) (- 1 (max a b)))
        (else (if (= a b) 1 0))))
(define (il-move x y pts)
  (if (null? pts) '()
      (cons (+ x (car pts)) (cons (+ y (cadr pts)) (il-move x y (cddr pts))))))
(define il-or-shape '(0 0 20 0 35 5 45 12 50 20 45 28 35 35 20 40 0 40 6 30 8 20 6 10 0 0))
(define il-back '(-6 40 0 30 2 20 0 10 -6 0))
(define (il-gate g x y)
  (let ((bub (memq g '(not nand nor xnor))))
    (cond ((eq? g 'not) (path (il-move x y '(0 4 40 20 0 36 0 4)) 'ink 1.5))
          ((memq g '(and nand))
           (path (il-move x y '(30 0 0 0 0 40 30 40)) 'ink 1.5)
           (arc (+ x 30) (+ y 20) 20 -90 90 'ink 1.5))
          (else (path (il-move x y il-or-shape) 'ink 1.5)
                (if (memq g '(xor xnor)) (path (il-move x y il-back) 'ink 1.5))))
    (if bub (begin (ring (+ x 54) (+ y 20) 4 'ink 1.5) (+ x 58)) (+ x 50))))
(define (il-wire x0 y0 x1 y1 v)
  (line x0 y0 x1 y1 'ink (if (= v 1) 3 1)))
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define (demo g a b)
  (let* ((one (eq? g 'not)) (o (il-out g a b))
         (we (if (memq g '(or nor)) 76 70)))
    (text 18 (if one 65 55) "A" 'body 'bold)
    (il-wire 32 (if one 60 50) we (if one 60 50) a)
    (if (not one) (begin (text 18 75 "B" 'body 'bold) (il-wire 32 70 we 70 b)))
    (il-wire (il-gate g 70 40) 60 150 60 o)
    (il-lamp 162 60 o)
    (text 162 100 (if (= o 1) "1" "0") 'centre 'big)
    (text 240 30 (if one "A  out" "A B  out") 'centre 'small 'bold)
    (for-range 0 (if one 2 4)
      (lambda (r)
        (let ((ra (if one r (floor (/ r 2)))) (rb (modulo r 2)) (ty (+ 52 (* r 20))))
          (text 240 ty (string-append (fmt ra 0) (if one "" (string-append " " (fmt rb 0))) "   "
                                      (fmt (il-out g ra rb) 0)) 'centre 'body)
          (if (and (= ra a) (or one (= rb b))) (frame 204 (- ty 15) 72 20 'ink 1)))))))
(demo 'or (if (toggle 'a "A") 1 0) (if (toggle 'b "B") 1 0))
(caption "An OR gate: 1 when either input is 1, or both.")
```

```check
(choice
  (ask "An OR gate has A = 1 and B = 1. What is its output?")
  (right "1")
  (wrong "0"
    "In everyday speech \"or\" often means one or the other but not both. A logic OR includes both: it is 1 whenever at least one input is 1.")
  (hint "Try the toggles: tick both A and B."))
```

---

## More inputs

Gates can have more than two inputs, and they work the same way. A
four-input AND gate outputs 1 only when all four inputs are 1; a
four-input OR outputs 1 when any of them is.

A truth table has one row for each pattern of the inputs. As in the
Information unit, n bits make $2^n$ patterns.

```check
(number
  (ask "How many rows has the truth table of a gate with 3 inputs?")
  (answer 8) (from "2^3")
  (wrong-if 3 "That is one row for each input. A row is a whole pattern of all the inputs, and 3 bits make 2 x 2 x 2 patterns.")
  (wrong-if 6 "Each extra input doubles the number of rows, rather than adding 2: 2, 4, 8.")
  (hint "Two inputs gave 4 rows. One more input doubles it."))
```

---

## Ones and zeros on real pins

The 1s and 0s of logic are real voltages on real pins. The Zeitlos app
logic is a **logic analyser**, an instrument that shows highs and lows,
for the computer's **GPIO pins**: pins at the edge of its board that
you can wire things to. More: [logic](docs:logic_app).

```try
(try (app "logic")
  (task "Open logic with the button below. If no window opens, this computer has no GPIO pins: just read on. Each row is one pin, with a lamp that is filled when the pin is high. Click channel 0's mode button (the bottom row) so that it reads OUT, then click its level toggle between LO and HI and watch the lamp. Set the mode back to IN (click it twice more), then come back.")
  (check (choice
    (ask "With channel 0 set to OUT and its level LO, what did its lamp show?")
    (right "Dark: the pin is low, a 0")
    (wrong "Lit: the pin is high"
      "The level toggle sets what the pin drives. LO drives it low, so the lamp, which shows the pin's real level, is dark.")
    (hint "LO means low, and low is 0."))))
```

```sources
(source "Zeitlos docs: logic_app.md, Channels")
```

---

## Choosing a gate

```example
(example
  (problem "A machine may start only when its lid is closed and the start button is pressed. Each is a bit, 1 when true. Which gate gives the start signal?")
  (step "The signal must be 1 only when both bits are 1.")
  (step "That is exactly the AND gate's truth table.")
  (answer "An AND gate, with the lid and the button as its inputs."))
```

```check
(choice
  (ask "A room light should come on when either of two buttons is pressed, or both. Which gate?")
  (right "OR")
  (wrong "AND"
    "AND needs both buttons pressed. Here one button is enough, so the gate must give 1 when either input is 1.")
  (wrong "NOT"
    "NOT has only one input and turns it over. Here there are two inputs, and one being on is enough.")
  (hint "Is one button enough to light it?"))
```

---

```check
(choice
  (ask "A four-input OR gate has the inputs 0, 0, 1, 0. What is its output?")
  (right "1")
  (wrong "0"
    "Three of the inputs are 0, but OR needs only one input to be 1. One is, so the output is 1.")
  (hint "Is any input a 1?"))
```

So: NOT gives the opposite, AND is 1 only when every input is 1, OR is
1 when any input is, and a truth table lists the output for every
pattern of the inputs.
