# The processor core

```lesson
(objective "Describe the Zeitlos CPU and work out its speed from its clock and its instructions per cycle.")
(minutes 20)
(keywords "PicoRV32" "Zeitlos32" "MIPS" "CPI")
```

You will learn what the Zeitlos CPU is and how fast it runs.

The CPU is **PicoRV32**, a small, well-tested open RISC-V core written in
Verilog, kept unchanged in `rtl/cpu/picorv32`. Its instruction set is
**RV32IM**: the 32-bit base integer instructions plus the M extension,
multiply and divide in hardware. Every board builds the multiplier and
divider, because software built with `mul` instructions would fail on a
board without them.

It runs at **48 MHz on every board**. 48 rather than a rounder 50
because 48 MHz divides evenly for the serial port's 1 Mbaud.

```sources
(source "Zeitlos docs: boot.md, CPU speed check (Z_SYSCLK_HZ, 48 MHz on every board)")
(source "Zeitlos source: rtl/boards.vh, CPU_MUL, CPU_MUL_FAST and CPU_DIV (universal)")
```

```check
(choice
  (ask "Why do all boards build the hardware multiplier, even the small ones?")
  (right "So one build of the software runs everywhere: a mul on a board without it would be an illegal instruction")
  (wrong "Because RISC-V cannot run without multiply"
    "RV32I, the base set, has no multiply at all. M is an extension; Zeitlos builds it everywhere so the software can rely on it.")
  (wrong "Because the multiplier makes the clock faster"
    "The clock is 48 MHz either way. The point is that every board runs the same binaries.")
  (hint "What happens when a CPU meets an instruction it does not have?"))
```

---

## Not one instruction a cycle

PicoRV32 is **multi-cycle**: it is not pipelined, so it fetches, decodes
and executes one instruction at a time, taking several clock cycles for
each. Waiting for memory adds more.

Two measures say how it does. **CPI** is clock cycles per instruction;
its inverse is IPC, instructions per cycle. **MIPS** is millions of
instructions per second: the clock in MHz times the IPC.

At start-up the kernel measures IPC with the CPU's own counters and
prints a line such as `cpu: picorv32 12.34 MIPS @ 48 MHz (0.26 IPC)`.

```sources
(source "Zeitlos docs: boot.md, What the boot log tells you; CPU speed check")
```

```check
(number
  (ask "The CPU runs at 48 MHz with an IPC of 0.25. How many MIPS is that?")
  (answer 12) (from "48 * 0.25")
  (wrong-if 192 "That is 48 divided by 0.25. IPC is instructions per cycle, so multiply the cycles by it.")
  (wrong-if 48 "That would be one instruction every cycle. PicoRV32 does a quarter of one per cycle here.")
  (hint "Instructions per second = cycles per second x instructions per cycle."))
```

---

## Where the cycles go

On a board with SDRAM and no cache, PicoRV32 was measured at a CPI of
16.7 (`sergei_ml1`). Only about 4 of those cycles are the CPU's own
work; the rest is waiting for each instruction to come from memory. On
a slow memory, the CPU spends most of its time fetching, not computing.
That is what the caches in the next lesson are for.

```sources
(source "Zeitlos docs: icache.md, Why this exists; Measured on hardware")
```

```check
(number
  (ask "With a CPI of 16.7, about how many instructions does the CPU run per second at 48 MHz, in millions? (Two decimal places.)")
  (answer 2.87) (within 0.06) (from "48 / 16.7")
  (wrong-if 801.6 "That is 48 x 16.7. CPI is cycles per instruction, so divide the cycles by it.")
  (hint "Instructions = cycles / cycles per instruction."))
```

---

## A core of its own

Zeitlos also has **Zeitlos32**, its own experimental RV32IM core
(`rtl/cpu/zeitlos32`): a single state machine with nine states and no
pipeline, about 5 cycles for an arithmetic instruction and 8 for a load
when memory answers at once. It is not the default. One line in
`rtl/boards.vh`, `CPU_ZEITLOS32`, switches a board to it, and nothing
else changes, so the two cores can be compared on the same gateware.

```sources
(source "Zeitlos docs: zeitlos32.md, Status; Design")
```

```check
(choice
  (ask "Which core runs Zeitlos on the boards as they are built today?")
  (right "PicoRV32")
  (wrong "Zeitlos32" "Zeitlos32 runs Zeitlos, but it is experimental and off unless a board block defines CPU_ZEITLOS32.")
  (wrong "A hard RISC-V core inside the FPGA" "The FPGAs of these boards have no hard processor. Both cores are soft, built from the FPGA's logic."))
```

---

## Practice

```check
(number
  (ask "PicoRV32 is measured at 8.33 cycles per instruction. What is its IPC? (Two decimal places.)")
  (answer 0.12) (within 0.005) (from "1 / 8.33")
  (wrong-if 8.33 "That is the CPI. IPC is its inverse: one divided by it.")
  (hint "IPC = 1 / CPI."))
```

```check
(choice
  (ask "Which instructions does RV32IM have that RV32I does not?")
  (right "Multiply and divide")
  (wrong "Floating point" "Floating point would be the F extension. Zeitlos has none: floats are done in software.")
  (wrong "Loads and stores" "Loads and stores are in the base set, I. M adds multiply and divide."))
```

So: the Zeitlos CPU is PicoRV32, RV32IM at 48 MHz on every board, and its speed is the clock times its instructions per cycle.
