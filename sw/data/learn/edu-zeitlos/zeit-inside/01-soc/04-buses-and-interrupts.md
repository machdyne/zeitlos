# Buses and interrupts

```lesson
(objective "Say which parts of the Zeitlos SOC share each bus, and which interrupt line each device raises.")
(minutes 20)
(keywords "main bus" "VRAM bus" "round robin")
```

You will learn how the parts of the SOC take turns, and how they get the
CPU's attention.

The CPU is not the only bus master in the SOC. Two buses carry most of
the traffic:

- the **main bus**, to main memory and the devices, used by the CPU, by
  the blitter when it copies a picture out of main memory, and, on
  boards with audio, by the audio mixer, which fetches its sound
  samples itself;
- the **VRAM bus**, to the screen's pixels, used by the CPU, the line
  rasterizer and the blitter.

Each bus has its own arbiter (`rtl/arbiter_main.v`,
`rtl/arbiter_vram.v`), which decides who goes next when two masters ask
at once.

```sources
(source "Zeitlos source: rtl/arbiter_main.v and rtl/arbiter_vram.v, their headers")
```

```check
(choice
  (ask "The blitter copies a picture from main memory onto the screen. Which buses does it use?")
  (right "Both: it reads on the main bus and writes on the VRAM bus")
  (wrong "Only the VRAM bus"
    "The pixels it writes are in VRAM, but the picture it reads is in main memory, and main memory is on the main bus.")
  (wrong "Neither: the blitter has its own memory"
    "The blitter reads and writes the same memories the CPU does, through the two arbiters.")
  (hint "Where is the source, and where is the destination?"))
```

---

## Taking turns

The main bus's arbiter works by **round robin**: whichever master had the
bus last goes to the back of the queue. Fixed priorities were rejected,
because either way round one master could shut out another: a long
blit could starve the CPU, or a CPU in a tight loop could keep a blit
from ever finishing. The audio mixer takes its turn like the others; it
needs eight single-word reads per sample, under 1% of the bus.

The VRAM bus's arbiter is simpler, with fixed priorities: the blitter
first, then the line rasterizer, then the CPU.

```sources
(source "Zeitlos source: rtl/arbiter_main.v, round robin, not fixed priority; why the audio mixer is in the rotation")
```

```check
(choice
  (ask "The CPU and the blitter both want the main bus, and the CPU had it last. Who goes next?")
  (right "The blitter")
  (wrong "The CPU, because it always comes first"
    "There is no fixed first. In round robin the master that went last waits its turn.")
  (wrong "Whichever asked first, however long ago the CPU had it"
    "The arbiter rotates. The one that had the bus last drops to the back.")
  (hint "Round robin: the last one served goes to the back."))
```

---

## Interrupt lines

Devices get the CPU's attention through numbered interrupt lines. These
are wired in `rtl/sysctl.v` and named in the kernel (`sw/os/kernel.h`):

| Line | Raised by |
|---|---|
| 0 | the CPU's own timer, used when a process gives up its turn |
| 1, 2 | the CPU itself: an illegal instruction, a misaligned access |
| 3 | the kernel's timer, about 732 times a second |
| 4 | the serial port UART0 |
| 5, 6 | the two USB keyboard and mouse ports |
| 7 | the audio output, when it needs more samples |
| 8 | Ethernet, when a frame arrives |
| 9 | the USB host controller |
| 10 | the MPU, when it stops a forbidden access |

A line exists only where the board has the device.

```sources
(source "Zeitlos source: rtl/sysctl.v, the cpu_irq assignments; sw/os/kernel.h, Z_IRQ_*")
(source "Zeitlos docs: mpu.md, Crash reporting (IRQ 1 and 2)")
```

```check
(number
  (ask "A key is typed into a terminal connected to the serial port. Which interrupt line does the CPU see?")
  (answer 4)
  (wrong-if 5 "Line 5 is a USB keyboard port. A key arriving over the serial port raises the UART's line.")
  (wrong-if 3 "Line 3 is the kernel's timer, which comes whether or not anything happens.")
  (hint "Look for UART0 in the table."))
```

---

```check
(choice
  (ask "Why does the audio output raise an interrupt?")
  (right "To ask for more samples before its buffer runs dry")
  (wrong "To tell the CPU that a sound has finished playing"
    "The line is raised by the output's buffer when it is running low, so the CPU can fill it before the sound breaks up.")
  (wrong "To keep time for the scheduler"
    "The scheduler's beat is the timer on line 3."))
```

```check
(choice
  (ask "An app runs an instruction the CPU does not know. What tells the kernel?")
  (right "Interrupt line 1, raised by the CPU itself")
  (wrong "Interrupt line 10, the MPU"
    "The MPU stops forbidden memory accesses. An unknown instruction is the CPU's own complaint, line 1.")
  (wrong "Nothing: the CPU skips it"
    "The CPU raises an interrupt, and the kernel ends the app with a report."))
```

So: the CPU shares the main bus with the blitter and the audio mixer by round robin, and the VRAM bus with the GPU, which goes first; devices call it through numbered interrupt lines.
