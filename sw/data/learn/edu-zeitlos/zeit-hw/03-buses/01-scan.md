# Finding I2C devices

```lesson
(objective "Scan a PMOD port's I2C bus with the i2c command, and read the addresses it finds.")
(minutes 20)
(keywords "POSIX shell (of Zeitlos)" "scan (a bus)")
```

The `i2c` command runs in the **POSIX shell**, a Unix-style shell:
open `term` from the dock and choose POSIX instead of REPL. (If the panel
says POSIX is not installed, the REPL has I2C commands too, as a later
lesson shows.)

To **scan** a bus is to call each address in turn and list the devices
that answer. Type:

```
$ i2c scan
0x48
```

Each line is the address of one device. With none, it says
`nothing answers`.

```sources
(source "Zeitlos docs: i2c_app.md; terminal.md, Starting the shells")
```

```try
(try (app "term") (task "Open term from the dock, choose POSIX, and type i2c scan. Then come back."))
```

```check
(choice
  (ask "i2c scan prints nothing answers. Which is a possible reason?")
  (right "No device on the bus acknowledged any address")
  (wrong "The scan found a device at address 0" "Every found device gets a line with its address. nothing answers means no device answered at all.")
  (wrong "The scan was too fast for the devices" "A device that is powered and wired answers a scan. The causes are wiring, power and pull-ups, not speed.")
  (hint "What does a scan list?"))
```

---

## Which pins

By default `i2c` uses the bus `pmod0`: GPIO port 0, with **SCL on
PMOD pin 1** and **SDA on PMOD pin 2** (bits 0 and 1). Another port is
chosen with `-b`: `i2c -b pmod1 scan`. `i2c buses` lists the buses
there are.

The board's pull-ups are on these pins already, so a short bus works
with nothing more than the device, its power and the two wires.

```sources
(source "Zeitlos docs: i2c_app.md; i2c.md, Wiring")
```

```check
(choice
  (ask "You wire a sensor for i2c scan on port 0. Where do its SCL and SDA go?")
  (right "SCL to PMOD pin 1, SDA to PMOD pin 2")
  (wrong "SCL to pin 3, SDA to pin 4" "That is the Pmod I2C type's layout. The i2c command uses pins 1 and 2.")
  (wrong "SDA to pin 1, SCL to pin 2" "The other way round: the clock, SCL, is on pin 1.")
  (hint "The default bus uses bits 0 and 1 of port 0."))
```

---

## Seven-bit addresses

An I2C address is 7 bits. `i2c` scans 0x08 to 0x77, the addresses
devices may use; the ones below and above are reserved by the I2C
standard for other jobs.

```check
(number
  (ask "How many addresses does i2c scan try, from 0x08 to 0x77 inclusive?")
  (answer 112) (from "119 - 8 + 1")
  (wrong-if 111 "Count both ends: from 8 to 119 is 119 - 8 + 1 addresses.")
  (wrong-if 128 "128 is every 7-bit address. The scan leaves out 0x00-0x07 and 0x78-0x7F.")
  (hint "0x77 is 119 and 0x08 is 8.")
  (hint "Count from 8 to 119, both included."))
```

---

## Addresses on paper

Some data sheets give the address already shifted left, with the
read/write bit added: an 8-bit "write address" twice the real one.
Zeitlos always takes the 7-bit address, and it adds the read/write bit
itself. A temperature sensor of the LM75 family, with its address pins
at ground, is at binary 1001000: **0x48**, or 72.

```sources
(source "Zeitlos docs: i2c.md, Wiring (7-bit throughout)")
(source "Timeless Computing (docs/tc.md), Talking to the World: Connecting a Device")
```

```check
(text
  (ask "A data sheet gives a display's write address as 0x78. Which address do you give i2c? (In hex, with 0x.)")
  (accept "0x3c") (accept "0x3C") (accept "3c") (accept "3C")
  (wrong-if "0x78" "0x78 has the read/write bit added on the right. Shift it off: halve the number.")
  (wrong-if "0xf0" "That doubles it. The 7-bit address is half of the 8-bit one.")
  (hint "The 8-bit form is the 7-bit address times 2.")
  (hint "0x78 is 120. Half of it is 60, which is 0x3C."))
```

---

```check
(number
  (ask "i2c scan prints 0x48. What is that address in decimal?")
  (answer 72) (from "4 * 16 + 8")
  (wrong-if 48 "48 is the hex digits read as decimal. The 4 counts sixteens.")
  (hint "4 sixteens and 8."))
```

So: `i2c scan` in the POSIX shell lists the 7-bit addresses that answer
on PMOD pins 1 (SCL) and 2 (SDA) of port 0.
