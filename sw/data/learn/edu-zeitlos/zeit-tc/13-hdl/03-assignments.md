# Assignments and state machines

```lesson
(objective "Choose between Verilog's two kinds of assignment, read a state machine in Verilog, and avoid the classic mistakes.")
(minutes 25)
(keywords "non-blocking assignment" "blocking assignment")
```

You will learn the most common beginner's mistake in Verilog, and how
to avoid it.

Inside an always block, `<=` is the **non-blocking assignment**. It
describes a register loading at the clock edge. All the non-blocking
assignments in a clocked block take effect together, using the values
from before the edge, as real flip-flops do. So this swaps two
registers:

```verilog
always @(posedge clk) begin
    a <= b;
    b <= a;
end
```

In hardware these are two flip-flops with their inputs crossed: `a`'s
input is wired to `b`'s output, and `b`'s to `a`'s.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Two Kinds of Assignment")
```

```check
(choice
  (ask "a holds 1 and b holds 2. After one rising edge of clk, what do they hold?")
  (right "a holds 2 and b holds 1")
  (wrong "Both hold 2"
    "That would be true if the second line saw the new a. With <=, both registers load at the same instant, from the values before the edge.")
  (wrong "a holds 2 and b still holds 2, until the next edge"
    "Both flip-flops load at the same edge. b takes a's old value, 1, at the same moment a takes 2.")
  (hint "Both load at once, each from the other's value before the edge."))
