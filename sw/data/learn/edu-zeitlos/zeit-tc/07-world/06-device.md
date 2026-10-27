# Connecting a device

```lesson
(objective "Add a device to a computer the safe way: read its data sheet, work out its bus address, wire it with the power off, check that it answers, and read it.")
(minutes 25)
(keywords "protocol" "Sechs" "boot window" "Pmod" "two's complement")
```

A bus such as I2C only moves bytes. What the bytes mean is set by a
**protocol** on top of it: an agreed set of rules for who sends what,
when, and what each byte means.

The **Sechs** interface of Machdyne's programmable modules is a protocol
built to forgive mistakes. It has six contacts: an I2C bus on two, two
pins for the module's own use, ground and 3.3 V. A host on the bus can
find every module, ask what it is, start and stop its program, and load
a new program into it. It also promises:

- at power-on, a module keeps all its pins inactive;
- for a short **boot window** after power-on, the host can always halt
  a module, whatever program it holds, and no program can remove this;
- a new program replaces the old one only after it has arrived whole
  and been checked.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Protocols on Top of Buses")
```

```check
(choice
  (ask "A module has been loaded with a program that never stops and ignores the host. How do you get it back?")
  (right "Power it off and on, and halt it in the boot window")
  (wrong "It cannot be recovered: the program has taken over"
    "The boot window exists for exactly this. No program can remove it, so the host can always halt the module just after power-on.")
  (wrong "Send it a stop command while it runs"
    "This program ignores the host. The boot window works whatever the program does, because it comes before the program runs.")
  (hint "Which promise holds whatever program the module holds?"))
```

---

## Safe by design

The same ideas make any protocol good: start in a safe state, always
leave a way to recover, and never leave a change half done.

```check
(choice
  (ask "The power fails halfway through loading a new program into a Sechs module. What does the module hold when it comes back?")
  (right "Its old program, whole")
  (wrong "Half of the new program"
    "The new program replaces the old one only after it has arrived whole and been checked. Half a program never replaces anything.")
  (wrong "Nothing: the old program was erased first"
    "Erasing first would leave nothing if the power failed. The protocol keeps the old program until the new one is complete.")
  (hint "When does the new program replace the old one?"))
```

---

## Connectors and modules

Many FPGA boards have **Pmod** connectors: rows of six pins, with four
signals, ground and 3.3 V, often doubled to twelve pins. A UART, SPI or
I2C connection fits on one row, so the same socket can take a memory
module, a network adapter, a sensor, a display or a programmable
module.

When a new need comes up, a small, cheap module meets it, and the
computer itself stays as it is.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connectors and Modules")
```

```check
(number
  (ask "A doubled Pmod connector has twelve pins: two rows of six. How many of them carry signals?")
  (answer 8) (from "2 * 4")
  (wrong-if 12 "Each row of six has a ground pin and a 3.3 V pin as well as its four signals.")
  (wrong-if 4 "4 is one row's signals. A doubled connector has two rows.")
  (hint "Each row: four signals, ground and 3.3 V."))
```

---

## Read the data sheet first

Take a temperature sensor of the LM75 family, made by several makers
since the 1990s. Its data sheet answers four questions before anything
is connected:

- **Supply:** it works from 3 to 5.5 V, so the 3.3 V pin of a Pmod
  can power it, and its signals are then at 3.3 V too.
- **Bus and address:** it is an I2C slave. Its address is 1001 followed
  by three bits set by its pins A2, A1 and A0.
- **Pull-ups:** SCL and SDA need pull-up resistors, typically 4.7k to
  3.3 V. Small boards that carry the sensor often have them.
- **What to read:** register 0 holds the temperature.

```example
(example
  (problem "A2, A1 and A0 are all connected to ground. What is the sensor's address?")
  (step "Ground is 0, so the three bits are 000.")
  (step "The address is 1001 then 000: binary 1001000.")
  (step "That is 64 + 8 = 72, or 0x48.")
  (answer "72 (0x48)"))
```

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connecting a Device")
```

```check
(number
  (ask "Now A2 and A1 go to ground, but A0 goes to 3.3 V. What is the address, in decimal?")
  (answer 73) (from "64 + 8 + 1")
  (wrong-if 72 "72 is the address with all three pins at ground. A0 at 3.3 V makes the last bit 1.")
  (wrong-if 1001001 "That is the address in binary. Add up its place values: 64 + 8 + 1.")
  (hint "The address is 1001 001 in binary."))
```

---

## Wire it, then ask it

The wiring is four pins: supply to 3.3 V, ground to ground, SCL to SCL
and SDA to SDA. Before the power goes on, check that the grounds are
shared, the voltages match, and supply and ground are not shorted.

The first test is a scan of the bus. If the sensor acknowledges
address 72, it is powered and wired correctly. If nothing answers, the
usual causes, in order, are: a missing pull-up, SCL and SDA swapped, no
power, and a wrong address.

```check
(choice
  (ask "You scan the bus and nothing answers. The sensor's own board has no pull-up resistors, and neither does the computer. What do you fix first?")
  (right "Add pull-up resistors to SCL and SDA")
  (wrong "Try other addresses in the program"
    "A scan already tries every address. With no pull-ups, the open-drain wires can never go high, so nothing on the bus can work.")
  (wrong "Swap SCL and SDA"
    "That is the next thing to check. But with no pull-ups at all, nothing can work however the wires go.")
  (hint "I2C's wires are open-drain. What takes them high?"))
```

---

## Reading the temperature

The first byte of register 0 is the temperature in whole degrees
Celsius, as a signed 8-bit number in **two's complement**: a byte from
0 to 127 is that number, and a byte above 127 is negative, worth the
byte minus 256. A byte of 23 means 23 degrees; a byte of 251 means
251 - 256 = -5 degrees.

```check
(number
  (ask "The sensor's byte is 246. What temperature is that, in degrees Celsius?")
  (answer -10) (from "246 - 256")
  (wrong-if 246 "A byte above 127 is negative in two's complement. Subtract 256.")
  (wrong-if 10 "Right size, wrong sign: 246 - 256 is below zero.")
  (hint "246 is above 127, so subtract 256."))
```

```check
(choice
  (ask "You have a new sensor in your hand. What comes first?")
  (right "Reading its data sheet: supply voltage, bus, address, pull-ups")
  (wrong "Writing the program that reads it"
    "The program comes last, once the device is wired and answers. Without the data sheet you do not know its address or what to read.")
  (wrong "Connecting it with the power on, to see whether it works"
    "Connect with the power off, after checking the voltages. A wrong supply can destroy the sensor at once.")
  (hint "What tells you its voltage and its address?"))
```

So: read the data sheet, check voltage and bus, connect with the power
off, make sure it answers, and only then write the program.
