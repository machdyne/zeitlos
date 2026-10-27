# Inside the CPU

```lesson
(objective "Name the parts inside a CPU, and read a few RISC-V instructions.")
(minutes 20)
(keywords "program counter" "arithmetic logic unit" "ALU" "control unit" "instruction set architecture" "ISA" "RISC" "CISC")
```

This lesson opens up the CPU and looks at the instructions it
understands.

A CPU keeps the numbers it is working on in its registers, which you
met in digital logic: each holds one word. This course uses **RISC-V**
processors, the kind in Zeitlos computers. Such a CPU has 32
registers, named x0 to x31, each 32 bits wide. Register **x0** always
reads as 0: writing to it changes nothing. That gives every instruction
a zero to use when it needs one.

Arithmetic is done between registers, so a program keeps its working
numbers there.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Inside the CPU; RISC-V Reference: Registers")
```

```check
(number
  (ask "A program writes 7 into register x0, then reads x0. What does it read?")
  (answer 0)
  (wrong-if 7 "Most registers would give back the 7. Register x0 is special: it always reads as 0, and writes to it are thrown away.")
  (hint "One register is wired to a constant. Which one, and what constant?"))
```

---

## The other parts

Besides its registers, a CPU has:

- the **program counter** (PC), a register that holds the memory
  address of the next instruction: the number that says where in
  memory it is;
- the **arithmetic logic unit** (**ALU**), built from adders and gates,
  which adds, subtracts, compares and does AND, OR and XOR on the
  numbers from the registers;
- the **control unit**, a state machine that steps through each
  instruction and directs the other parts, setting the multiplexers
  that route numbers between them.

```check
(choice
  (ask "Which part of the CPU says where the next instruction is?")
  (right "The program counter")
  (wrong "The ALU"
    "The ALU does the arithmetic and logic. Where the next instruction is kept is a separate register, the program counter.")
  (wrong "Register x0"
    "x0 always reads as 0, so it cannot keep track of anything. The program counter holds the next instruction's address.")
  (hint "It is a register whose name says what it counts through."))
```

---

## The instruction set

The set of instructions a CPU understands, and exactly what each one
does, is its **instruction set architecture** (**ISA**). The ISA is a
contract between hardware and software: a program written for an ISA
runs on any processor that has it, whoever built the processor and
however it is built inside.

RISC-V is an open ISA, first published in 2011, that anyone may build
without a licence. Its base set of instructions, **RV32I**, has about
forty instructions.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: The Instruction Set")
```

```check
(choice
  (ask "A program was built for RV32I on one maker's chip. Will it run on another maker's RV32I chip?")
  (right "Yes: both follow the same ISA")
  (wrong "Only if the chips are built the same way inside"
    "The ISA is the contract. How a processor is built inside can differ completely; if it runs the same instructions with the same effects, the program runs.")
  (wrong "No: each chip needs its own programs"
    "That is what an ISA prevents. Every processor that has the ISA runs the programs written for it.")
  (hint "What does the ISA promise?"))
```

---

## Reading instructions

An instruction names an operation and its registers. The first register
named is where the result goes:

| Instruction | Does |
|---|---|
| `add x7, x5, x6` | x7 = x5 + x6 |
| `sub x7, x5, x6` | x7 = x5 - x6 |
| `addi x5, x5, 3` | x5 = x5 + 3 |
| `slt x7, x5, x6` | x7 = 1 if x5 < x6, else 0 |

`addi` adds a constant written in the instruction itself. There is no
subtract-a-constant: `addi x5, x5, -1` takes 1 away.

```check
(number
  (ask "x5 holds 3 and x6 holds 4. The CPU runs add x7, x5, x6. What is in x7?")
  (answer 7) (from "3 + 4")
  (wrong-if 3 "That is x5. The result goes into the first register named, x7, and it is x5 + x6.")
  (hint "add x7, x5, x6 means x7 = x5 + x6."))
```

---

```check
(number
  (ask "x5 holds 10. The CPU runs addi x6, x5, -4. What is in x6?")
  (answer 6) (from "10 + (-4)")
  (wrong-if 14 "Adding -4 takes 4 away: 10 + (-4).")
  (wrong-if 10 "x5 is still 10, but the result goes into x6: x5 + (-4).")
  (hint "addi x6, x5, -4 means x6 = x5 + (-4)."))
```

---

```check
(number
  (ask "x5 holds 2 and x6 holds 9. The CPU runs slt x7, x5, x6. What is in x7?")
  (answer 1) (true "2 < 9")
  (wrong-if 0 "slt gives 1 when the first source is less than the second. Is 2 less than 9?")
  (wrong-if 9 "slt does not copy the larger number. It gives 1 if x5 < x6, else 0.")
  (hint "slt means set if less than: x7 = 1 if x5 < x6."))
```

---

## Extensions, RISC and CISC

Extra groups of instructions are **extensions**. The M extension adds
multiplying and dividing. A processor's full ISA is written as a name
such as **RV32IM**: 32-bit, the base set I, plus M.

ISAs are sometimes sorted into **CISC** (complex instruction set
computer), with many powerful instructions of different lengths, as in
the x86 family, and **RISC** (reduced instruction set computer), with
fewer, simpler, more regular instructions, as in Arm and RISC-V. Simple,
regular instructions are easier to build, to fit into an FPGA (a chip
whose circuits can be loaded like a program) and to understand.

```check
(choice
  (ask "A processor's ISA is RV32IM. What can it do that an RV32I processor cannot?")
  (right "Multiply and divide with single instructions")
  (wrong "Use 64-bit registers"
    "The 32 in the name says the registers are 32 bits. The M after the I is the extension for multiplying and dividing.")
  (wrong "Run programs written for x86"
    "x86 is a different ISA altogether. RV32IM is RV32I plus the M extension: multiply and divide.")
  (hint "Read the name in parts: 32, I, M."))
```

So: a CPU has registers, a program counter, an ALU and a control unit,
and its ISA lists the instructions it understands.
