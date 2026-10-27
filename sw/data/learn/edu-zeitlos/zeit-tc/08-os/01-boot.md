# What an operating system does

```lesson
(objective "Say when a computer needs an operating system, what it does, and how a computer starts it.")
(minutes 20)
(keywords "operating system" "bare metal" "abstraction" "firmware" "bootloader" "kernel")
```

A processor follows one program at a time. Yet a desktop computer seems
to run many programs at once, shares one screen and one disk among
them, and keeps a faulty program from stopping the others. The
software that does this is the **operating system** (OS).

Not every computer has one. A small module that reads a sensor and
switches a pump runs a single program, written for its hardware, from
the moment it is powered on. This is running on **bare metal**. For one
task on one small device it is often the better choice: there is less
to go wrong and less to understand.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: When an Operating System Is Needed")
```

```check
(choice
  (ask "A small board's only job is to turn on a fan when a room gets hot. What should it run?")
  (right "One program on bare metal")
  (wrong "A full operating system, to be safe"
    "An operating system earns its place when many programs share a machine. For one small task it is only more to go wrong.")
  (hint "How many programs does this board need to run?"))
```

---

## Four jobs

An operating system is needed when a computer must run several
programs, share devices among them, keep files, and be used by a
person for many tasks. It has four broad jobs:

- **sharing** the processor, memory and devices among programs;
- **abstraction**: hiding the details of the hardware behind something
  simpler, so a program can write a file without knowing how the SD
  card works;
- **protection** of programs from each other, and of the system from
  them;
- **services** that most programs need: files, windows, networking and
  time.

```check
(choice
  (ask "A program saves a file the same way whether it goes to an SD card or a USB stick. Which job of the operating system is that?")
  (right "Abstraction")
  (wrong "Protection"
    "Protection keeps programs from harming each other. Here the program is spared the details of each device: abstraction.")
  (wrong "Sharing"
    "Sharing divides the processor and devices among programs. Hiding how each device works is abstraction.")
  (hint "Which job hides how the hardware works?"))
```

---

## Booting

When the power comes on, the processor starts fetching instructions
from a fixed address. The program there must already be present, so it
is kept in memory that keeps its contents without power. This first
program is part of the computer's **firmware**, the software built into
the hardware.

The firmware sets up the hardware and starts a **bootloader**: a
program whose job is to load another program. It finds the operating
system, copies it into memory and jumps to it. The core of the
operating system, the **kernel**, then starts the parts that run the
devices, and finally the first program you see: a place to type
commands, a desktop or an application.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Booting")
```

```check
(choice
  (ask "Why must the very first program a processor runs be kept in memory that keeps its contents without power?")
  (right "Nothing has run yet that could load it from anywhere else")
  (wrong "It is too large for RAM"
    "It is usually small. The point is that, at power-on, RAM is empty and no program has yet run to fill it.")
  (wrong "So that programs cannot change it while it runs"
    "Protection is not the reason. At power-on nothing has run yet, so the first program must already be there.")
  (hint "What does RAM hold just after the power comes on?"))
```

---

## An FPGA computer, and a way back

An FPGA computer, whose processor is itself a design loaded into a
programmable chip, has one step more at the very start. Before there is
any processor, the FPGA loads its design, the one that makes it a
computer, from a flash memory chip. Only then does that processor
begin to fetch instructions.

Booting is also the usual way to recover. A well-made system lets you
interrupt it, often by holding a key, and start something simpler
instead: a minimal console, a known good copy of the system, or a way
to load a new one. Every owner should know how to reach it.

```check
(choice
  (ask "On an FPGA computer, what happens first when the power comes on?")
  (right "The FPGA loads its design from flash")
  (wrong "The processor fetches its first instruction"
    "On an FPGA the processor is part of the design. Until the design is loaded from flash, there is no processor to fetch anything.")
  (wrong "The bootloader loads the kernel"
    "That comes later, once there is a processor and its firmware has run.")
  (hint "Where does an FPGA computer's processor come from?"))
```

---

```check
(choice
  (ask "Put the usual order right. Which comes last?")
  (right "The first program you see, such as a desktop")
  (wrong "The kernel"
    "The kernel starts the parts that run the devices, and then the first program you see. It comes before that program.")
  (wrong "The bootloader"
    "The bootloader loads the kernel, so it comes before it.")
  (wrong "The firmware"
    "The firmware runs first of all, from the fixed address at power-on.")
  (hint "Firmware, bootloader, kernel, then ...?"))
```

So: an operating system shares, hides, protects and serves; it is
started by firmware and a bootloader, and its core is the kernel.
