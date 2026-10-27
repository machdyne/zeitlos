# Sechs modules

```lesson
(objective "Name the six contacts of a Sechs module and the three roles in a Sechs system, and plug a module into a PMOD connector.")
(minutes 20)
(keywords "Sechs (interface)" "carrier (of a module)" "Wolfszahn")
```

Machdyne's programmable modules are small computers: a RISC-V
microcontroller and long-lived memory on a tiny board, running Machdyne
BASIC. The first, the LS10, has 16 KB of flash, 2 KB of RAM and 8 KB of
F-RAM for its programs. Their first six contacts follow a standard
called **Sechs**, German for six:

```figure
(define p (slider 'pin 1 6 1 "Contact" 1))
(hw-pmod 40 70 40 1 '("A" "B" "C" "D" "GND" "3V3") p)
(text 160 150
  (hw-nth '("A: the shared I2C bus clock" "B: the shared I2C bus data"
            "C: the module's own" "D: the module's own"
            "GND: ground" "3V3: 3.3 V, from the carrier") (- p 1))
  'centre 'body 'bold)
(caption "The six Sechs contacts, on one row of a PMOD connector")
```

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Modules; The Sechs Interface; Connector Pinouts: Sechs")
```

```check
(choice
  (ask "Which two Sechs contacts form the I2C bus that all the modules share?")
  (right "A and B")
  (wrong "C and D" "C and D belong to each module alone: its sensor, its output, a local bus.")
  (wrong "GND and 3V3" "Those are power. The shared bus is A, the clock, and B, the data.")
  (hint "The bus is on the first two contacts."))
```

---

## Three roles

A Sechs system has:

- **modules**, which run programs;
- a **carrier**, the board a module plugs into, which supplies its
  power, the pull-ups for A and B, and decides what C and D are used
  for;
- a **master**, usually a computer, which controls the modules over
  the shared bus.

Modules never have pull-ups of their own on A and B: the carrier does.

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: The Sechs Interface; Connector Pinouts: Sechs")
```

```check
(choice
  (ask "Three modules share a bus. Where are the pull-ups on A and B?")
  (right "On the carrier, once for the bus")
  (wrong "On each module" "The standard says modules never have them. The carrier provides the pull-ups once, for the whole bus.")
  (wrong "Nowhere: Sechs needs none" "A and B are I2C, which is open drain and needs pull-ups. The carrier provides them.")
  (hint "Which role supplies the power and decides about the bus?"))
```

---

## A module on a Zeitlos computer

A **Wolfszahn** is a carrier that puts a module on a PMOD connector, in
the top row: A on PMOD pin 1, B on pin 2, C on 3, D on 4, ground on 5
and 3V3 on 6. A and B are then GPIO bits 0 and 1, the same pins the
`i2c` command uses. The board's own weak pull-ups make a short bus
work; for long wires or many modules, add 2.2k to 4.7k pull-ups on pins
1 and 2.

```sources
(source "Zeitlos docs: sechs_app.md, Wiring")
```

```warn
(warn "Power off, right row" "Plug a module in with the board's power unplugged, into the row with pin 1, so that its GND and 3V3 meet the connector's. A module the wrong way round puts 3.3 V where ground should be.")
```

```check
(number
  (ask "A module in a Wolfszahn is on port 0. Which GPIO bit is its contact D?")
  (answer 3)
  (wrong-if 4 "D is on PMOD pin 4, and pin 4 is bit 3: bits count from 0.")
  (wrong-if 1 "Bit 1 is pin 2, contact B. D is on pin 4.")
  (hint "D is on PMOD pin 4.")
  (hint "Pin 4 is bit 3."))
```

---

## Built to be recovered

Sechs makes promises that keep a system safe: at power-on a module
keeps its pins inactive; for a short **boot window** after power-on the
master can always halt it, whatever program it holds; and a new program
replaces the old one only when it has all arrived and been checked.

```check
(choice
  (ask "A module's program has gone wrong and keeps it busy. How can the master still stop it?")
  (right "Power it up again and halt it in the boot window, which no program can remove")
  (wrong "Only by writing new firmware with a programmer" "Sechs guarantees a boot window in which the master can always halt the module.")
  (wrong "It cannot: the module must be replaced" "The boot window exists so that a module can always be recovered.")
  (hint "What can the master always do just after power-on?"))
```

So: Sechs is six contacts (a shared I2C bus on A and B, C and D for the
module, ground and 3V3), a carrier gives power and pull-ups, and a
master controls the modules.
