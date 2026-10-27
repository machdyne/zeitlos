# Look-up tables

```lesson
(objective "Say how a look-up table in an FPGA computes any function of its inputs, and write the bits it holds for a simple gate.")
(minutes 20)
(keywords "FPGA" "field-programmable gate array" "logic block" "look-up table" "LUT")
```

You will see how one chip can become any circuit.

An **FPGA**, a **field-programmable gate array**, is a chip with a
large grid of identical, uncommitted **logic blocks**, and wiring
between them that can be switched to join any block to others. Loading
a configuration into the chip sets what each block does and how the
blocks are joined. The same chip can become a blinking light, a serial
port or a whole computer.

The chip itself never changes: its transistors are fixed when it is
made. What changes is a large number of stored bits that set up the
blocks and their wiring.

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: The Big Picture")
```

```check
(choice
  (ask "An FPGA is given a different configuration. What changes?")
  (right "What its logic blocks do and how they are joined")
  (wrong "The transistors inside the chip are rebuilt"
    "Nothing in the chip is rebuilt: its transistors are fixed when it is made. Only stored bits change, and those bits set up the blocks and the wiring.")
  (wrong "Only the program that its processor runs"
    "An FPGA has no fixed processor that runs a program. The configuration sets up the circuit itself, which can be a processor or something else entirely.")
  (hint "Think of what is fixed when a chip is made, and what can be stored in it afterwards."))
```

---

## A table in memory

The main part of a logic block is a **look-up table**, or **LUT**. A
4-input LUT is a tiny memory of 16 bits. Its four inputs, a, b, c and
d, form an address from 0 to 15, with a worth 1, b worth 2, c worth 4
and d worth 8. Its output is the bit stored at that address.

Change the inputs. The framed row is the address they make, and the
output is the bit stored there.

```figure
(define a (if (toggle 'a "a") 1 0))
(define b (if (toggle 'b "b") 1 0))
(define c (if (toggle 'c "c") 1 0))
(define d (if (toggle 'd "d") 1 0))
(fl-lut 26214 a b c d)
(caption "The output is the stored bit at the address the inputs make.")
```

```check
(number
  (ask "How many bits does a 4-input LUT store?")
  (answer 16) (from "2^4")
  (wrong-if 4 "4 is the number of inputs. The inputs make an address, and there is one stored bit for every address they can make.")
  (wrong-if 8 "Three inputs would make 8 addresses. Four inputs make twice as many.")
  (hint "4 bits of address make how many different addresses?"))
```

---

## Any function

In Digital Logic you met the truth table: a row for
every combination of inputs, and the output for each. A function of
four inputs has 16 rows. A LUT simply stores that column of 16 outputs.

So a LUT can be an AND gate, an XOR gate, a multiplexer or any other
function of up to four inputs. An FPGA's general logic has no fixed
gates at all, only small memories that can stand in for any of them.

Choose a function and try the inputs.

```figure
(define a (if (toggle 'a "a") 1 0))
(define b (if (toggle 'b "b") 1 0))
(define c (if (toggle 'c "c") 1 0))
(define d (if (toggle 'd "d") 1 0))
(define f (choice 'f '("XOR" "AND" "sum" "carry") "Function"))
(fl-lut (list-ref '(26214 34952 38550 59624) f) a b c d)
(caption "XOR, AND: of a and b. Sum, carry: of adding a, b and c.")
```

```check
(choice
  (ask "A LUT holding an AND gate and a LUT holding an XOR gate: which gives its answer sooner?")
  (right "Neither: both take the same time")
  (wrong "The AND gate, because AND is simpler"
    "A LUT does not build the gate. Whatever it holds, it does the same thing: it reads one stored bit at the address its inputs make.")
  (wrong "The XOR gate, because it has more 1s stored"
    "How many 1s are stored makes no difference. Reading one bit of the memory takes the same time whichever bit it is.")
  (hint "What does a LUT actually do, whatever it holds?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Inside an FPGA: Look-Up Tables")
```

---

## The contents as a number

Design tools write a LUT's 16 bits as one 16-bit number, often called
INIT. Bit 0 is the output for address 0, bit 1 the output for address
1, and so on. Written as binary numbers are, address 15 is on the left.

```example
(example
  (problem "Write the contents of a LUT that gives a XOR b, in hex.")
  (step "Addresses 0, 1, 2 and 3 have b a = 00, 01, 10, 11. The XOR of each is 0, 1, 1, 0.")
  (step "With address 3 on the left, that is 0110: hex 6.")
  (step "Inputs c and d do not change the answer, so the same 4 bits repeat for addresses 4-7, 8-11 and 12-15.")
  (answer "6666 in hex: the first function in the figure."))
```

```check
(text
  (ask "A LUT gives a AND b AND c AND d: 1 only when all four inputs are 1. Write its contents in hex.")
  (accept "8000") (accept "0x8000")
  (wrong-if "0001" "That puts the 1 at address 0, where every input is 0. All four inputs 1 is address 15, the leftmost bit.")
  (wrong-if "0x0001" "That puts the 1 at address 0, where every input is 0. All four inputs 1 is address 15, the leftmost bit.")
  (wrong-if "FFFF" "That is 1 at every address. AND gives 1 at only one address.")
  (hint "Which address has all four inputs 1?")
  (hint "Address 15 is the leftmost of the 16 bits: 1000 0000 0000 0000.")
  (hint "Write each group of four bits as one hex digit."))
```

---

```check
(text
  (ask "A LUT gives a OR b, and ignores c and d. Write its contents in hex.")
  (accept "EEEE") (accept "0xEEEE")
  (wrong-if "7777" "That is the right four bits in the wrong order: 0111. Address 0 is on the right, and a OR b is 0 only there.")
  (wrong-if "0x7777" "That is the right four bits in the wrong order: 0111. Address 0 is on the right, and a OR b is 0 only there.")
  (wrong-if "FFFE" "That is the OR of all four inputs, 0 only at address 0. Here c and d are ignored, so the 4-bit pattern for addresses 0-3 repeats.")
  (hint "For addresses 0 to 3, a OR b is 0, 1, 1, 1.")
  (hint "With address 3 on the left: 1110, hex E.")
  (hint "The pattern repeats for each value of c and d."))
```

---

## More outputs, more inputs

A LUT has one output. A function with two outputs needs two LUTs, one
for each. A function of more than four inputs is split among several
LUTs.

FPGA families use LUTs of 4 to 6 inputs, but the idea is the same.

```check
(number
  (ask "A full adder adds three bits, a, b and a carry in. It has two outputs, the sum and the carry out. How many 4-input LUTs does it need?")
  (answer 2)
  (wrong-if 1 "One LUT has one output. The sum and the carry out are two different functions.")
  (wrong-if 3 "Three is the number of inputs, and one LUT takes up to four. What sets the count here is the outputs.")
  (hint "Count the outputs. Each LUT gives one."))
```

---

```check
(number
  (ask "Some FPGAs have 6-input LUTs. How many bits does each one store?")
  (answer 64) (from "2^6")
  (wrong-if 6 "6 is the number of inputs. There is one stored bit for every address the inputs can make.")
  (wrong-if 32 "32 is what 5 inputs address. One more input doubles it.")
  (hint "6 inputs make 2 to the power 6 addresses."))
```

So: a LUT is a small memory that stores a truth table, which is why an
FPGA can become any logic circuit.
