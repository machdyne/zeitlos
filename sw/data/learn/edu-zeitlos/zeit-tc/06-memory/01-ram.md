# The hierarchy and RAM

```lesson
(objective "Place each kind of memory in the hierarchy, and say how static and dynamic RAM hold a bit.")
(minutes 20)
(keywords "memory hierarchy" "random-access memory" "volatile (memory)" "non-volatile" "static RAM" "SRAM" "dynamic RAM" "DRAM" "refresh (DRAM)" "memory controller" "SDRAM" "PSRAM")
```

This lesson is about the memory a computer runs from.

Every kind of memory trades speed against size, cost and keeping its
data. So a computer uses several kinds at once, from small and fast to
large and slow: the **memory hierarchy**.

| Level | Typical size | Speed | Keeps data with power off |
|---|---|---|---|
| CPU registers | 128 bytes | fastest | no |
| cache | kilobytes | faster | no |
| main memory | MB to GB | fast | no |
| storage: flash memory, disk | KB to TB | slower | yes |
| archive: copies kept apart | any | slowest | yes |

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: The Memory Hierarchy")
```

```check
(choice
  (ask "RISC-V has 32 registers of 32 bits. How many bytes is that?")
  (right "128 bytes" (true "32 * 32 / 8 = 128"))
  (wrong "1,024 bytes" (false "32 * 32 / 8 = 1024")
    "32 times 32 is 1,024 bits. A byte is 8 bits, so divide by 8.")
  (wrong "32 bytes" (false "32 * 32 / 8 = 32")
    "Each register is 32 bits, which is 4 bytes, not 1.")
  (hint "32 registers, each 32 bits, which is 4 bytes."))
```

---

## Volatile and non-volatile

Main memory is **random-access memory**, RAM: any address can be read
or written as quickly as any other. Memory that loses its contents when
the power goes is **volatile**; memory that keeps them is
**non-volatile**.

A computer runs from volatile memory, because it is fast and can be
rewritten without limit. It keeps everything of lasting value in
non-volatile storage.

```check
(choice
  (ask "You type a letter and the power fails before you save it. Why is the letter lost?")
  (right "It was only in main memory, which is volatile")
  (wrong "The power cut damaged the storage"
    "The storage is fine: what was saved there is still there. The letter had never left volatile main memory.")
  (wrong "Main memory is too small to hold a letter"
    "Main memory holds millions of bytes, and a letter is a few thousand. The trouble is that it forgets when the power goes.")
  (hint "Which kinds of memory keep data with the power off?"))
```

---

## Static RAM

**Static RAM** (**SRAM**) keeps each bit in a small loop of
transistors, like the latch you met in digital logic. It holds its
value while it has power, needs no care, and is fast and simple: put an
address on its pins and the data appears.

Its drawback is size. Each bit needs about six transistors, so SRAM
costs a lot per bit. Caches are made of it, and so are the memory blocks
inside an FPGA. Some small computers use an SRAM chip of a megabyte or
two as their main memory.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Static RAM")
```

```check
(choice
  (ask "SRAM is fast and simple. Why is main memory in most computers not SRAM?")
  (right "About six transistors a bit make it costly for gigabytes")
  (wrong "SRAM forgets its data after a few milliseconds"
    "That is another kind of RAM, in the next step. SRAM keeps its bits as long as it has power.")
  (wrong "SRAM is non-volatile, so it cannot be rewritten"
    "SRAM is volatile and can be rewritten freely. Its drawback is the cost of six transistors for every bit.")
  (hint "Count the transistors in one bit."))
```

---

## Dynamic RAM

**Dynamic RAM** (**DRAM**) keeps each bit as a small charge on a
capacitor, with one transistor to reach it. That makes it dense and
cheap, so nearly every computer uses it for main memory.

But the charge leaks away. So that no bit is lost, every row of the
memory is read and written back, a **refresh**, at least every 64
milliseconds. Turn refresh off in the figure, a sketch of one leaky cell, to see
a 1 fade.

```figure
(define r (toggle 'refresh "Refresh" #t))
(define (q t) (exp (- (/ (if r (- t (* 64 (floor (/ t 64)))) t) 144))))
(graph 50 20 250 150 0 200 0 1)
(graph-axes 50 0.5)
(line (gx 0) (gy 0.5) (gx 200) (gy 0.5) 'grey 1)
(text 296 (- (gy 0.5) 4) "reads as 1 above" 'tiny 'right)
(plot q 'ink)
(text 54 14 "charge" 'tiny)
(text 175 200 "time (ms)" 'centre 'small)
(caption (if r "Refreshed in time, it is always read as 1" "Without refresh, the 1 fades into a 0"))
```

```check
(number
  (ask "Each row must be refreshed at least once every 64 ms. At least how many times a second is that?")
  (answer 15.625) (within 0.4) (from "1000 / 64")
  (wrong-if 64 "64 is the milliseconds between refreshes. A second is 1,000 ms: 1000 / 64.")
  (hint "A second is 1,000 milliseconds."))
```

---

## The memory controller

DRAM is also harder to talk to: it is laid out in rows and columns that
must be opened and closed in the right order. A **memory controller**,
a circuit between the bus and the memory chips, does this and the
refresh. In an FPGA computer, it is part of the design loaded into the
FPGA. Common kinds of DRAM:

- **SDRAM** (synchronous DRAM) moves data in step with a clock;
- DDR SDRAM, such as DDR3, moves data on both edges of the clock, twice
  as fast, with a much harder controller;
- **PSRAM** (pseudo-static RAM) is DRAM with its own refresh built in,
  so it can be used almost like SRAM.

Zeitlos boards differ: their main memory is SRAM, SDRAM, PSRAM or DDR3.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Dynamic RAM")
(source "Zeitlos docs: hwmap.md; source rtl/sysctl.v, the main-memory decoder")
```

```check
(choice
  (ask "What makes PSRAM easier to use than plain DRAM?")
  (right "It refreshes itself, so the computer need not")
  (wrong "It keeps its data with the power off"
    "PSRAM is DRAM inside, so it is volatile like DRAM. What it adds is its own refresh.")
  (wrong "It stores each bit in six transistors"
    "That is SRAM. PSRAM is DRAM with a refresh circuit built in.")
  (hint "Pseudo-static: it behaves almost like static RAM. What does static RAM not need?"))
```

So: a computer runs from fast, volatile RAM, static or dynamic, and
keeps what lasts in slower, non-volatile storage.
