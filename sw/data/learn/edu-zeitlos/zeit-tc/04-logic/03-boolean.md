# Logic as algebra

```lesson
(objective "Simplify logic with the rules of Boolean algebra and De Morgan's laws, and build any gate from NAND.")
(minutes 20)
(keywords "Boolean algebra" "De Morgan's laws" "universal (gate)" "FPGA")
```

Logic can be written as algebra, as George Boole described in 1854:
**Boolean algebra**. Its values are only 0 and 1. This course writes
the gates as the programming language C does:

| Written | Means |
|---|---|
| `A & B` | A AND B |
| `A \| B` | A OR B |
| `A ^ B` | A XOR B |
| `~A` | NOT A |

So `~(A & B)` is NAND, and `(A & B) | C` is an AND gate whose output
goes into an OR gate with C.

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Boolean Algebra")
```

```check
(choice
  (ask "A = 1, B = 0 and C = 1. What is (A & B) | C?")
  (right "1")
  (wrong "0"
    "A & B is 0, but that is only the first part. It goes into an OR with C, and C is 1, so the result is 1.")
  (hint "Work out the brackets first: A & B. Then OR that with C."))
```

---

## Rules

Some rules hold for every value of A. You can check each one by trying
A = 0 and A = 1.

| Rule | In words |
|---|---|
| `A & 1 = A`, `A \| 0 = A` | AND with 1, OR with 0: unchanged |
| `A & 0 = 0`, `A \| 1 = 1` | AND with 0 is 0, OR with 1 is 1 |
| `A & ~A = 0`, `A \| ~A = 1` | a bit and its opposite |
| `~~A = A` | two NOTs cancel |

```check
(choice
  (ask "What is A | 1, whatever A is?")
  (right "1")
  (wrong "A"
    "That is A | 0. With 1 as one input, an OR gate already has a 1, so its output is 1 whatever A is.")
  (wrong "0"
    "An OR gate gives 0 only when every input is 0. Here one input is always 1.")
  (hint "OR is 1 when any input is 1."))
