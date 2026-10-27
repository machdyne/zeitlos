# Reading and writing a device

```lesson
(objective "Read and write a device's registers with i2c, and turn what it prints into a value.")
(minutes 20)
(keywords "register (of a device)" "repeated start (I2C)" "NACK (I2C)")
```

Most I2C devices hold their data in numbered **registers**: small
memories of a byte each, inside the device. To read one, you first
write the register's number, then read:

```
$ i2c write 0x48 0 read 1
17
```

Between the write and the read, `i2c` sends a **repeated start**: a new
start condition with no stop before it, so the device keeps the
register number it was given. This is how almost every device's
registers are read.

```sources
(source "Zeitlos docs: i2c_app.md; i2c.md, Repeated START, and why write_read exists")
```

```check
(choice
  (ask "Why does i2c write 0x48 0 read 1 use a repeated start rather than a stop between the write and the read?")
  (right "A device that sees a stop may forget the register number it was just given")
  (wrong "A stop would end the power to the device" "A stop only ends a transfer. The trouble is that some devices then forget the register pointer.")
  (wrong "I2C cannot do a read after a stop" "It can, but the device may have forgotten which register you chose. The repeated start keeps it.")
  (hint "What was the write for?"))
```

---

## What it prints

`i2c` prints each byte it reads as two hex digits, without `0x`. An
LM75 temperature sensor keeps the temperature in register 0, whose
first byte is whole degrees Celsius as a signed byte. So `17` is
0x17, which is 23 degrees.

```example
(example
  (problem "i2c write 0x48 0 read 1 prints 19. What is the temperature?")
  (step "19 is hex: 0x19.")
  (step (number (ask "0x19 in decimal?") (answer 25) (from "1 * 16 + 9")
          (wrong-if 19 "19 is the hex digits read as decimal. The 1 counts sixteens.")))
  (answer "25 degrees Celsius."))
```

```check
(number
  (ask "The sensor answers 1e. How many degrees is that?")
  (answer 30) (from "1 * 16 + 14")
  (wrong-if 114 "e is a hex digit, fourteen, not a 1 and a 4. 0x1E is 16 + 14.")
  (hint "1 sixteen and e, which is fourteen."))
```

---

## Below zero

A byte above 127 is negative here, as in two's complement: subtract
256. The sensor's `fb` is 251, and 251 - 256 is -5 degrees.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connecting a Device")
```

```check
(number
  (ask "The sensor answers f6. What is the temperature, in degrees?")
  (answer -10) (from "15 * 16 + 6 - 256")
  (wrong-if 246 "246 is 0xF6 as a plain byte. Above 127, subtract 256.")
  (hint "0xF6 is 246. It is above 127, so subtract 256."))
```

---

## Writing

`i2c write ADDR BYTE...` writes bytes, usually a register number and
then its new value. For a chip at 0x20 that keeps its pins'
directions in register 6:

```
$ i2c write 0x20 6 0x00
```

Numbers are decimal, or hex with `0x`: `6` and `0x06` are the same. If
the device does not answer, `i2c` prints `0x20: no answer (NACK)`: a
**NACK**, not acknowledged.

```check
(choice
  (ask "Which command writes the value 255 to register 3 of the device at 0x20?")
  (right "i2c write 0x20 3 255")
  (wrong "i2c write 0x20 255 3" "The register number comes first after the address, then the value.")
  (wrong "i2c write 3 0x20 255" "The address comes first: i2c write ADDR, then the bytes.")
  (wrong "i2c read 0x20 3 255" "read takes an address and a count of bytes to read. Writing is i2c write.")
  (hint "i2c write, the address, the register, the value."))
```

---

```check
(text
  (ask "Type the command that reads 2 bytes from register 0 of the device at 0x48.")
  (accept "i2c write 0x48 0 read 2") (accept "i2c write 72 0 read 2")
  (accept "i2c write 0x48 0x00 read 2") (accept "i2c write 0x48 0x0 read 2")
  (wrong-if "i2c read 0x48 2" "That reads 2 bytes from wherever the device's pointer is. To start at register 0, write the 0 first.")
  (wrong-if "i2c write 0x48 0 read 1" "That reads one byte. Two were asked for.")
  (hint "Write the register number, then read.")
  (hint "i2c write 0x48 0 read ..."))
```

So: write the register number and read with a repeated start; `i2c`
prints bytes in hex, and a byte above 127 may stand for a negative
number.
