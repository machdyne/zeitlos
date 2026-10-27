# Registers and clocks

```lesson
(objective "Read a clocked always block in Verilog, and work out how fast a counter's bits change.")
(minutes 20)
(keywords "always block" "reg")
```

You will see how Verilog describes flip-flops, and read a whole small
design: a blinking light.

Combinational logic has no memory. Flip-flops and registers are
described with an **always block** that acts at each rising clock edge:

```verilog
reg [7:0] q;

always @(posedge clk)
    q <= d;
```

`always @(posedge clk)` means "at every rising edge of `clk`". Anything
assigned inside an always block is declared as a **reg**. `q <= d`
loads `q` with the value of `d` at the edge.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Registers and Clocks")
```

```check
(choice
  (ask "If d is an 8-bit wire, what hardware does this always block describe?")
  (right "An 8-bit register: 8 flip-flops that load d at each rising edge of clk")
  (wrong "A wire joining d to q"
    "A wire would follow d all the time. Here q changes only at the rising edge of clk, and keeps its value in between: that is a register.")
  (wrong "A loop that copies d to q again and again"
    "There is no loop running. The block describes flip-flops, which load their input at each clock edge.")
  (hint "Something that changes only at a clock edge, and holds its value between edges."))
```

---

## A blinking light

```verilog
// Blink an LED from a fast clock by counting clock cycles.
module blink #(
    parameter WIDTH = 24            // 2^24 cycles at 12 MHz: 1.4 s
) (
    input  wire clk,
    output wire led
);
    reg [WIDTH-1:0] count = 0;

    always @(posedge clk)
        count <= count + 1;

    assign led = count[WIDTH-1];    // the top bit changes slowest
endmodule
```

`count` is a 24-bit register that starts at 0 and goes up by one at
every rising edge: a counter of 24 flip-flops and an adder. The LED
(a small light) shows its top bit.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Registers and Clocks")
```

---

## Watching a counter

Here is a 4-bit version of `count`, cycle by cycle. Move the slider to
let more clock cycles pass.

```figure
(define n (slider 'n 1 16 10 "Cycles" 1))
(define x1 (+ 80 (* 14 n)))
(text 4 36 "clk" 'small)
(fl-clk 80 x1 20 18 7 80 'ink)
(for-range 0 4
  (lambda (i)
    (let ((y (+ 60 (* 36 i))))
      (text 4 (+ y 16) (string-append "count[" (fmt i 0) "]") 'small)
      (fl-wave 80 y 18 14 (fl-list n (lambda (j) (fl-bit j i))) 'ink))))
(text 160 220 (string-append "count = " (fl-bin (- n 1) 4) " = " (fmt (- n 1) 0)) 'centre 'body 'bold)
```

```check
(choice
  (ask "Which bit of the counter changes least often?")
  (right "count[3], the top bit")
  (wrong "count[0], the bottom bit"
    "count[0] changes at every rising edge, the most often of all. Each bit above it changes half as often.")
  (wrong "They all change equally often"
    "Look at the figure: count[0] changes every cycle, count[1] every 2, count[2] every 4. Each bit changes half as often as the one below.")
  (hint "Let 16 cycles pass, and count how often each line turns over."))
```

---

## How long a blink takes

In a counter, bit k changes every $2^k$ cycles. The top bit of a
24-bit counter is bit 23, so it changes every $2^23$ cycles. With a
12 MHz clock, 12,000,000 cycles pass each second.

```check
(number
  (ask "With a 12 MHz clock, how many seconds pass between changes of bit 23? (Two decimal places is enough.)")
  (answer 0.7) (within 0.01) (unit "s") (from "2^23 / 12000000")
  (wrong-if 1.4 "1.4 s is a whole blink, on and then off. The bit changes twice in that time.")
  (wrong-if 0.35 "That is 2 to the power 22 cycles. Bit 23 changes every 2 to the power 23.")
  (hint "2 to the power 23 is 8,388,608 cycles.")
  (hint "Divide it by 12,000,000 cycles a second."))
```

---

## Parameters

So the LED is on for 0.7 s and off for 0.7 s: one blink is the 1.4 s
in the comment.

`WIDTH` is a **parameter**: a constant that can be set differently for
each instance of the module. A design for testing can make a 4-bit
blink, which turns over after a few cycles instead of millions, and so
can be checked quickly.

```check
(number
  (ask "An instance of blink has WIDTH = 4. Every how many cycles does the LED change?")
  (answer 8) (from "2^3")
  (wrong-if 16 "16 cycles is one whole blink, on and off. The top bit, bit 3, changes every 2 to the power 3 cycles.")
  (wrong-if 4 "4 is the width. The top bit is bit 3, which changes every 2 to the power 3 cycles.")
  (hint "With WIDTH = 4 the top bit is bit 3."))
```

---

```check
(number
  (ask "A blink with WIDTH = 25 runs on a 12 MHz clock. How many seconds does one whole blink, on and off, take? (One decimal place.)")
  (answer 2.8) (within 0.05) (unit "s") (from "2^25 / 12000000")
  (wrong-if 1.4 "That is a blink with WIDTH = 24. One more bit doubles the time.")
  (hint "The top bit is bit 24; one whole blink is two of its changes.")
  (hint "2 to the power 25 cycles, divided by 12,000,000."))
```

So: an always block on a rising edge describes registers, and the bits
of a counter change more slowly the higher they are.
