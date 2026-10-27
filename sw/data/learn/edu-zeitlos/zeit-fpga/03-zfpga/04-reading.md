# Reading what a build made

```lesson
(objective "Read zfpga's report and its logical netlist, and work out from a design how fast its LED blinks.")
(minutes 20)
(keywords "ccu2 (FPGA)")
```

Here is the design you built, `blink.v`:

```verilog
module top(input CLK_48, output LED_B);
	reg [23:0] ctr;
	always @(posedge CLK_48) ctr <= ctr + 1;
	assign LED_B = ctr[23];
endmodule
```

A 24-bit counter adds one at every edge of the 48 MHz clock, and its
top bit drives the LED. The report said:

```text
zfpga: synthesised 0 LUTs, 24 flip-flops, 1 carry chains (12 cells)
```

24 flip-flops hold the count, and a chain of 12 carry cells adds one
to it. There are no other LUTs: the adding is all in the carry cells,
and the LED is wired straight to bit 23.

```sources
(source "sw/apps/zfpga/examples/blink.v; docs/zfpga-formats.md, section 1")
```

```check
(choice
  (ask "Why does the report say 0 LUTs, when carry cells are made of LUTs?")
  (right "It counts the carry cells apart from the plain LUTs, and this design needs no plain LUTs")
  (wrong "Because zfpga failed to synthesise the adder"
    "The adder is there: it is the chain of 12 carry cells. They are counted on their own.")
  (wrong "Because the counter is made of flip-flops only"
    "Flip-flops only hold the count. Something must add one to it each cycle: the carry chain.")
  (hint "Look at what else the line counts."))
```

---

## The logical netlist

Open `/data/zfpga/examples/blink.zl` in the text editor. Its lines
read like this (shortened):

```text
input CLK_48 A7 net=CLK_48 io_type=LVCMOS33
output LED_B A2 net=ctr[23] io_type=LVCMOS33
ccu2 _ch0_0 init0=0x060A ... a0=ctr[0] ... a1=ctr[1] ... cin=0 cout=_ch0_c0
ff ctr[0] d=_n27 q=ctr[0] clk=CLK_48 regset=RESET
```

A **ccu2** line is one carry cell: its two halves add one bit each,
`cin` is the carry in and `cout` the carry out to the next. Each `ff`
line is one flip-flop. The nets keep the Verilog's names, `ctr[0]` to
`ctr[23]`, so you can find your design in it.

```sources
(source "the .zl zfpga synth writes for blink.v; docs/zfpga-formats.md, sections 2.3 and 2.4")
```

```check
(text
  (ask "In blink.zl, which net does the output LED_B read? Write it as the file does.")
  (accept "ctr[23]")
  (wrong-if "ctr[24]" "The counter's bits are numbered 23 down to 0, so its top bit is ctr[23].")
  (hint "The design says assign LED_B = ctr[23]."))
```

---

## How fast it blinks

The top bit of an n-bit counter is 0 for 2^(n-1) cycles, then 1 for
as many. With n = 24 at 48 MHz:

```example
(example
  (problem "How long is the LED on, for blink.v?")
  (step "The top bit is bit 23, so it stays 1 for 2^23 = 8,388,608 cycles.")
  (step "At 48,000,000 cycles a second, that is 8,388,608 / 48,000,000 seconds.")
  (answer "About 0.17 s on, then 0.17 s off: about three blinks a second."))
```

```check
(number
  (ask "blinkh.v uses a 25-bit counter's top bit. How many seconds is its LED on? Give it to 2 places after the point.")
  (answer 0.35) (within 0.01) (unit "s") (from "2^24 / 48000000")
  (wrong-if 0.17 "That is the 24-bit counter. One more bit doubles the time.")
  (hint "The top bit of a 25-bit counter is bit 24: 2^24 cycles."))
```

---

```check
(number
  (ask "blinkf.v uses bit 3 of an 8-bit counter. For how many clock cycles is its LED on each time?")
  (answer 8) (from "2^3")
  (wrong-if 128 "That is bit 7, the top of the 8-bit counter. The design reads bit 3.")
  (wrong-if 16 "16 cycles is a whole blink, on and off. Bit 3 stays 1 for 2^3 cycles.")
  (hint "Bit k stays 1 for 2^k cycles."))
```

---

## Too fast to see

8 cycles at 48 MHz is a sixth of a microsecond. The LED of `blinkf.v`
turns on and off three million times a second, and looks steadily
half lit. That design exists to be simulated, where 8 cycles is
quick to watch; a counter you can see blink needs well over 20 bits.

```check
(number
  (ask "A whole blink of blinkf.v is 16 cycles. How many blinks is that a second at 48 MHz?")
  (answer 3000000) (from "48000000 / 16")
  (hint "Divide 48,000,000 by 16."))
```

So: the report counts LUTs, flip-flops and carry cells, the .zl keeps
your names, and bit k of a counter at 48 MHz stays on for 2^k cycles.
