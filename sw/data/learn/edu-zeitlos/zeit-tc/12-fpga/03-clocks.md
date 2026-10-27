# Clocks and clock domains

```lesson
(objective "Say how an FPGA makes and delivers its clocks, and how a signal safely crosses from one clock to another.")
(minutes 20)
(keywords "global clock network" "PLL" "phase-locked loop" "clock domain" "dual-clock FIFO" "timing report")
```

You will see where an FPGA's clocks come from, and what happens where
two clocks meet.

A clock has to reach every flip-flop on the chip at nearly the same
moment. Ordinary routing cannot do that, so FPGAs have **global clock
networks**: routing built to carry a clock to the whole chip at once.

They also have one or more **phase-locked loops**, or **PLLs**:
circuits that take the clock from a crystal on the board and make
clocks of other frequencies from it. A board with a 12 MHz crystal can
use a PLL to make 48 MHz for its processor and 25 MHz for its video.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Clocks")
```

```check
(number
  (ask "A PLL makes a 48 MHz clock from a 12 MHz crystal. How many times faster is the new clock?")
  (answer 4) (from "48 / 12")
  (wrong-if 36 "36 MHz is the difference between them. How many times faster is a ratio: 48 divided by 12.")
  (hint "Divide the new frequency by the crystal's."))
