# Machine code and assembly

```lesson
(objective "Say how an assembly instruction becomes a 32-bit number, and read the fields of one.")
(minutes 20)
(keywords "assembly language" "assembler" "machine code" "opcode" "immediate (in an instruction)" "compiler" "interpreter" "two's complement")
```

This lesson shows the number that an instruction really is.

The programs so far were written in **assembly language**: one line for
each instruction, with names a person can read. The CPU cannot read
text. A program called an **assembler** turns each line into the 32-bit
number that the CPU decodes: its **machine code**.

The instruction `addi x5, x0, 10` becomes the number `0x00A00293`.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Machine Code and Assembly")
```

```check
(choice
  (ask "What does the CPU fetch from memory when it runs addi x5, x0, 10?")
  (right "The 32-bit number 0x00A00293")
  (wrong "The letters a, d, d, i and the rest"
    "The words are for people. The assembler has already turned them into one 32-bit number, and that number is what is in memory.")
  (wrong "The number 10"
    "10 is only one part of it. The whole instruction, operation and registers too, is packed into one 32-bit word.")
  (hint "The CPU cannot read text. What does the assembler make?"))
```

---

## Fields

The 32 bits are cut into **fields**, groups of bits with one job each:

- the **opcode**, the lowest 7 bits: what kind of instruction it is;
- **rd**: the destination register, for the result;
- **rs1**: the source register;
- the **immediate**: a constant kept in the instruction itself (the i
  in `addi`).

The 3 bits beside rs1 pick the operation: 000 is add. Choose an
instruction, 1 to 4, to see its fields.

```figure
(define ch (choice 'ins '("1" "2" "3" "4") "Instruction"))
(define f (list-ref '((5 0 10) (6 0 0) (5 5 -1) (10 10 100)) ch))
(define rd (car f))
(define rs (cadr f))
(define im (caddr f))
(define im12 (modulo im 4096))
(define w (+ (* im12 1048576) (* rs 32768) (* rd 128) 19))
(text 160 30 (string-append "addi x" (fmt rd 0) ", x" (fmt rs 0) ", " (fmt im 0)) 'centre 'big 'bold)
(define (field x n v name)
  (frame x 54 (+ 8 (* 7 n)) 22 'ink 1)
  (text (+ x 4) 70 (cp-bin v n) 'small)
  (text (+ x 4 (* 3.5 n)) 90 name 'tiny 'centre))
(field 16 12 im12 "immediate")
(field 112 5 rs "rs1")
(field 159 3 0 "add")
(field 192 5 rd "rd")
(field 239 7 19 "opcode")
(text 16 48 "31" 'tiny)
(text 296 48 "0" 'tiny 'right)
(text 16 120 (string-append "immediate = " (fmt im 0) "    rs1 = x" (fmt rs 0) "    rd = x" (fmt rd 0)) 'small)
(text 16 140 "opcode 0010011 = arithmetic, immediate" 'small)
(text 160 180 (string-append "0x" (cp-hex w 8)) 'centre 'big 'bold)
(caption "The machine code, in hex")
```

```check
(text
  (ask "In an instruction, the rd field holds 00110. Which register gets the result?")
  (accept "x6") (accept "6")
  (wrong-if "x110" "00110 is binary. Read it as a number: 4 + 2.")
  (wrong-if "x5" "00101 is x5. 00110 is 4 + 2.")
  (hint "Read 00110 as a binary number: the places are worth 16, 8, 4, 2 and 1."))
```

---

## A negative constant

Instruction 3 in the last figure, `addi x5, x5, -1`, has the
immediate 111111111111. The immediate is 12 bits, and it is written in
**two's complement**, the usual way to write negative numbers in bits:
its top bit counts as -2048 instead of +2048. So twelve 1s are
-2048 + 2047 = -1.

```check
(number
  (ask "A 12-bit immediate field holds 111111111110. What number is it?")
  (answer -2) (from "4094 - 4096")
  (wrong-if 4094 "That is the pattern read as an unsigned number. The top bit is 1, so in two's complement it is negative: 4094 - 4096.")
  (wrong-if 2 "Close: the size is right but the top bit is 1, so the number is negative.")
  (hint "Twelve 1s is -1. This pattern is one less than that."))
```

---

## The same places

The fields sit in the same bit positions in most RISC-V instructions:
rd is always bits 7 to 11, rs1 always bits 15 to 19. So the decoder in
the CPU, a combinational circuit, stays small: it takes the word apart
the same way every time and sets the multiplexers and the ALU from the
pieces.

```check
(number
  (ask "The 12-bit immediate is a two's complement number. What is the largest it can hold?")
  (answer 2047) (from "2^11 - 1")
  (wrong-if 4095 "That is 12 bits unsigned. In two's complement the top bit means negative, so 11 bits are left for the size.")
  (wrong-if 2048 "2048 is 2 to the 11th, one more than fits. The largest is 2 to the 11th minus 1.")
  (hint "The top bit is the sign. The other 11 bits make at most 2 to the 11th minus 1."))
```

---

## Labels

A name such as `loop` is a **label**: a help that the assembler gives
you. In the machine code there is no name. The branch holds the
distance to its target, in bytes, and the CPU adds it to the program
counter. In the program from the last lesson, `loop` is at 0x08 and the
branch at 0x10, so the branch holds -8.

```check
(number
  (ask "A branch at address 0x20 goes back to a label at 0x14. What distance does its machine code hold?")
  (answer -12) (from "20 - 32")
  (wrong-if 12 "The target is before the branch, at a lower address, so the distance is negative.")
  (wrong-if -6 "0x20 and 0x14 are hex: 32 and 20. Subtract those.")
  (hint "0x14 is 20 and 0x20 is 32. The distance is target minus branch."))
```

---

## Above assembly

Large programs are now seldom written in assembly. They are written in
languages closer to how people think, then turned into machine code by
a **compiler**, a program that translates a whole program at once, or
run by an **interpreter**, a program that reads and does them line by line.
Either way, in the end every program runs as instructions like these.

```check
(choice
  (ask "A program is written in the language C and compiled. What does the CPU run?")
  (right "Machine code: 32-bit instructions made by the compiler")
  (wrong "The C text, a line at a time"
    "The CPU only decodes machine code. The compiler has already turned the C into instructions.")
  (wrong "Assembly language"
    "Assembly is still text for people. Whatever the language, what reaches the CPU is machine code.")
  (hint "What is the only thing the CPU's decoder understands?"))
```

So: an assembler turns each line of assembly into a 32-bit word of
machine code, cut into fields: the opcode, registers and an immediate.
