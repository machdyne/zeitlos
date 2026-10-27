# Carry chains, pins and the clock

```lesson
(objective "Count the flip-flops and carry cells a counter needs, and say how a design reaches the board's pins and its 48 MHz clock.")
(minutes 20)
(keywords "carry chain (FPGA)" "carry cell (ECP5)" "bank (of pins)" "constraints file (FPGA)" "CLK_48 (pin)" "LED_B (pin)")
```

Counters and adders need a carry from each bit to the next. Sent
through the ordinary wiring, a carry would be slow, so the ECP5 has a
**carry chain**: a short, fast, fixed path from each slice to the
next.

The ECP5's unit of arithmetic is the **carry cell**. It uses the two
LUTs of a slice and adds two bits, passing the carry on to the next
cell in the chain. So an n-bit counter needs n flip-flops, to hold the
count, and n/2 carry cells, rounded up, to add one to it.

```figure
(define n (slider 'n 4 32 24 "Counter bits" 1))
(define cells (ceiling (/ n 2)))
(define w (/ 288 16))
(text 16 30 (string-append (fmt n 0) " flip-flops, " (fmt cells 0) " carry cells") 'body 'bold)
(for-range 0 cells
  (lambda (k)
    (let ((x (+ 16 (* k w))) (half (and (= k (- cells 1)) (odd? n))))
      (if half
          (rect (+ x 3) 63 (- (/ w 2) 3) 24 'warm)
          (rect (+ x 3) 63 (- w 6) 24 'warm))
      (frame x 60 w 30 'ink 1)
      (if (< k (- cells 1)) (arrow (+ x w -3) 98 (+ x w 3) 98 'ink 1)))))
(text 16 120 "carry in at the left, out at the right" 'small)
(if (odd? n) (text 16 140 "the last cell adds only one bit" 'small))
(define half-cycles (expt 2 (- n 1)))
(define secs (/ half-cycles 48000000))
(text 16 185 (string-append "top bit high for " (fmt half-cycles 0) " cycles") 'small)
(text 16 210 (if (< secs 0.001)
                 (string-append "at 48 MHz: " (fmt (* secs 1000000) 2) " microseconds")
                 (string-append "at 48 MHz: " (fmt secs 3) " s, then as long low")) 'small)
```

```sources
(source "docs/zfpga.md, sections 5.1 and 19.2")
```

```check
(number
  (ask "A 24-bit counter: how many carry cells does it need?")
  (answer 12) (from "24 / 2")
  (wrong-if 24 "24 is the number of flip-flops. Each carry cell adds two bits.")
  (wrong-if 6 "A carry cell adds two bits, not four.")
  (hint "Each carry cell handles two of the 24 bits."))
```

---

```check
(number
  (ask "And a 25-bit counter?")
  (answer 13) (true "12 * 2 < 25") (true "13 * 2 >= 25")
  (wrong-if 12 "12 cells add only 24 bits. The 25th bit needs one more cell, half of it unused.")
  (wrong-if 12.5 "There is no half cell: the last one is used for one bit only.")
  (hint "Divide 25 by 2 and round up."))
```

---

## Pins

Each pin of the FPGA has its own I/O block, which a design sets as an
input or an output, with a voltage standard. Many pins on Zeitlos's
boards use LVCMOS33: 3.3 V logic.

Pins come in groups called **banks**, and every pin in a bank shares
one supply voltage. So zfpga refuses a design that puts outputs of two
voltages in one bank.

A design's pins are named in a **constraints file**, a `.lpf` file for
the ECP5. On a Lakritz the 48 MHz clock arrives on the pin named A7,
and the blue LED is driven from pin A2:

```text
LOCATE COMP "CLK_48" SITE "A7";   IOBUF PORT "CLK_48" IO_TYPE=LVCMOS33;
LOCATE COMP "LED_B"  SITE "A2";   IOBUF PORT "LED_B"  IO_TYPE=LVCMOS33;
```

```sources
(source "docs/zfpga.md, section 5.1; docs/zfpga-formats.md, section 2.1")
(source "Timeless Computing (docs/tc.md), Inside an FPGA: I/O Blocks; Describing Hardware: From Description to Bitstream")
```

```check
(choice
  (ask "Why can two outputs in one bank not use 3.3 V and 1.8 V?")
  (right "Every pin in a bank shares one supply voltage")
  (wrong "An FPGA can only drive 3.3 V"
    "The ECP5 has several voltage standards, LVCMOS33 to LVCMOS12. What it cannot do is run one bank at two voltages.")
  (wrong "zfpga only knows LVCMOS33"
    "zfpga also takes LVCMOS25, 18, 15 and 12. The limit is the bank's supply, which the board fixes.")
  (hint "What do the pins of one bank share?"))
```

---

## Two names on every board

Each Machdyne board's constraints file is different, because its pins
are wired differently. But every one of them gives two pins the same
names: **CLK_48**, the input from the board's 48 MHz oscillator, and
**LED_B**, an output to an LED. So a design that uses only those two
names builds for any of these boards; only the constraints file
changes.

The same 48 MHz clock drives Zeitlos's processor. Large designs carry
their clocks on the ECP5's global clock wiring; zfpga does not use it
yet, which is fine for small designs and one reason it cannot build
large ones.

```sources
(source "boards/*.lpf in the Zeitlos source; docs/zfpga.md, sections 5.2 and 23.3")
```

```check
(number
  (ask "How many clock cycles does CLK_48 give in one second?")
  (answer 48000000) (from "48 * 1000000")
  (wrong-if 48000 "That is 48 kHz. Mega means a million: 48 million cycles a second.")
  (wrong-if 48 "48 is the number of megahertz. Each megahertz is a million cycles a second.")
  (hint "48 MHz is 48 million cycles a second."))
```

---

```check
(number
  (ask "A 24-bit counter on CLK_48. Its top bit, bit 23, stays high for 2^23 cycles. How many seconds is that? Give it to 2 places after the point.")
  (answer 0.17) (within 0.01) (unit "s") (from "2^23 / 48000000")
  (wrong-if 0.35 "That is 2^24 cycles, the top bit of a 25-bit counter. Bit 23 is high for 2^23.")
  (hint "2^23 is 8,388,608. Divide by 48,000,000."))
```

So: a counter is flip-flops and a carry chain, and a design reaches the
board through named pins, CLK_48 and LED_B among them.