```

---

## The other kind

`=` is the **blocking assignment**. It takes effect at once, so the
next line sees the new value. Written with `=`, the swap's second line
would read the new `a`, and both registers would end with the old
value of `b`.

Toggle between the two and let clock edges pass.

```figure
(define blk (toggle 'blk "Written with ="))
(define k (slider 'k 0 4 0 "Clock edges" 1))
(define a (if blk (if (= k 0) 1 2) (if (= (modulo k 2) 0) 1 2)))
(define b (if blk 2 (if (= (modulo k 2) 0) 2 1)))
(frame 50 40 80 60 'ink 2)
(frame 190 40 80 60 'ink 2)
(text 90 34 "a" 'centre 'body 'bold)
(text 230 34 "b" 'centre 'body 'bold)
(text 90 82 (fmt a 0) 'centre 'big 'bold)
(text 230 82 (fmt b 0) 'centre 'big 'bold)
(text 160 140 (if blk "a = b;   b = a;" "a <= b;   b <= a;") 'centre 'body)
(text 160 170 "At the start: a is 1, b is 2." 'centre 'small)
(caption (if blk "With =, the second line reads the new a." "With <=, both load together: they swap."))
```

The rule: use `<=` in clocked blocks, and `=` in combinational ones,
written `always @*` ("whenever any input changes"). Do not mix them in
one block.

```check
(choice
  (ask "Which assignment belongs in an always @(posedge clk) block?")
  (right "<=, the non-blocking assignment")
  (wrong "=, the blocking assignment"
    "= takes effect at once, so a later line sees the new value, which is not how flip-flops behave. Clocked blocks use <=.")
  (wrong "Either, mixed as convenient"
    "Mixing them in one block is a classic source of mistakes. Clocked blocks use <= only.")
  (hint "Which one behaves like flip-flops all loading at the same edge?"))
```

---

## A state machine

The traffic light from Digital Logic, in Verilog:

```verilog
module traffic (
    input  wire clk,
    input  wire reset,
    input  wire done,               // the timer has run out
    output wire green, yellow, red
);
    localparam GREEN = 2'd0, YELLOW = 2'd1, RED = 2'd2;

    reg [1:0] state;

    always @(posedge clk) begin
        if (reset)
            state <= RED;
        else if (done)
            case (state)
                GREEN:   state <= YELLOW;
                YELLOW:  state <= RED;
                default: state <= GREEN;
            endcase
    end

    assign green  = (state == GREEN);
    assign yellow = (state == YELLOW);
    assign red    = (state == RED);
endmodule
```

`localparam` names the state numbers. The always block is the state
register and its next-state logic; `case` chooses by the present state.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: State Machines in Verilog")
```

```check
(choice
  (ask "The state is YELLOW, reset is 0 and done is 1. What is the state after the next rising edge?")
  (right "RED")
  (wrong "GREEN"
    "GREEN follows RED, through the default line. From YELLOW the case statement goes to RED.")
  (wrong "YELLOW, unchanged"
    "The state stays the same only when done is 0. Here done is 1, so it moves on.")
  (hint "Find the YELLOW line in the case statement."))
```

---

## A reset

Every state machine needs a **reset**: a known state to start from. An
FPGA gives its flip-flops known values when it is configured, but a
custom chip does not. A simulator shows unknown values until something
sets them, and a running circuit sometimes has to start again without
being loaded again.

```check
(choice
  (ask "The state is GREEN and done is 0, when reset becomes 1. What is the state after the next rising edge?")
  (right "RED")
  (wrong "GREEN, since done is 0"
    "reset is tested first. When it is 1, the state becomes RED whatever done is.")
  (wrong "YELLOW"
    "That is where GREEN goes when done is 1 and reset is 0. reset comes first and sends the state to RED.")
  (hint "Which if comes first in the always block?"))
```

---

## Classic mistakes

- **Two drivers.** A signal can be driven from one place only. A
  register assigned in two different always blocks is refused by the
  tools that build the design, even if a simulator accepts it. Put every assignment to it in one
  block:

```verilog
always @(posedge clk)
    if (start)
        busy <= 1'b1;
    else if (stop)
        busy <= 1'b0;
```

- **Width mismatches.** Assigning a 32-bit value to an 8-bit register
  quietly drops the top 24 bits. The tools warn about it: read the
  warnings.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Classic Mistakes")
```

```check
(choice
  (ask "One always block sets busy to 1 when start is 1, and a second always block sets busy to 0 when stop is 1. What is wrong?")
  (right "busy has two drivers; both assignments belong in one always block")
  (wrong "Nothing, as long as start and stop are never 1 together"
    "A flip-flop has one input, so the two blocks would be two pieces of hardware fighting over one signal. The tools refuse it whatever the inputs do.")
  (wrong "They should use = instead of <="
    "The kind of assignment is right for clocked blocks. The trouble is that two blocks drive the same signal.")
  (hint "From how many places may one signal be driven?"))
```

---

## Accidental latches

In a combinational `always @*` block, every output must be given a
value in every case. If an `if` has no `else`, the output must keep its
old value then, so the tools build a **latch**, a circuit that holds a
value, usually by mistake. This block gives `y` a value either way:

```verilog
always @*
    if (sel)
        y = a;
    else
        y = b;
```

```check
(choice
  (ask "The block above loses its else and its last line. What do the tools build?")
  (right "A latch, to keep y's old value when sel is 0")
  (wrong "Just a wire from a to y"
    "When sel is 0, nothing gives y a value, so y must keep the value it had. Keeping a value needs a latch.")
  (wrong "Nothing different: y is 0 when sel is 0"
    "Verilog does not give y 0 by itself. With no value given, y keeps its old one, which needs a latch.")
  (hint "What must y do when sel is 0 and nothing assigns it?"))
```

---

```check
(number
  (ask "A register is 8 bits. It is given the 32-bit value 32'h0000_01FF. What does it hold, in decimal?")
  (answer 255) (from "511 - 256")
  (wrong-if 511 "511 is 0x1FF, nine bits. An 8-bit register keeps only the low 8 bits: 0xFF.")
  (wrong-if 1 "That keeps the top part. The top bits are the ones dropped; the low 8 bits, 0xFF, are kept.")
  (hint "Keep only the last two hex digits."))
```

So: use `<=` in clocked blocks and `=` in combinational ones, give every
state machine a reset, drive each signal from one place, and give every
output a value in every case.
