# Modules and Sechs

```lesson
(objective "Say what each Sechs pin does and what the three Sechs roles are, and plan a system as modules on one bus.")
(minutes 20)
(keywords "Sechs")
```

You will see how small computers are joined to make a larger system.

A **module**, here, is a microcontroller on a small circuit board, with
enough around it to work on its own and a standard set of contacts for
joining it to a larger system.

Machdyne's modules are made to last. Each pairs a RISC-V
microcontroller with long-lived memory, F-RAM or EEPROM, that holds
its programs and data as files. The first, the LS10, has 16 KB of flash
and 2 KB of RAM in its microcontroller, and 8 KB of F-RAM. The modules
fit a 12-pin footprint (a pattern of pads on a board), and also plug
into a six-pin Pmod connector, a common small expansion socket. Their
first six pins follow a standard called **Sechs**, German for "six".

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Modules")
```

```check
(choice
  (ask "Where does a Machdyne module keep its BASIC programs?")
  (right "As files in its long-lived memory, F-RAM or EEPROM")
  (wrong "Only in RAM, so they are lost at power-off"
    "The modules keep programs as files in long-lived memory, so they stay when the power is off.")
  (wrong "On the host computer, which sends them at every start"
    "Each module keeps its own programs, so it can run with no host at all.")
  (hint "The modules are made to last: what memory do they pair with?"))
```

---

## The six pins

| Pin | Name | Use |
|---|---|---|
| 1 | A | shared I2C bus: clock |
| 2 | B | shared I2C bus: data |
| 3 | C | the module's own I/O |
| 4 | D | the module's own I/O |
| 5 | GND | ground |
| 6 | 3V3 | 3.3 V power |

Pins A and B are an I2C bus, shared by every module joined to it. Pins
C and D belong to each module alone: for its sensor, its output, a
local I2C device or a serial console.

```check
(choice
  (ask "A soil-moisture probe belongs to one module only. Which pins does it go on?")
  (right "C and D")
  (wrong "A and B"
    "A and B are the bus shared by all the modules. A device that belongs to one module goes on that module's own pins, C and D.")
  (wrong "GND and 3V3"
    "Those two carry power, not signals. The probe's signal goes on C or D.")
  (hint "Which pins does each module have to itself?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: The Sechs Interface")
```

---

## Three roles

Sechs names three roles:

- a **module**, which runs programs;
- a **carrier**, the board a module plugs into, which supplies power
  and sets what pins C and D are used for;
- a **master**, usually a host computer, which controls modules over
  the shared bus.

Over the bus, the host can find every module by scanning addresses and
ask each one what it is. It can stop and start a module's program, read
and write its sixteen program registers, send it a new program, and
open a console to type commands into it. A new module answers at a
default address, and the host can give each its own, so that several
share one bus.

Sechs is still a draft standard: its registers may change, but its
pinout and its safety rules are settled.

```check
(choice
  (ask "Three new modules are joined to one bus. Why must the host give them addresses?")
  (right "They all start at the same default address, and each needs its own to share the bus")
  (wrong "Modules have no address until the host gives them one"
    "A new module answers at a default address. The trouble is that all three would answer at that same one.")
  (wrong "To decide which pins C and D they use"
    "Pins C and D are set by the carrier. Addresses are for telling modules apart on the shared bus.")
  (hint "What address does a new module answer at?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: The Sechs Interface")
(source "Zeitlos documentation: sechs (docs/sechs_app.md)")
```

---

## A system of modules

```text
                 +----------------------+
                 |    host computer     |
                 | (logs, shows, sets)  |
                 +----------+-----------+
                            |  two-wire bus (A, B)
       +--------------------+--------------------+
       |                    |                    |
  +----+-----+         +----+-----+         +----+-----+
  |  heater  |         |   soil   |         |  water   |
  |  module  |         |  module  |         |  pump    |
  +----+-----+         +----+-----+         +----+-----+
       | C, D               | C, D               | C, D
  sensor, relay        moisture probe        pump relay
```

In this greenhouse each module runs its own program, which it starts by
itself. (A relay is a switch worked by a small current.) The host reads each module's registers to log and show its
state, and writes registers to change settings.

```check
(choice
  (ask "The host computer in the greenhouse breaks down. What happens to the plants' care?")
  (right "The modules go on heating and watering; only the logs and display stop")
  (wrong "Everything stops until the host is repaired"
    "Each module runs its own program on its own chip. The host only reads and changes their settings.")
  (wrong "The modules stop when they lose the bus"
    "A module does not need the bus to run its program. It keeps working with no host at all.")
  (hint "Where does each module's program run?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Building Systems from Modules")
```

---

## Why it lasts

Each part is simple enough to be understood completely, and each can be
replaced: a failed module is swapped for a spare and programmed in
minutes, while the rest runs on. A failure takes out one part, not the
whole system. The programs are kept as text in long-lived memory, so
whoever looks after the system later can read and change them.

```check
(choice
  (ask "The pump module fails. What is the quickest repair?")
  (right "Fit a spare module and send it the pump's BASIC program")
  (wrong "Replace the host computer"
    "The host still works; only the pump module failed. A spare module and the same program fix it.")
  (wrong "Rebuild the whole system around a bigger computer"
    "That throws away what works. Only one part failed, and it can be replaced alone.")
  (hint "Only one part failed."))
```

So: Sechs gives every module the same six pins, a shared bus and its
own two, and a system of modules keeps working when any one part fails.
