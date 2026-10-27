# More gates

```lesson
(objective "Say what NAND, NOR, XOR and XNOR gates output for any inputs.")
(minutes 15)
(keywords "NAND gate" "NOR gate" "XOR gate" "exclusive or" "XNOR gate")
```

A small circle on a gate's output means NOT: the output is turned over.

A **NAND gate** is an AND followed by a NOT: its output is 0 only when
both inputs are 1. A **NOR gate** is an OR followed by a NOT: its
output is 1 only when both inputs are 0.

Compare each truth table with the AND and OR tables from the last
lesson: every output is turned over.

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
(define g (choice 'g '("NAND" "NOR") "Gate"))
(demo (if (= g 0) 'nand 'nor) (if (toggle 'a "A") 1 0) (if (toggle 'b "B") 1 0))
(caption (if (= g 0) "NAND: NOT (A AND B)" "NOR: NOT (A OR B)"))
```

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Logic Gates")
```

```check
(choice
  (ask "A NAND gate has A = 1 and B = 1. What is its output?")
  (right "0")
  (wrong "1"
    "1 is what AND gives for these inputs. NAND turns the AND's output over, so it gives 0.")
  (hint "Work out AND first, then turn it over."))
```

---

## XOR

An **XOR gate**, short for **exclusive or**, outputs 1 when its inputs
**differ**: one is 1 and the other 0. When both are 1, it outputs 0,
which is where it differs from OR.

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
(demo 'xor (if (toggle 'a "A") 1 0) (if (toggle 'b "B") 1 0))
(caption "XOR: 1 when A and B differ.")
```

```check
(choice
  (ask "An XOR gate has A = 1 and B = 1. What is its output?")
  (right "0")
  (wrong "1"
    "That is OR's answer. XOR is 1 only when the inputs differ, and here they are the same.")
  (hint "Are the two inputs different?"))
```

---

## XOR as a switchable NOT

Look at the XOR table one input at a time. When B is 0, the output
equals A. When B is 1, the output is NOT A. So B decides whether A
passes through unchanged or turned over. Later lessons use this to
turn an adder into a subtractor.

```check
(choice
  (ask "An XOR gate has B = 1. What is its output?")
  (right "The opposite of A")
  (wrong "Always 1"
    "Try A = 1 with B = 1: the inputs are the same, so XOR gives 0. With B = 1, the output follows A, turned over.")
  (wrong "The same as A"
    "That is what happens when B is 0. With B = 1, the two inputs differ exactly when A is 0.")
  (hint "Try both values of A with B ticked."))
```

---

## XNOR

An **XNOR gate** is XOR followed by NOT. It outputs 1 when its inputs
are **the same**, so it tells whether two bits are equal.

```check
(choice
  (ask "Which gate outputs 1 exactly when its two inputs are equal?")
  (right "XNOR")
  (wrong "XOR"
    "XOR is 1 when the inputs differ. Equal inputs need its opposite, XNOR.")
  (wrong "AND"
    "AND is 1 when both inputs are 1, but 0 and 0 are equal too, and AND gives 0 for them.")
  (hint "Equal means both 0 or both 1."))
```

---

## Practice

```check
(choice
  (ask "A gate's truth table, for A B = 00, 01, 10, 11, gives 1, 0, 0, 0. Which gate is it?")
  (right "NOR")
  (wrong "NAND"
    "NAND gives 1, 1, 1, 0: it is 0 only when both inputs are 1. This gate is 1 only when both are 0.")
  (wrong "AND"
    "AND gives 0, 0, 0, 1. This table is 1 only for 00, which is OR turned over.")
  (wrong "XNOR"
    "XNOR would also give 1 for 11, since those inputs are equal too. This gate gives 0 there.")
  (hint "The output is 1 only for 00. Which gate is OR turned over?"))
```

---

```check
(choice
  (ask "A NOR gate has A = 0 and B = 1. What is its output?")
  (right "0")
  (wrong "1"
    "OR gives 1 here, because B is 1. NOR turns that over, to 0.")
  (hint "Work out OR first, then turn it over."))
```

So: NAND and NOR are AND and OR turned over, XOR is 1 when the inputs
differ, and XNOR is 1 when they are the same.
