# Modules, parameters and include

```lesson
(objective "Build a design from two modules, with a parameter and an included file, and say what zfpga does with them.")
(minutes 20)
(keywords "flatten (a design)" "include file (FPGA)")
```

`/data/zfpga/examples/blinkh.v` is the blinky in two modules. A
`counter` module with a parameter, `WIDTH`, and a `top` that makes one
instance of it:

```verilog
`include "blinkh.vh"

module counter #(parameter WIDTH = 8) (input clk, output reg [WIDTH-1:0] q);
    always @(posedge clk) q <= q + 1'b1;
endmodule

module top(input CLK_48, output LED_B);
    wire [`BLINK_BITS-1:0] c;
    counter #(.WIDTH(`BLINK_BITS)) u_ctr (.clk(CLK_48), .q(c));
    assign LED_B = c[`BLINK_BITS-1];
endmodule
```

The instance `u_ctr` sets `WIDTH` to `` `BLINK_BITS``, whatever that is.

```sources
(source "sw/apps/zfpga/examples/blinkh.v and blinkh.vh")
```

```check
(choice
  (ask "Without the #(.WIDTH(...)) part, how wide would u_ctr's counter be?")
  (right "8 bits, the parameter's default")
  (wrong "As wide as c"
    "A parameter takes its default unless the instance sets it. Connecting a wider wire does not change it.")
  (wrong "zfpga would refuse it"
    "A parameter left at its default is fine; zfpga takes the 8 written in the module.")
  (hint "Look at parameter WIDTH = 8."))
```

---

## The include file

The first line pulls in `blinkh.vh`, an **include file**: its text is
read as if it were written there. It has one line:

```verilog
`define BLINK_BITS 25
```

So the counter is 25 bits wide, and the LED blinks at half
`blink.v`'s rate: 0.35 s on, 0.35 s off. zfpga looks for an include
file beside the file that includes it, so copy both if you move the
design:

```text
$ cp /data/zfpga/examples/blinkh.v /home/blinkh.v
$ cp /data/zfpga/examples/blinkh.vh /home/blinkh.vh
```

```sources
(source "docs/zfpga-formats.md, section 8.1; docs/zfpga-test.md, section 4")
```

```check
(number
  (ask "You change blinkh.vh to `define BLINK_BITS 26. How many seconds is the LED on? Give it to 2 places after the point.")
  (answer 0.70) (within 0.01) (unit "s") (from "2^25 / 48000000")
  (wrong-if 0.35 "That is 25 bits. With 26 the top bit is bit 25, high for 2^25 cycles: twice as long.")
  (hint "One more bit doubles the time: 0.35 s becomes ...")
  (hint "2^25 / 48,000,000."))
```

---

## Flattening

The FPGA has no modules: only LUTs, flip-flops and wires. So zfpga
**flattens** the design first: it copies each instance's logic into
the top module, with names that keep the path, such as `u_ctr.q[24]`.
The build says so:

```text
zfpga: top top: 1 instance flattened
zfpga: synthesised 0 LUTs, 25 flip-flops, 1 carry chains (13 cells)
```

zfpga takes as the top the one module nothing else makes an instance
of. With several such modules, name the top with `-t` on `zfpga synth`.

```sources
(source "docs/zfpga.md, section 24.2; docs/zfpga-formats.md, section 8")
```

```check
(choice
  (ask "Which module does zfpga take as the top of blinkh.v, and why?")
  (right "top, because no other module makes an instance of it")
  (wrong "counter, because it comes first in the file"
    "Order in the file does not matter. top makes an instance of counter, so counter cannot be the top.")
  (wrong "Whichever is named top: the name is required"
    "The name is not special. zfpga looks for the module nothing instantiates; -t names it when there is doubt.")
  (hint "Which module is used by the other?"))
```

---

```try
(try (app "term")
  (task "In posix, copy blinkh.v and blinkh.vh to /home as above, and run: zfpga build /home/blinkh.v -b NAME, with your board's name. Come back when it has written blinkh.bit.")
  (check (number
    (ask "How many flip-flops did it synthesise?")
    (answer 25)
    (wrong-if 8 "8 is the parameter's default. The instance sets WIDTH to BLINK_BITS, which is 25.")
    (hint "BLINK_BITS is 25."))))
```

So: zfpga reads several modules, parameters and include files, and
flattens them into one circuit before mapping it to the chip.
