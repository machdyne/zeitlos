# Registers and counters

```lesson
(objective "Say what a register, a counter and a shift register do, and work out how fast a counter's bits change.")
(minutes 20)
(keywords "register" "counter (circuit)" "shift register" "serial" "parallel")
```

Flip-flops side by side, all on the same clock, store a number of
many bits. This is a **register**. A 32-bit register is 32
flip-flops; at each rising edge it takes in a whole new 32-bit number
at once, or keeps the one it has.

```sources
(source "Timeless Computing (docs/tc.md), Digital Logic: Registers, Counters and Shift Registers")
```

```check
(number
  (ask "How many flip-flops does an 8-bit register have?")
  (answer 8)
  (wrong-if 256 "256 is how many different numbers 8 bits can hold. Each flip-flop holds one bit, so 8 bits need 8 flip-flops.")
  (wrong-if 1 "One flip-flop holds only one bit. A register has one for each bit.")
  (hint "Each flip-flop stores one bit."))
```

---

## The counter

Feed a register's output through an adder that adds 1, and back into
its input. At every rising edge it takes in its own value plus one: it
is a **counter**.

This one has 4 bits and a clock with one rising edge a second. Watch
the bits: the rightmost changes at every edge, the next at every
second edge, the next at every fourth. Each bit changes half as often
as the one to its right.

```figure
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define n (modulo (floor (seconds)) 16))
(define half (- (seconds) (floor (seconds))))
(text 40 44 "clock" 'centre 'small)
(il-lamp 40 64 (if (< half 0.5) 1 0))
(bit-boxes 96 60 4 n 40 #t #f)
(text 160 150 (string-append "= " (fmt n 0)) 'centre 24)
(caption "A 4-bit counter: one more at each rising edge.")
```

```check
(text
  (ask "A 4-bit counter holds 0111. What does it hold after the next rising edge?")
  (accept "1000")
  (wrong-if "0112" "Binary has only the digits 0 and 1. 0111 is 7, so the next value is 8.")
  (wrong-if "1111" "That sets the top bit and keeps the others. Adding 1 to 0111 carries all the way: the three 1s become 0s and the top bit becomes 1.")
  (hint "0111 is 7. What is 8 in binary?"))
```

---

## When a counter is full

A 4-bit counter goes up to 1111, which is 15. At the next edge it
goes back to 0000 and starts again, because the fifth bit that 16 needs
does not exist.

```check
(text
  (ask "A 4-bit counter holds 1111. What does it hold after the next rising edge?")
  (accept "0000") (accept "0")
  (wrong-if "10000" "16 needs five bits, and the counter has only four. The fifth bit is lost, so the counter goes back to 0000.")
  (wrong-if "1111" "A counter does not stop at the top. It adds 1, and 1111 + 1 leaves 0000 in its four bits.")
  (hint "1111 + 1 = 10000. How many bits does the counter keep?"))
```

---

## Dividing a clock

Since each bit changes half as often as the one before, the top bit of
a counter is a much slower clock. Counters measure time, count events,
and slow a fast clock down.

```example
(example
  (problem "A 24-bit counter runs on a 12 MHz clock. How long does its top bit stay at 1 each time?")
  (step "The top bit is worth 2 to the power 23. It flips each time the counter counts that far.")
  (step "That takes 8,388,608 edges, and the clock makes 12,000,000 a second.")
  (step "8,388,608 / 12,000,000 is about 0.7 seconds.")
  (answer "About 0.7 seconds on, then 0.7 seconds off: an LED on it blinks slowly enough to see."))
```

```check
(number
  (ask "In a counter, bit 0 changes at every rising edge. How many edges are there between two changes of bit 3?")
  (answer 8) (from "2^3")
  (wrong-if 4 "Bit 2 changes every 4 edges. Each bit to the left takes twice as long: 1, 2, 4, 8.")
  (wrong-if 3 "The bits double, rather than adding one: bit 0 every edge, bit 1 every 2, bit 2 every 4.")
  (hint "Bit n changes every 2 to the power n edges."))
```

---

## The shift register

Flip-flops in a chain, each one's output feeding the next one's input,
make a **shift register**. At each rising edge, every bit moves one
place along, and a new bit comes in at the end.

Bits sent one after another on one wire are **serial**; bits side by
side on several wires at once are **parallel**. A shift register turns
one into the other: load 8 bits in one at a time, then read all 8
together. Every serial connection,
from a keyboard cable to the link between two computers, has a shift
register at each end.

```figure
(define (il-lamp x y v)
  (if (= v 1) (circle x y 9 'ink) (ring x y 9 'ink 1.5)))
(define tick (floor (seconds)))
(define pattern '(1 0 1 1 0 0 1 0))
(define (in k) (if (< k 0) 0 (list-ref pattern (modulo k 8))))
(text 30 58 "next" 'centre 'small)
(il-lamp 30 76 (in (+ tick 1)))
(arrow 44 76 80 76 'ink 1.5)
(for-range 0 4
  (lambda (k)
    (let ((v (in (- tick k))) (x (+ 84 (* 52 k))))
      (frame x 54 44 44 'ink 1.5)
      (if (= v 1) (rect (+ x 3) 57 38 38 'ink))
      (text (+ x 22) 82 (fmt v 0) 'centre 'big (if (= v 1) 'paper 'ink))
      (if (< k 3) (arrow (+ x 44) 76 (+ x 52) 76 'ink 1.5)))))
(text 160 140 "the bits come in on one wire, one a second," 'centre 'small)
(text 160 158 "and can be read from all four boxes at once" 'centre 'small)
(caption "A 4-bit shift register: each edge moves every bit right.")
```

```check
(text
  (ask "A 4-bit shift register holds 1011. At the next edge a 0 comes in on the left, and every bit moves one place right. What does it hold then?")
  (accept "0101")
  (wrong-if "0110" "That moves the bits left. Here they move right: the 0 enters on the left, and the rightmost 1 falls off the end.")
  (wrong-if "1101" "That puts the new bit on the right. It comes in on the left, pushing the others right.")
  (hint "Write the new 0 first, then the first three bits of 1011."))
```

---

```check
(choice
  (ask "A keyboard sends each key's code down one wire, a bit at a time. What in the computer gathers the bits into a whole byte?")
  (right "A shift register")
  (wrong "A counter"
    "A counter adds one at each edge; it does not take bits in from a wire. Gathering serial bits into parallel is a shift register's job.")
  (wrong "A decoder"
    "A decoder turns a number that is already whole into one active line. The bits must first be gathered, one at a time.")
  (hint "Which circuit turns serial bits into parallel ones?"))
```

So: a register holds a number, a counter adds one at every edge, and a
shift register moves bits along, turning serial into parallel.
