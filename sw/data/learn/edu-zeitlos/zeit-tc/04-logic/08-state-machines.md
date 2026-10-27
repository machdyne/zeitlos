# State machines and timing

```lesson
(objective "Follow a state machine from state to state, and work out the fastest clock a circuit can use.")
(minutes 25)
(keywords "state machine" "setup time" "hold time" "critical path" "metastability")
```

A **state machine** is a register that holds the machine's present
state, plus combinational logic that works out the next state from
the present state and the inputs. At each clock edge, the register
takes in the next state.

A traffic light is one. It has three states, and a timer input that
says when the present light has been on long enough:

| State | Timer done: next state | Code |
|---|---|---|
| GREEN | YELLOW | 00 |
| YELLOW | RED | 01 |
| RED | GREEN | 10 |

While the timer runs, the state stays the same.

```figure
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define t (- (seconds) (* 10 (floor (/ (seconds) 10)))))
(define s (if (< t 4) 0 (if (< t 6) 1 2)))
(define names '("GREEN" "YELLOW" "RED"))
(frame 18 20 44 120 'ink 1.5)
(for-range 0 3
  (lambda (k)
    (let ((y (+ 40 (* 40 (- 2 k)))))
      (il-lamp 40 y (if (= k s) 1 0))
      (text 72 (+ y 4) (list-ref names k) 'left 'tiny))))
(for-range 0 3
  (lambda (k)
    (let ((x (+ 130 (* 0 k))) (y (+ 40 (* 50 k))))
      (if (= k s) (rect 130 (- y 16) 120 30 'ink) (frame 130 (- y 16) 120 30 'ink 1.5))
      (text 190 (+ y 4) (list-ref names k) 'centre 'body 'bold (if (= k s) 'paper 'ink))
      (if (< k 2) (arrow 190 (+ y 14) 190 (+ y 34) 'ink 1.5)))))
(path '(250 139 280 139 280 39 254 39) 'ink 1.5)
(arrow 262 39 252 39 'ink 1.5)
(text 160 210 (string-append "state register: " (list-ref '("00" "01" "10") s)) 'centre 'body 'bold)
(caption "Each arrow is taken when the timer is done.")
```

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: State Machines")
```

```check
(choice
  (ask "The light is YELLOW and the timer is done. What is the state after the next clock edge?")
  (right "RED")
  (wrong "GREEN"
    "GREEN comes after RED, not after YELLOW. Follow the arrow from YELLOW.")
  (wrong "YELLOW"
    "The state stays the same only while the timer runs. Here it is done, so the machine moves on.")
  (hint "Find YELLOW in the table, and read its next state."))
```

---

## Coding the states

Each state is a pattern of bits in the register, so three states fit
in two bits, and the table becomes a few gates. Every controller in a
computer, from the circuit that reads a keyboard to the part of a
processor that steps through its work, is a state machine like this,
often with many more states.

```check
(number
  (ask "A state machine has 5 states. How few bits can its state register have?")
  (answer 3) (true "2^2 < 5") (true "2^3 >= 5")
  (wrong-if 5 "That is one bit for each state. The bits form a pattern, and 3 bits already make 8 patterns.")
  (wrong-if 2 "2 bits make only 4 patterns, one too few for 5 states.")
  (hint "Double from 2: 2, 4, 8. Which is the first at least 5?"))
```

---

## Setup and hold

Synchronous design has one hard rule: the logic between two
flip-flops must settle before the next clock edge. A flip-flop needs
its input steady for a short **setup time** before the edge, and for a
short **hold time** after it.

```check
(choice
  (ask "A flip-flop's input is still changing just before the clock edge. Which rule is broken?")
  (right "Its setup time")
  (wrong "Its hold time"
    "The hold time is after the edge. The input must already be steady before the edge: that is the setup time.")
  (hint "Is the problem before the edge or after it?"))
```

---

## The fastest clock

The slowest chain of logic between two flip-flops is the
**critical path**. It sets the fastest clock the whole circuit can
use: the time between edges must be at least as long as it takes to
settle,
with the setup time.

If the critical path takes 20 ns, the clock can be at most 50 MHz,
since 1 second / 20 ns is 50 million. To go faster, a designer
shortens it, for example by splitting a long calculation over
two clock cycles. The tools that build a design for an FPGA report
its critical path and fastest clock.

```check
(number
  (ask "A circuit's critical path, with the setup time, takes 25 ns. What is its fastest clock, in MHz?")
  (answer 40) (unit "MHz") (from "1000 / 25")
  (wrong-if 25 "25 is the time in ns. The clock rate is one second divided by that time.")
  (wrong-if 4 "1 second is 1,000 million ns: 1,000 / 25 is 40, so 40 million cycles a second.")
  (hint "1,000 million ns in a second, divided by 25 ns each cycle, in millions."))
```

---

## Metastability

A signal from outside, such as a button, can change at the very moment
a flip-flop takes it in. The flip-flop can then be caught between 0
and 1 for a moment: **metastability**. The usual cure is to pass such
a signal through two flip-flops in a row before using it, which gives
the first one a whole clock cycle to settle.

```check
(choice
  (ask "A button's signal goes into a synchronous circuit. What is the usual way to make it safe?")
  (right "Pass it through two flip-flops in a row first")
  (wrong "Pass it through a NOT gate twice"
    "Two NOTs give the same signal back, still changing whenever the button does. The trouble is its timing against the clock, which flip-flops fix.")
  (wrong "Use a faster clock"
    "A faster clock gives the flip-flop less time to settle, not more. The signal can still change at the edge.")
  (hint "Give the first flip-flop a whole clock cycle to settle."))
```

---

## From logic to a processor

Every part of a processor has now been met:

- **registers** to hold numbers;
- an **ALU** of adders and gates to calculate with them;
- **multiplexers** to route numbers between them;
- **decoders** to pick out memory and devices;
- a **state machine** to step through the work, one clock cycle at a
  time.

The next unit puts these parts together into a computer.

```check
(choice
  (ask "Which part of a processor routes numbers from one place to another?")
  (right "Multiplexers")
  (wrong "Registers"
    "Registers hold numbers. Choosing which number goes where is done by multiplexers.")
  (wrong "Decoders"
    "Decoders pick out one memory chip or device by its number. Routing a number from one place to another is a multiplexer's job.")
  (hint "Which circuit chooses one of its inputs to pass on?"))
```

So: a state machine is a register and the logic for its next state,
and a circuit's critical path sets how fast its clock can run.