```

---

## Clock domains

A large design often has several clocks: one for the processor, one
for the video at its pixel rate, one for USB. All the flip-flops run
by one clock make a **clock domain**.

Inside a domain the timing rule holds: the logic between flip-flops
settles before the next edge. Between two domains it does not, because
the edges of two unrelated clocks fall at moments with no fixed
relation to each other. Change clock B and watch where its rising
edges fall against A's, marked by the dotted lines.

```figure
(define pb (slider 'pb 18 46 29 "Clock B period" 1))
(text 8 66 "A" 'body 'bold)
(text 8 146 "B" 'body 'bold)
(fl-clk 30 312 40 30 16 40 'ink)
(fl-clk 30 312 120 30 (/ pb 2) 37 'ink)
(for-range 0 9
  (lambda (k) (let ((x (+ 40 (* 32 k)))) (if (< x 312) (line x 30 x 170 'grey 1)))))
(caption "The dotted lines are A's rising edges.")
```

```check
(choice
  (ask "A flip-flop clocked by A reads a signal made in clock B's domain. Why can it not count on the signal being settled?")
  (right "B's edges can fall at any moment relative to A's")
  (wrong "Clock B is always faster than A"
    "Speed is not the trouble: a slower clock B gives the same problem. Its edges have no fixed relation to A's, so a change can arrive just as A samples.")
  (wrong "Signals cannot travel between clock domains at all"
    "They can and must, but with care. The trouble is only that a change can arrive at the very moment A's flip-flop samples it.")
  (hint "Look at where B's edges fall against the dotted lines."))
```

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Clock Domains")
```

---

## One bit across

A signal that crosses into another domain is treated as if it came
from outside the chip. If it changes just as a flip-flop samples it,
the flip-flop can be caught between 0 and 1 for a moment.

So **a single bit**, such as a flag, passes through two flip-flops in a
row in the receiving domain before it is used. The first may be caught
for a moment; it has a whole clock cycle to settle before the second
reads it.

```check
(choice
  (ask "A button's signal comes into a design. What should the design do before using it?")
  (right "Pass it through two flip-flops on the design's clock")
  (wrong "Use it at once, since it is only one bit"
    "One bit can still change just as a flip-flop samples it, and catch it between 0 and 1. Two flip-flops in a row give it a cycle to settle.")
  (wrong "Make a slower clock from it"
    "A button is not a clock. Its signal is brought into the design's own clock domain, through two flip-flops.")
  (hint "A button belongs to no clock domain: it is like a signal from another one."))
```

---

## Many bits across

A number of several bits cannot cross that way. Its bits change at
slightly different moments, and the receiver may take some old bits
and some new.

Here a 4-bit value changes from 0111 (7) to 1000 (8). Move the moment
at which the other domain reads it.

```figure
(define t (slider 't 0 29 15 "Moment read" 1))
(define old '(1 1 1 0))
(define at '(14 19 17 12))
(define (b i) (if (< t (list-ref at i)) (list-ref old i) (- 1 (list-ref old i))))
(for-range 0 4
  (lambda (k)
    (let ((i (- 3 k)) (y (+ 24 (* 34 k))))
      (text 8 (+ y 16) (string-append "bit " (fmt i 0)) 'small)
      (fl-wave 64 y 18 8
        (fl-list 30 (lambda (j) (if (< j (list-ref at i)) (list-ref old i) (- 1 (list-ref old i)))))
        'ink))))
(line (+ 64 (* 8 t)) 16 (+ 64 (* 8 t)) 160 'ink 2)
(define v (+ (b 0) (* 2 (b 1)) (* 4 (b 2)) (* 8 (b 3))))
(text 160 188 (string-append "read: " (fl-bin v 4) " = " (fmt v 0)) 'centre 'body 'bold)
(text 160 210 (if (or (= v 7) (= v 8)) "a value that was sent" "a value that never existed") 'centre 'small)
```

```check
(choice
  (ask "Read at a bad moment, which of these values could the receiver get?")
  (right "15, with bit 3 already new and the others still old" (true "8 + 4 + 2 + 1 = 15"))
  (wrong "Only 7 or 8"
    "That would be true if all four bits changed at the same instant. They change at slightly different moments, so a read can catch some old and some new.")
  (wrong "Nothing at all until the bits settle"
    "A flip-flop always holds a 0 or a 1 after a moment. The receiver gets a value; the trouble is that it can be one that was never sent.")
  (hint "Move the line to just after bit 3 changes."))
```

---

## Handshakes and FIFOs

A number of several bits crosses with a **handshake**: the sender holds
the value steady and raises a single flag, which crosses safely; the
receiver reads the value and answers with a flag of its own.

A stream of data, such as sound samples or pixels, goes through a
**dual-clock FIFO** (first in, first out): a small buffer in block RAM
that one domain writes and the other reads.

A reset crosses with the same care: it can start at any moment, but
must end in step with each domain's clock, or some flip-flops leave
reset a cycle before others.

```check
(choice
  (ask "The video circuit, on its own pixel clock, needs a steady stream of pixels from the processor's domain. What carries them?")
  (right "A dual-clock FIFO")
  (wrong "Two flip-flops in a row"
    "Two flip-flops are for one bit, such as a flag. A stream of many-bit values goes through a FIFO that one domain writes and the other reads.")
  (wrong "A wire straight from one domain to the other"
    "A many-bit value on a plain wire can be read half old and half new, as in the figure before. A stream needs a dual-clock FIFO.")
  (hint "Which of these is made for a stream of data?"))
```

---

## Meeting timing

The design tools are told the frequency of each clock, and check every
path. Their **timing report** says whether each clock meets its
target. A design that fails timing may still seem to work, then fail
when the chip is warm, when its supply is a little low, or on another
chip of the same type.

The remedies: shorten the slow paths, split a long calculation over
several clock cycles, or lower the clock.

```check
(choice (several)
  (ask "A design's timing report says its clock is too fast for its slowest path. Which of these are remedies?")
  (right "Lower the clock")
  (right "Split the long calculation over two clock cycles")
  (wrong "Keep it, since it works on the desk"
    "A design that fails timing can work on the desk and fail when warm, or on another chip. The report is the warning to act on.")
  (wrong "Use a second clock domain for the slow path"
    "Crossing domains makes the design harder, not faster, and the slow path is still slow. Shorten the path, split it, or lower the clock.")
  (hint "Either give the slow path more time, or give it less to do in one cycle."))
```

So: global networks carry clocks, PLLs make them, and a signal between
two clock domains crosses through two flip-flops, a handshake or a FIFO.
