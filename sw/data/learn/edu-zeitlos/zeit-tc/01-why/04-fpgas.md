# FPGAs and Zeitlos

```lesson
(objective "Say what an FPGA is, why it suits timeless computing, and what Zeitlos is.")
(minutes 20)
(keywords "chip (electronic)" "pin (of a chip)" "processor (of a computer)" "memory (of a computer)" "flash memory (storage)" "field-programmable gate array (chip)" "FPGA (chip)" "configuration (of an FPGA)" "megahertz (speed)" "system on a chip (SOC)" "SOC (system on a chip)" "Zeitlos (the system)" "survival computing (idea)")
```

You will learn what kind of hardware the computer in front of you is,
and why it was chosen.

Electronic parts are made as **chips**: small, flat packages, each
holding a complete working part, with metal legs, its **pins**, that
connect it to a board.

The **processor** is the chip that carries out a program's
instructions, one after another. In an ordinary computer the processor
is fixed when the chip is made: its design can never change.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Why FPGAs")
```

```check
(choice
  (ask "In an ordinary computer, can the owner change how the processor itself works?")
  (right "No: its design is fixed when the chip is made")
  (wrong "Yes, by installing new software"
    "New software gives the processor new instructions to follow, but the processor itself, the way it follows them, was fixed in the factory.")
  (hint "When is an ordinary processor's design decided?"))
```

---

## The FPGA

A **field-programmable gate array**, or **FPGA**, is a different kind
of chip. It is a large array of identical small building blocks. A
file, its **configuration**, sets what each block does and how the
blocks are connected. "Field-programmable" means this can be set after
the chip has left the factory.

The configuration is kept in **flash memory**. A computer's
**memory** is where it keeps information; flash memory keeps it even
with the electricity off. The FPGA loads its configuration each time it
is switched on.

The same chip can be set up as a complete computer, with its own
processor, graphics and keyboard input, or as something else entirely.

```check
(choice
  (ask "An FPGA computer is switched off for a year. When it is switched on, where does the FPGA get its configuration from?")
  (right "From flash memory, which kept it while the computer was off")
  (wrong "The FPGA itself remembered it"
    "The FPGA loads its configuration every time it is switched on. It is the flash memory that keeps the file while the computer is off.")
  (wrong "From the factory, over the Internet"
    "Nothing has to come from outside. The configuration is a file kept in the computer's own flash memory.")
  (hint "Which kind of memory keeps what it holds with the electricity off?"))
```

---

## Why it suits timeless computing

Because the computer is a design loaded into the chip, the whole
design, processor included, can be written down as text that anyone can
read; the configuration file is made from that text. No part of the
computer's own design is hidden, although the FPGA that carries it is
still a manufactured chip.

And the design can be corrected or extended after the hardware is
built, and moved to a different FPGA when the first one is no longer
made.

```check
(choice
  (ask "The FPGA a computer was built with is no longer made. What can be done?")
  (right "Move the computer's design to a different FPGA")
  (wrong "Nothing: the computer's design is lost with the chip"
    "The design is a readable file, not part of the chip. It can be moved to another FPGA.")
  (wrong "Use an ordinary processor chip instead, with the same design"
    "An ordinary processor's design is fixed in its factory and cannot be loaded with another design. Another FPGA can.")
  (hint "Where does an FPGA computer's design live: in the chip, or in a file?"))
```

---

## Speed and electricity

The price is speed. An FPGA computer runs at tens of **megahertz**: a
megahertz is a million steps a second. A modern processor runs at
thousands of megahertz. An FPGA computer is about as fast as a desktop
computer of the early 1990s. Those were used for real work, and their
small software often made them respond faster than computers today.

An FPGA computer also uses little electricity, which helps when it
comes from a battery or a solar panel.

```check
(number
  (ask "A processor runs at 50 megahertz. How many steps does it take each second?")
  (answer 50000000) (from "50 * 1000000")
  (wrong-if 50 "That is the number of megahertz. Each megahertz is a million steps a second.")
  (wrong-if 50000 "That is 50 thousand. A megahertz is a million steps a second, not a thousand.")
  (hint "Mega means a million.")
  (hint "50 times 1,000,000."))
```

---

## Zeitlos

Machdyne, a maker in Germany, designs and builds open hardware for
timeless computing: computers built around FPGAs, storage meant to keep
information for a century, and small modules.

**Zeitlos**, the system you are using now, has two halves designed
together. One is a **system on a chip** (**SOC**): a complete computer
on one chip, here a design configured into the FPGA. The other is an
operating system. Zeitlos also runs on common FPGA boards made for
students. [The book](docs:tc) this course follows comes with it.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Putting the Ideas Into Practice")
```

```check
(choice
  (ask "On a Zeitlos computer, where is the processor?")
  (right "It is part of the design configured into the FPGA")
  (wrong "A separate, fixed processor chip next to the FPGA"
    "Zeitlos is a system on a chip: the whole computer, processor included, is a design loaded into the FPGA.")
  (wrong "Zeitlos has no processor"
    "Every computer that runs programs has a processor. In Zeitlos it is part of the design in the FPGA.")
  (hint "SOC: a complete computer on one chip."))
```

---

## Survival computing

**Survival computing** is timeless computing taken furthest. It asks
what a computer needs to stay useful without cheap electricity, new
parts or the Internet: in a disaster, a war, or a remote place.

The answers are a machine that is simple, uses little electricity, can
be repaired, and comes with its documentation. These help in ordinary
times too.

```sources
(source "Timeless Computing (docs/tc.md), chapter 1, Survival Computing")
```

```check
(choice
  (ask "A village far from any town has a small solar panel and no Internet. Which computer suits it best?")
  (right "A simple FPGA computer that uses little electricity, with its documentation")
  (wrong "A fast desktop computer that downloads its updates"
    "There is no Internet for updates, and a desktop computer needs far more electricity than a small solar panel gives.")
  (wrong "Any computer, as long as it is new"
    "New does not help when it breaks and no parts can arrive. Simple, repairable, thrifty with electricity and documented is what lasts there.")
  (hint "Think of what the village lacks: lots of electricity, new parts, the Internet."))
```

---

```check
(choice
  (ask "What is the main disadvantage of an FPGA computer?")
  (right "It is much slower than a modern processor")
  (wrong "Its design is hidden inside the chip"
    "The opposite: its design is a file anyone can read.")
  (wrong "It needs a lot of electricity"
    "An FPGA computer uses little electricity, which is one of its advantages.")
  (hint "Tens of megahertz against thousands."))
```

So an FPGA is a chip whose design is a readable file, slower than a
modern processor but open and lasting, and Zeitlos is a computer and
operating system built on one.
