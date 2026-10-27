# The clock and the tick

```lesson
(objective "Work out the Zeitlos tick rate from the 48 MHz clock, and say what one tick is worth in cycles and instructions.")
(minutes 15)
(keywords "tick (of the kernel)")
```

You will learn where the kernel's beat comes from.

The SOC has a 16-bit counter, `rtc_ctr` in `rtl/sysctl.v`, that adds one
on every cycle of the 48 MHz clock. Each time it wraps round to 0 it
raises interrupt line 3. That interrupt is the kernel's **tick**: the
beat the scheduler uses to share the CPU, and the unit its timeouts are
counted in.

A 16-bit counter wraps every $2^16$ = 65,536 cycles.

```sources
(source "Zeitlos source: rtl/sysctl.v, rtc_ctr and irq_timer")
(source "Zeitlos docs: kernel.md, The tick rate")
```

```check
(number
  (ask "How many ticks are there in a second? (Round to a whole number.)")
  (answer 732) (within 0.5) (from "48000000 / 65536")
  (wrong-if 65536 "65,536 is cycles per tick. Divide the cycles in a second by it.")
  (wrong-if 48 "48 is the clock in MHz. The tick is the clock divided by 65,536.")
  (hint "48,000,000 cycles a second, one tick every 65,536 of them."))
```

---

## A tick, in time and in work

At about 732 ticks a second, one tick lasts about 1.37 ms. On this CPU
that is only some 8,000 instructions: a tick is 65,536 cycles, and the
CPU does roughly one instruction for every 8 cycles.

The tick itself is not free. In October 2026 the kernel's tick handler
was measured at about 8,300 cycles a tick while the shell waited for
input, and saving and restoring registers adds more: some 10 to 17% of
the CPU, depending on what it interrupted, spent keeping time.

```sources
(source "Zeitlos docs: kernel.md, What a tick costs; The tick rate (measured after round 5c)")
```

```check
(number
  (ask "How many milliseconds is one tick? (Two decimal places.)")
  (answer 1.37) (within 0.01) (from "1000 / (48000000 / 65536)")
  (wrong-if 732 "732 is ticks per second. One tick lasts a second divided by 732.")
  (hint "1,000 ms divided by about 732.4."))
```

---

## A clock it cannot check

The CPU also counts its own cycles (`rdcycle`) and instructions
(`rdinstret`). But the tick counter, the cycle counter and the serial
port all run from the same clock. If that clock were wrong, everything
would run fast or slow together, and nothing on the chip could tell.

So the 48 MHz in the boot log is **stated, not measured**: it is
`Z_SYSCLK_HZ` in `sw/common/zsoc.h`. The one figure that depends on no
assumption is IPC, the ratio of two counters.

```sources
(source "Zeitlos docs: boot.md, CPU speed check")
```

```check
(choice
  (ask "The kernel counts the cycles between two ticks. What does it always find?")
  (right "Exactly 65,536, whatever the clock really is")
  (wrong "About 65,536, a little more or less as the clock drifts"
    "The tick counter counts the same clock cycles. It wraps after exactly 65,536 of them, so the count can never differ.")
  (wrong "48,000,000"
    "That is cycles per second, not per tick, and the chip has no independent second to count against.")
  (hint "What clock does the tick counter count?"))
```

---

```check
(number
  (ask "A timeout of 100 ticks: about how many milliseconds is that? (Whole number.)")
  (answer 137) (within 1) (from "100 * 1000 * 65536 / 48000000")
  (wrong-if 100 "Ticks are not milliseconds: each one is about 1.37 ms.")
  (hint "100 x 1.37 ms."))
```

```check
(choice
  (ask "Why is the tick rate about 732 Hz, not a round number like 1,000?")
  (right "It is the 48 MHz clock divided by the 65,536 counts of a 16-bit counter")
  (wrong "It was measured to suit the screen"
    "The screen refreshes at 60 Hz. The tick comes from the counter's size, not from the display.")
  (wrong "It changes from board to board"
    "The clock is 48 MHz on every board, so the tick is the same everywhere."))
```

So: the tick is the 48 MHz clock divided by 65,536, about 732 a second, each one some 1.37 ms and 8,000 instructions.
