# Modules and wires

```lesson
(objective "Read a small Verilog module of combinational logic, and read Verilog's vectors and numbers.")
(minutes 20)
(keywords "hardware description language" "HDL" "Verilog" "VHDL" "Verilog module" "vector (in Verilog)")
```

You will read your first circuits written as text.

Digital circuits of any size are no longer drawn: they are described
in a **hardware description language**, or **HDL**, and tools turn the
description into a chip or an FPGA's bitstream. Two are in common use:
**Verilog**, from 1984, used for the designs in this course, and
**VHDL**, from the same years. The ideas of one carry over to the other.

Verilog looks like the programming language C, but it is not a list of steps. It is a list of
pieces of hardware that all exist, and all work, at the same time.

```verilog
assign sum = a ^ b;
```

This line does not compute a value once. It places an XOR gate whose
output `sum` is always the XOR of `a` and `b`, for as long as the
circuit has power.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: A Different Kind of Language")
```

```check
(choice
  (ask "In a Verilog description, the line assign sum = a ^ b; comes after another assign line. When does it take effect?")
  (right "Always: it is an XOR gate, working for as long as there is power")
  (wrong "Once, after the line before it has run"
    "Verilog lines are not steps run in turn. Each assign is a piece of hardware, and all of them work at the same time.")
  (wrong "Only when the program reaches it in a loop"
    "There is no program running through the lines. The line describes a gate, and a gate works all the time.")
  (hint "Is this line a step, or a part of a circuit?"))
```

---

## Order does not matter

Because each line is a piece of hardware, the order of the lines in
the file does not change the circuit, just as the order in which parts
are listed for a circuit diagram does not change the circuit.

```check
(choice
  (ask "Two assign lines in a description are swapped. What happens to the circuit?")
  (right "Nothing: it is the same circuit")
  (wrong "The second gate now works before the first"
    "Both gates work all the time; neither goes first. Swapping their lines only changes the text.")
  (wrong "The circuit no longer works, because one uses the other's output"
    "A gate whose input is another gate's output is simply wired to it, wherever the lines are written.")
  (hint "Does the order of parts in a list change how they are wired?"))
```

---

## Modules

A Verilog design is built from **Verilog modules**, each with named
**ports**, its inputs and outputs. This is the full adder from Digital
Logic: it adds the bits `a`, `b` and a carry in, `cin`.

```verilog
module full_adder (
    input  wire a, b, cin,
    output wire sum, cout
);
    assign sum  = a ^ b ^ cin;
    assign cout = (a & b) | (cin & (a ^ b));
endmodule
```

A `wire` is a connection, carrying whatever drives it. Each `assign`
is combinational logic driving a wire, with `&` for AND, `|` for OR and
`^` for XOR. On an ECP5 FPGA this module becomes two LUTs.

```sources
(source "Timeless Computing (docs/tc.md), Describing Hardware: Verilog Modules")
```

```check
(number
  (ask "In the full adder, a = 1, b = 0 and cin = 1. What is cout?")
  (answer 1)
  (wrong-if 0 "a & b is 0, but the other half is cin & (a ^ b). a ^ b is 1 and cin is 1, so that half is 1, and cout is 1.")
  (hint "Work out a & b, then a ^ b, then cin & (a ^ b).")
  (hint "cout is 1 if either half is 1."))
```

---

## Instances

A module is used inside another by making an **instance** of it, as a
circuit board can carry several copies of one chip. Each instance is a
separate piece of hardware. A 32-bit adder could be made of 32 full
adder instances, and a computer is thousands of instances of hundreds
of modules.

```check
(number
  (ask "An adder for two 8-bit numbers is made of full adders, one for each bit. How many full adder instances does it have?")
  (answer 8)
  (wrong-if 16 "16 is the bits of both numbers together. Each full adder takes one bit of each number, so there is one for each bit position.")
  (wrong-if 1 "One full adder adds only one bit of each number. Each instance is a separate piece of hardware: one per bit.")
  (hint "One full adder for each bit position."))
```

---

## Vectors

A port or wire can carry many bits. `wire [7:0] data` declares an
8-bit **vector**, its bits numbered from 7, the most significant, down
to 0. Brackets pick bits: `data[0]` is the lowest bit, and `data[7:4]`
the top four.

```check
(number
  (ask "How many bits does wire [15:0] addr carry?")
  (answer 16) (from "15 - 0 + 1")
  (wrong-if 15 "The bits are numbered 15 down to 0, and 0 counts too: 16 bits.")
  (hint "Count the numbers from 15 down to 0, both included."))
```

---

## Numbers

Numbers are written with their width and base: `8'd200` is the 8-bit
decimal number 200, `4'b1010` is four bits in binary, and
`32'hE000_0000` is 32 bits in hex, with `_` only to make it easier to
read. Braces join vectors: `{4'b0000, data[7:4]}` is an 8-bit value
whose low four bits are the top four of `data`.

```example
(example
  (problem "data is 8'hA5. What is {4'b0000, data[7:4]}?")
  (step "8'hA5 is 1010 0101. Its top four bits, data[7:4], are 1010.")
  (step "Four 0s go in front: 0000 1010.")
  (answer "8'h0A, which is 10."))
```

```check
(number
  (ask "data is 8'h3C. What is {4'b0000, data[7:4]}, in decimal?")
  (answer 3)
  (wrong-if 12 "12 is C, the low four bits. data[7:4] is the top four: the hex digit 3.")
  (wrong-if 60 "60 is all of 8'h3C. The braces keep only data[7:4], with four 0s in front.")
  (hint "data[7:4] is the first hex digit of 8'h3C."))
```

---

```check
(text
  (ask "Write the 8-bit number 255 in Verilog, in hex.")
  (accept "8'hFF")
  (wrong-if "8'd255" "That is 255 written in decimal, which Verilog accepts, but the question asks for hex: h and two hex digits.")
  (wrong-if "255" "That has neither width nor base. Verilog writes the width, a quote, the base letter, then the digits.")
  (wrong-if "0xFF" "0x marks hex in C and in earlier lessons. Verilog writes the width, a quote and h: 8'h and the digits.")
  (hint "The form is width, ', base letter, digits.")
  (hint "Hex is h; 255 in hex is FF."))
```

So: a Verilog module describes hardware that all works at once, built
from wires, assign lines and instances of other modules.
