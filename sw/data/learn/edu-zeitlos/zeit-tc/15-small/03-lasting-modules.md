# Programs and parts that outlive their chips

```lesson
(objective "Compare two ways of making programs outlive their hardware, and tell a memory's data retention from a device's functional life.")
(minutes 15)
(keywords "portability" "data retention" "functional life")
```

You will see how a program can outlast the chip it was written for.

**Portability** is how easily a program moves to other hardware. There
are two general ways to get it:

- a **common instruction set**: every device runs the same machine
  code, directly or through a small interpreter (a virtual machine);
- a **common language**: every device accepts the same source programs,
  and runs them in whatever way suits its chip.

Machdyne's modules have used both. The Zwölf platform, from which
Sechs grew, defined a small 8-bit processor that every module was to
copy, whatever chip it was built from. Sechs keeps the six pins but
defines no common instruction set; its portability comes from Machdyne
BASIC, which can be interpreted on some chips and could be compiled on
others.

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Portability: Machines and Languages")
```

```check
(choice
  (ask "Under Sechs, a BASIC program written for one module is moved to a module with a different chip. Why can it still run?")
  (right "Both accept the same language, BASIC, and run it in their own way")
  (wrong "Both chips run the same machine code"
    "That was Zwölf's way. Sechs defines no common instruction set; the common layer is the BASIC language.")
  (wrong "Sechs translates the program into the new chip's machine code as it moves"
    "Sechs is an interface of six pins and a bus, not a translator. Each module's firmware runs BASIC its own way.")
  (hint "What is common to every Sechs module's firmware?"))
```

---

## A stable layer

A common instruction set makes compiled programs portable, and many
systems have used one, from early teaching computers to modern languages
that compile to a virtual machine's code. A common language is simpler to build, and leaves each
device free to run programs its own way.

Either way, what matters is a stable, documented layer between the
programs and the hardware, so that either can change without the other.

```check
(choice
  (ask "What do both ways of portability share?")
  (right "A stable, documented layer between programs and hardware")
  (wrong "Every device using the same chip"
    "Then nothing would need to be portable. Both ways let programs run on different chips, through a layer that stays the same.")
  (wrong "Programs written in machine code"
    "With a common language, programs are source text, not machine code. What both share is the stable layer.")
  (hint "What stays the same while the chips change?"))
```

---

## How long a module lives

Two measures describe a small device's life. **Data retention** is how
long its memory keeps what was written to it: for F-RAM it is rated at
a century or more, depending on temperature. The **functional life** of
the whole device depends on its other parts, the microcontroller, the
solder joints, the connectors and the capacitors, and on how it is
used. It is generally shorter.

So the important information is kept in the most durable form, the
parts are cheap and replaceable, and the designs and documents are
open, so that a replacement can always be made.

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Module Lifetime")
```

```check
(choice
  (ask "A module's F-RAM is rated to keep its data for a century. Will the module work for a century?")
  (right "Not necessarily: its other parts may fail sooner")
  (wrong "Yes: the F-RAM sets the module's life"
    "That is the data retention of one part. The functional life depends on the chip, the solder, the connectors and the capacitors too, and is generally shorter.")
  (wrong "No: F-RAM's rating means the module stops at a century"
    "The rating is how long the data is kept, not when the module stops. The other parts usually wear out first.")
  (hint "Data retention and functional life are two different measures."))
```

---

```check
(choice
  (ask "A module's parts may fail long before its F-RAM forgets. What makes the system last anyway?")
  (right "Its programs are kept as text, and a cheap spare module can be fitted and loaded with them")
  (wrong "Nothing: the system lasts only as long as its first modules"
    "Parts that are cheap and replaceable, and programs kept in a durable, readable form, let the system outlive any one module.")
  (wrong "Keeping the module switched off"
    "A module switched off does no work. The system lasts by replacing parts, not by not using them.")
  (hint "What can be replaced, and what is kept?"))
```

So: programs outlive their chips through a stable layer such as a
common language, and systems outlive their parts by keeping the
information durable and the parts replaceable.
