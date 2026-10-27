# Circuits that remember

```lesson
(objective "Explain how feedback stores a bit, and what a D flip-flop does at each rising edge of the clock.")
(minutes 25)
(keywords "sequential (circuit)" "feedback (in a circuit)" "latch" "clock signal" "clock cycle" "rising edge" "flip-flop" "synchronous (design)" "hertz" "megahertz")
```

A combinational circuit has no memory: change its inputs and the old
result is gone. A computer also needs circuits that remember. A
circuit whose outputs depend on what happened before, not only on its
inputs now, is **sequential**.

Memory comes from **feedback**: a gate's output wired back into the
inputs of the circuit that makes it.

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Sequential Circuits")
```

```check
(choice
  (ask "A circuit's output depends on which button was pressed last. What kind of circuit is it?")
  (right "Sequential")
  (wrong "Combinational"
    "A combinational circuit sees only its inputs as they are now. Knowing which button came last means remembering the past, which makes it sequential.")
  (hint "Does the circuit need to remember something?"))
```

---

## The latch

Two NOR gates, each one's output wired into an input of the other,
make a **latch**. It has two inputs, S (set) and R (reset), and an
output Q:

- a high pulse on S sets Q to 1;
- a high pulse on R resets Q to 0;
- with both low, each gate holds the other in place, and Q keeps its
  last value. The latch stores one bit.

S and R must never be high together. Watch the latch as time passes:
the line is now.

```figure
(still
(define (trace x y w vals name)
  (text (- x 6) y name 'right 'small)
  (let loop ((v vals) (px x) (last -1) (pts '()))
    (if (null? v)
        (path (reverse (cons (- y (* 14 last)) (cons px pts))) 'ink 1.5)
        (let ((h (- y (* 14 (car v)))))
          (loop (cdr v) (+ px w) (car v)
                (if (= (car v) last) pts
                    (if (< last 0) (list h px)
                        (cons h (cons px (cons (- y (* 14 last)) (cons px pts)))))))))))
  (trace 60 80 20 '(0 1 1 0 0 0 0 1 0 0 0 0) "S")
  (trace 60 130 20 '(0 0 0 0 1 1 0 0 0 0 0 0) "R")
  (trace 60 180 20 '(0 1 1 1 0 0 0 1 1 1 1 1) "Q")
  (caption "Q is set by S, reset by R, and held between."))
(define t (- (seconds) (* 12 (floor (/ (seconds) 12)))))
(define i (floor t))
(line (+ 60 (* 20 t)) 50 (+ 60 (* 20 t)) 190 'ink 1)
(text 160 30 (string-append "S = " (fmt (list-ref '(0 1 1 0 0 0 0 1 0 0 0 0) i) 0)
  "   R = " (fmt (list-ref '(0 0 0 0 1 1 0 0 0 0 0 0) i) 0)
  "   Q = " (fmt (list-ref '(0 1 1 1 0 0 0 1 1 1 1 1) i) 0)) 'centre 'body 'bold)
```

```check
(choice
  (ask "A latch's S input goes high, then low again. R stays low. What is Q now?")
  (right "1: the latch remembers the set")
  (wrong "0: S is low again"
    "That is how a combinational circuit would behave. In the latch, the two gates hold each other, so Q stays 1 after S goes low.")
  (hint "Look at Q in the figure after S's pulse ends."))
```

---

## The clock

A big circuit full of latches is hard to reason about, because each
change runs through it at its own pace. Nearly every digital design
follows one rule instead: one signal, the **clock signal**, changes
steadily between 0 and 1, and the circuit's memory changes only at
one moment in each **clock cycle**: the **rising edge**, when the
clock goes from 0 to 1.

A clock's speed is counted in **hertz** (Hz), cycles a second. A
**megahertz** (MHz) is a million cycles a second.

```check
(number
  (ask "A clock runs at 50 MHz. How many nanoseconds apart are its rising edges?")
  (answer 20) (unit "ns") (from "1000 / 50")
  (wrong-if 50 "50 is the number of millions of cycles a second. The time between edges is one second divided by 50 million.")
  (wrong-if 2 "One second is a billion nanoseconds: 1,000,000,000 / 50,000,000 is 20.")
  (hint "A second is 1,000 million ns. Share it among 50 million cycles."))
```

---

## The D flip-flop

A **flip-flop** is a memory element for one bit that changes only at
a clock edge. The D flip-flop has a data input D, a clock input and
an output Q. At each rising edge, Q takes the value D has at that
instant, and keeps it until the next rising edge, whatever D does in
between.

Watch Q: it changes only at the marks, the rising edges, and it
ignores the short dip in D between two edges.

```figure
(still
(define (trace x y w vals name)
  (text (- x 6) y name 'right 'small)
  (let loop ((v vals) (px x) (last -1) (pts '()))
    (if (null? v)
        (path (reverse (cons (- y (* 14 last)) (cons px pts))) 'ink 1.5)
        (let ((h (- y (* 14 (car v)))))
          (loop (cdr v) (+ px w) (car v)
                (if (= (car v) last) pts
                    (if (< last 0) (list h px)
                        (cons h (cons px (cons (- y (* 14 last)) (cons px pts)))))))))))
  (trace 60 70 7.5 '(0 0 1 1 0 0 1 1 0 0 1 1 0 0 1 1 0 0 1 1 0 0 1 1 0 0 1 1 0 0 1 1) "clock")
  (trace 60 125 7.5 '(1 1 1 1 1 0 0 0 0 1 1 1 0 1 1 1 1 0 0 0 0 1 1 1 1 0 0 0 0 0 0 0) "D")
  (trace 60 180 7.5 '(0 0 1 1 1 1 0 0 0 0 1 1 1 1 1 1 1 1 0 0 0 0 1 1 1 1 0 0 0 0 0 0) "Q")
  (for-range 0 8 (lambda (k) (text (+ 75 (* 30 k)) 85 "^" 'centre 'small)))
  (caption "At each rising edge (^), Q takes D."))
(define t (- (seconds) (* 16 (floor (/ (seconds) 16)))))
(line (+ 60 (* 15 t)) 40 (+ 60 (* 15 t)) 190 'ink 1)
(text 160 28 (string-append "Q = " (fmt (list-ref '(0 0 1 1 1 1 0 0 0 0 1 1 1 1 1 1 1 1 0 0 0 0 1 1 1 1 0 0 0 0 0 0) (floor (* 2 t))) 0)) 'centre 'body 'bold)
```

```check
(choice
  (ask "Between two rising edges, D changes from 0 to 1. What does Q do?")
  (right "Nothing until the next rising edge, when it becomes 1")
  (wrong "It becomes 1 at once"
    "That would be a plain wire. A flip-flop looks at D only at a rising edge, so Q waits for the next one.")
  (wrong "It becomes 0"
    "Q never turns D over. At the next rising edge it copies D, which is then 1.")
  (hint "A D flip-flop changes only at a rising edge."))
```

---

## Step by step

Between edges, the combinational logic is free to settle. At the
edge, every flip-flop captures its settled input at once; then the
logic settles again from the new values. This **synchronous** style
splits a circuit's work into steps, one per clock cycle. It is how
processors, and nearly everything built in an FPGA, are designed.

```check
(choice
  (ask "In a synchronous circuit, when do the flip-flops change?")
  (right "All together, at the clock's rising edge")
  (wrong "Whenever their inputs change"
    "That is how a latch or combinational logic behaves. Flip-flops in a synchronous circuit wait for the clock edge, so the logic can settle first.")
  (wrong "Each at its own pace"
    "Changes at their own pace are what make a circuit of latches hard to reason about. The clock makes them change together.")
  (hint "What is the one moment in each clock cycle?"))
```

---

```example
(example
  (problem "D is 1 at the first rising edge, then drops to 0 just after it, and rises to 1 again just before the second edge. What is Q between the two edges, and after the second?")
  (step "At the first edge D is 1, so Q becomes 1.")
  (step "D changes between the edges, but Q ignores it: Q stays 1.")
  (step (choice (ask "At the second edge D is 1. What is Q after it?") (right "1")
          (wrong "0" "The flip-flop copies D at the edge, and D is 1 there. The dip in between does not count.")))
  (answer "Q is 1 between the edges and after the second."))
```

```check
(number
  (ask "A clock runs at 10 MHz. How many rising edges does it make in one second?")
  (answer 10000000) (from "10 * 1000000")
  (wrong-if 10 "Mega means million: 10 MHz is 10 million cycles a second, and each cycle has one rising edge.")
  (wrong-if 20000000 "Each cycle has one rising edge and one falling edge. Only the rising ones count here.")
  (hint "One rising edge per cycle, and 10 MHz is 10 million cycles a second."))
```

So: feedback lets a circuit remember, and a D flip-flop copies its
input only at the clock's rising edge, so a whole circuit moves in
steps.
