# A pattern from a counter

```lesson
(objective "Write a small design with a clocked step register and a combinational case, and work out its timing.")
(minutes 20)
(keywords "step register (FPGA)")
```

A steady blink is one bit of a counter. A pattern -- two short flashes,
then a pause, like a heartbeat -- needs a little state machine. Save
this as `/home/beat.v`:

```verilog
// Heartbeat: two short flashes, then a pause.
module top(input CLK_48, output LED_B);
    reg [21:0] tick;
    reg [2:0]  step;
    reg        led;

    always @(posedge CLK_48) begin
        tick <= tick + 1;
        if (tick == 22'h3FFFFF)
            step <= step + 1;
    end

    always @(*) begin
        case (step)
            3'd0: led = 1'b1;
            3'd2: led = 1'b1;
            default: led = 1'b0;
        endcase
    end

    assign LED_B = led;
endmodule
```

```sources
(source "built with zfpga on the host, for this lesson: 8 LUTs, 25 flip-flops, 2 carry chains (13 cells)")
```

```check
(choice
  (ask "How many always blocks set led?")
  (right "One, the always @(*) block")
  (wrong "Two"
    "The clocked block sets tick and step only. led is set in one place, which is what zfpga requires.")
  (hint "Look for led = in each block."))
```

---

## How it works

`tick` counts every clock cycle. When it reaches its last value,
22'h3FFFFF, the **step register** `step` moves on by one. So `step`
goes 0, 1, 2 ... 7 and round again, staying at each value for 2^22
cycles.

The `case` lights the LED in steps 0 and 2, and the `default` line
keeps it dark in the rest: on, off, on, then five steps off.

```check
(number
  (ask "How long does step stay at each value, in seconds? Give it to 3 places after the point.")
  (answer 0.087) (within 0.001) (unit "s") (from "2^22 / 48000000")
  (wrong-if 0.175 "That is 2^23 cycles. tick has 22 bits, so it wraps after 2^22.")
  (hint "2^22 is 4,194,304 cycles, at 48,000,000 a second."))
```

---

```check
(number
  (ask "And one whole heartbeat, all 8 steps? Give it to 2 places after the point.")
  (answer 0.70) (within 0.01) (unit "s") (from "8 * 2^22 / 48000000")
  (wrong-if 0.087 "That is one step. A heartbeat is all 8 values of step.")
  (hint "8 steps of 0.087 s."))
```

---

## What zfpga makes of it

```text
zfpga: synthesised 8 LUTs, 25 flip-flops, 2 carry chains (13 cells)
```

25 flip-flops: 22 for `tick` and 3 for `step`. `led` is not a
flip-flop, although it is declared `reg`: it is set in an `always @(*)`
block, so it is plain logic. Two carry chains: one adds to `tick`, one
to `step`. The 8 LUTs test whether `tick` is all ones and pick the LED
from `step`.

```check
(choice
  (ask "Why is led not a flip-flop, though it is a reg?")
  (right "It is set in always @(*), so it is combinational logic, not stored at a clock edge")
  (wrong "zfpga removed it as unused"
    "led drives the LED pin, so it is used. It is built from LUTs, because nothing clocks it.")
  (wrong "A reg is never a flip-flop"
    "tick and step are regs and are flip-flops: they are set in a clocked block. Being a reg says only that an always block sets it.")
  (hint "Which kind of always block sets led?"))
```

---

## Your turn

Change the pattern to three flashes, in steps 0, 2 and 4. Add one line
to the `case`, build with zfpga, and check the report: the number of
flip-flops should not change.

```check
(text
  (ask "Write the line to add to the case for a third flash in step 4.")
  (accept "3'd4: led = 1'b1;") (accept "3'd4: led = 1;") (accept "4: led = 1'b1;") (accept "4: led = 1;") (accept "3'd4: led = 1'b1") (accept "3'd4 : led = 1'b1;")
  (wrong-if "3'd4: led <= 1'b1;" "<= is for clocked blocks. In always @(*) use =, as the other lines do.")
  (hint "Copy the line for step 2, and change the step.")
  (hint "3'd4: led = ..."))
```

So: a counter makes the time, a small register counts the steps, and
a case with a default turns the steps into a pattern without a latch.
