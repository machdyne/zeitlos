# What the four pins carry

```lesson
(objective "Read which signal a module puts on each of pins 1 to 4, from the Pmod types and the module's own documentation.")
(minutes 15)
(keywords "Pmod type (of a module)" "MMOD (module)")
```

The Pmod standard gives the four signal pins of a row standard uses,
so that a module and a board can agree without more documentation. Each
arrangement is a **Pmod type**:

| Type | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| 1, GPIO | GPIO | GPIO | GPIO | GPIO |
| 2, SPI | CS | MOSI | MISO | SCK |
| 3, UART | CTS | TXD | RXD | RTS |
| 6, I2C | INT | RESET | SCL | SDA |

```sources
(source "Timeless Computing (docs/tc.md), Connector Pinouts: Pmod")
```

```check
(number
  (ask "On a type 2 (SPI) module, which pin carries the clock, SCK?")
  (answer 4)
  (wrong-if 1 "Pin 1 is CS, the chip select. The clock is the last of the four.")
  (wrong-if 2 "Pin 2 is MOSI, data from the master. Look along the SPI row to SCK.")
  (hint "Read the row for type 2 across to SCK."))
```

---

## A module that follows its type

Machdyne's **MMOD** memory modules are SPI memory chips on a small
board for a PMOD connector, and they follow type 2: chip select on
pin 1, MOSI on 2, MISO on 3 and the clock on 4. They use only the top
row, so a 6-pin or a 12-pin connector takes one the same way.

```sources
(source "Zeitlos docs: mmod.md, What MMOD is")
```

```check
(choice
  (ask "An MMOD is plugged into a 12-pin connector. On which pins is it?")
  (right "Pins 1 to 6, the top row")
  (wrong "Pins 7 to 12" "An MMOD's pins 1 to 4 are its SPI signals and must meet the connector's pins 1 to 4, in the row with pin 1.")
  (wrong "All twelve" "An MMOD has six pins: four signals, ground and 3V3. It fills one row.")
  (hint "An MMOD uses only the first four signal pins, plus ground and 3V3."))
```

---

## A module that does not

Many modules do not follow the types exactly, so **the module's own
documentation decides**. Zeitlos's I2C tools, which you meet later in
this course, put SCL on pin 1 and SDA on pin 2, the way Machdyne's own
module boards wire them. A type 6 module has them on pins 3 and 4
instead.

```sources
(source "Zeitlos docs: i2c_app.md (pin 1 SCL, pin 2 SDA); tc.md, Connector Pinouts: Pmod")
```

```check
(choice
  (ask "You have a type 6 I2C module (SCL on pin 3, SDA on pin 4) and a tool that expects SCL on pin 1 and SDA on pin 2. What do you do?")
  (right "With the power off, wire the module to the board with jumper wires: its SCL to pin 1, SDA to pin 2, GND to GND and 3V3 to 3V3")
  (wrong "Plug it straight in: I2C finds the lines itself"
    "Nothing swaps the wires for you. Plugged in straight, the tool would drive pins 1 and 2, where this module has INT and RESET.")
  (wrong "Plug it into the second row instead"
    "Pins 7 and 8 are under pins 1 and 2 and would still not meet SCL and SDA. Wire each line to where the tool expects it.")
  (hint "The signals must meet: SCL to the tool's SCL pin, SDA to its SDA pin, and the power pins to power."))
```

---

## Practice

```check
(choice
  (ask "A USB-to-serial module follows type 3. Which pin is its TXD, the data it sends?")
  (right "Pin 2")
  (wrong "Pin 1" "Pin 1 of type 3 is CTS, a flow-control signal. TXD is next to it.")
  (wrong "Pin 3" "Pin 3 is RXD, what the module receives.")
  (hint "Type 3 reads CTS, TXD, RXD, RTS."))
```

---

```check
(choice
  (ask "A new module's pages say nothing about a Pmod type, but they give a pin table. Which do you trust?")
  (right "The module's pin table")
  (wrong "The Pmod type that matches its kind of bus"
    "Many modules do not follow the types exactly. Its own table says where each signal really is.")
  (wrong "Neither: try each way round until it works"
    "Trying wirings at random can put an output against an output. Read the table, wire it with the power off.")
  (hint "Which document describes this module, and no other?"))
```

So: the Pmod types say what pins 1 to 4 usually carry, and a module's
own documentation says what it really carries.
