# Which board, which connector

```lesson
(objective "Tell from a build's name what each PMOD connector of a board carries, and which port number its GPIO pins have.")
(minutes 15)
(keywords "target (of a build)" "GPIO port (of Zeitlos)" "drive (a pin)" "console (of a computer)")
```

On a Zeitlos computer, what a PMOD connector does is not fixed by the
board. It is chosen when the design that the FPGA loads at power-on is
built: the same connector can carry an ethernet module on one machine
and eight free pins on another.

Each such build is a **target**, named after the board and then what is
in its connectors, in order: the first in connector A, the second in B.
`obst_langkatze_gpio` is an Obst board with a Langkatze ethernet module
in A and free pins in B.

```sources
(source "Zeitlos docs: releases.md, Naming a target; gpio.md, Building it")
```

```check
(choice
  (ask "A machine runs the target schoko_langkatze_gpio. Which connector has the free pins?")
  (right "B")
  (wrong "A" "The first name after the board is in connector A: here, the Langkatze. The free pins come second, so they are in B.")
  (wrong "Both" "Each name after the board takes one connector. Langkatze has A, so the free pins have only B.")
  (hint "After the board's name, the connectors are named in order: A, then B."))
```

---

## Boards with free pins

Eight free pins on one connector are a **GPIO port** (general-purpose
input and output). Software can read each pin, or **drive** it: hold it
high or low. These targets
build one:

| Target | GPIO port on | The other connector |
|---|---|---|
| `lakritz_gpio` | A, the only one | none |
| `minze_gpio` | A, the only one | none |
| `obst_langkatze_gpio` | B | A: Langkatze ethernet |
| `schoko_langkatze_gpio` | B | A: Langkatze ethernet |

Some boards, such as Konfekt, have no PMOD connector at all.

```sources
(source "Zeitlos docs: gpio.md, Building it: Which targets have it; boards.md, Konfekt and Schoko")
```

```check
(choice
  (ask "Which of these machines has its GPIO port in connector A?")
  (right "One running minze_gpio")
  (wrong "One running obst_langkatze_gpio"
    "On that target the Langkatze is named first, so it has connector A. The GPIO port is in B.")
  (wrong "One running schoko_langkatze_gpio"
    "Langkatze comes first in that name, so it is in A, and the GPIO port is in B.")
  (hint "Find the target where gpio is the first thing named after the board."))
```

---

## What a connector gives up

A connector does one job at a time. Lakritz has a single PMOD
connector, and on an ordinary Lakritz its **serial console** (the
text line that the system can always be reached on) runs through
two of those pins. The `lakritz_gpio` target hands all eight pins to
GPIO, so that machine has no serial console and no ethernet: it starts
on its screen with a USB keyboard.

```sources
(source "Zeitlos docs: gpio.md, Building it (Lakritz has one PMOD connector)")
```

```check
(choice
  (ask "Why does a Lakritz running lakritz_gpio have no serial console?")
  (right "Its one PMOD connector now carries the GPIO port, and the console used two of its pins")
  (wrong "GPIO pins cannot work at the same time as a serial console on any board"
    "On Obst, Schoko and Minze the console goes over the USB-C socket, so they have both. Lakritz is short of pins, not of ability.")
  (wrong "The serial console needs ethernet"
    "A serial console needs only a UART's two wires and ground. Lakritz loses it because the connector it used is now GPIO.")
  (hint "Lakritz has only one PMOD connector. What used it before?"))
```

---

## Ports are numbered from 0

Software does not use the letters. GPIO ports are numbered from 0 in
the order the target builds them, so the first GPIO port is **port 0**
whichever connector it is in. On `obst_langkatze_gpio` the free pins
are in connector B, and they are still port 0.

```sources
(source "Zeitlos docs: gpio.md, Ports are numbers, and only numbers")
```

```check
(number
  (ask "A machine runs schoko_langkatze_gpio. What is the number of its GPIO port?")
  (answer 0)
  (wrong-if 1 "Connector B is the second connector, but the GPIO port is the first, and only, GPIO port. Ports count from 0.")
  (wrong-if 2 "Ports are numbered among GPIO ports only, from 0. This target has one.")
  (hint "Count only the GPIO ports, starting from 0."))
```

---

```check
(choice
  (ask "A friend says: \"My board's GPIO pins are in connector B, so in software they are port B.\" What do you tell them?")
  (right "Software has no letters: their pins are port 0, the first GPIO port")
  (wrong "They are right: the port is named after the connector"
    "The letters name connectors on the board. Software numbers GPIO ports from 0, and a lettered name was removed on purpose so the two could not be mixed up.")
  (wrong "They are port 1, because B is the second letter"
    "B is the second connector, but the GPIO port in it is the first GPIO port, so it is port 0.")
  (hint "How does software number GPIO ports?"))
```

So: a target's name says what each connector carries, A first, and a
GPIO port is numbered from 0 whatever connector it is in.