```

---

## De Morgan's laws

Two rules let you swap AND for OR. They are **De Morgan's laws**:

```text
~(A & B) = ~A | ~B
~(A | B) = ~A & ~B
```

Turn over the whole, swap AND and OR, and turn over each input. Tick
the inputs: both sides of each law always agree.

```figure
(define a (if (toggle 'a "A") 1 0))
(define b (if (toggle 'b "B") 1 0))
(define (row y left lv right rv)
  (text 16 y left 'left 'body)
  (text 128 y (fmt lv 0) 'centre 'body 'bold)
  (text 176 y right 'left 'body)
  (text 296 y (fmt rv 0) 'centre 'body 'bold))
(text 160 30 (string-append "A = " (fmt a 0) ",  B = " (fmt b 0)) 'centre 'body)
(row 80 "~(A & B)" (- 1 (* a b)) "~A | ~B" (max (- 1 a) (- 1 b)))
(row 130 "~(A | B)" (- 1 (max a b)) "~A & ~B" (* (- 1 a) (- 1 b)))
(text 148 80 "=" 'centre 'body)
(text 148 130 "=" 'centre 'body)
(caption "Each side worked out from A and B.")
```

```check
(choice
  (ask "By De Morgan's laws, ~(A | B) is the same as:")
  (right "~A & ~B")
  (wrong "~A | ~B"
    "Turning over each input is only half of it: the OR must also become an AND. Try A = 1, B = 0: ~(A | B) is 0, but ~A | ~B is 1.")
  (wrong "A & B"
    "The inputs must be turned over too. Try A = 0, B = 0: ~(A | B) is 1, but A & B is 0.")
  (hint "Turn over the whole, swap the OR for an AND, and turn over each input."))
```

---

## Everything from NAND

The NAND gate is **universal**: any logic circuit can be built from
NAND gates alone.

- NOT A is A NAND A: with both inputs the same, NAND turns A over.
- A AND B is NOT (A NAND B): a NAND, then a NAND used as a NOT.
- A OR B is (NOT A) NAND (NOT B), by De Morgan's laws.

Choose a circuit and try the inputs: each output matches the gate it
replaces.

```figure
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
(define k (choice 'k '("NOT" "AND" "OR") "Circuit"))
(define a (if (toggle 'a "A") 1 0))
(define b (if (toggle 'b "B") 1 0))
(define (nand x y p q)
  (il-wire (- x 12) (+ y 10) x (+ y 10) p)
  (il-wire (- x 12) (+ y 30) x (+ y 30) q)
  (il-gate 'nand x y)
  (- 1 (* p q)))
(define (tied x y p)
  (il-wire (- x 12) (+ y 10) (- x 12) (+ y 30) p)
  (nand x y p p))
(define (w x0 y0 x1 y1 v) (il-wire x0 y0 x1 y1 v))
(define ay (if (= k 1) 40 50))
(text 14 (+ ay 4) "A" 'body 'bold)
(w 26 ay 52 ay a)
(if (> k 0) (text 14 134 "B" 'body 'bold))
(define out
  (cond ((= k 0) (tied 64 30 a))
        ((= k 1)
         (w 26 130 40 130 b) (w 40 130 40 60 b) (w 40 60 52 60 b)
         (let ((n1 (nand 64 30 a b)))
           (w 122 50 140 50 n1)
           (tied 152 30 n1)))
        (else
         (w 26 130 52 130 b)
         (let ((na (tied 64 30 a)) (nb (tied 64 110 b)))
           (w 122 50 140 50 na) (w 140 50 140 80 na) (w 140 80 152 80 na)
           (w 122 130 140 130 nb) (w 140 130 140 100 nb) (w 140 100 152 100 nb)
           (nand 164 70 na nb)))))
(define ox (list-ref '(122 210 222) k))
(define oy (if (= k 2) 90 50))
(w ox oy 270 oy out)
(il-lamp 282 oy out)
(text 282 (+ oy 30) (fmt out 0) 'centre 'big)
(caption (string-append (list-ref '("NOT A" "A AND B" "A OR B") k) " from NAND gates = "
  (fmt (cond ((= k 0) (- 1 a)) ((= k 1) (* a b)) (else (max a b))) 0)))
```

```check
(choice
  (ask "Both inputs of a NAND gate are joined to A. What does the gate give?")
  (right "NOT A")
  (wrong "A"
    "With both inputs equal to A, the AND inside gives A, and the NOT on the output turns it over.")
  (wrong "Always 0"
    "Only 1 and 1 give 0 from a NAND. When A is 0, both inputs are 0, and the output is 1.")
  (hint "Try A = 0 and A = 1 in the NAND table."))
```

---

## Why it matters

Because NAND is universal, a computer can be built from one kind of
gate, repeated many times. NOR is universal too.

A Zeitlos computer is built on an **FPGA** (field-programmable gate
array), a chip that takes the idea further: it is made of many copies
of one kind of block, and each block can be set up to act as any gate.

```example
(example
  (problem "Simplify A & (B | 1).")
  (step "B | 1 is 1, whatever B is.")
  (step "So the expression is A & 1.")
  (step (choice (ask "What is A & 1?") (right "A")
          (wrong "1" "AND with 1 leaves A unchanged: if A is 0, A & 1 is 0.")))
  (answer "A & (B | 1) = A"))
```

```check
(choice
  (ask "Simplify A | (B & 0).")
  (right "A")
  (wrong "0"
    "B & 0 is 0, but it is ORed with A. A | 0 is A, not 0.")
  (wrong "B"
    "B & 0 is 0 whatever B is, so B drops out. What is left is A | 0.")
  (hint "Start with the brackets: B & 0."))
```

So: logic is algebra on 0 and 1, De Morgan's laws swap AND and OR, and
NAND alone can build any circuit.
