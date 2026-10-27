# When nothing answers

```lesson
(objective "Tell a missing device from a wiring fault on an I2C bus, and find the cause in a fixed order.")
(minutes 20)
```

`i2c` tells two failures apart:

| It prints | It means |
|---|---|
| `0x48: no answer (NACK)` | the bus works, and no device answered that address |
| `pmod0: the bus is stuck (no pull-ups, or a device holding a line)` | a line that was let go never came back up |

A NACK is an ordinary answer: nobody is at that
address. A **stuck** bus is a wiring fault, because nothing on an I2C
bus can hold a line high; it is a missing pull-up, a short, or a device
holding a line low.

```sources
(source "Zeitlos docs: i2c.md, NACK is not TIMEOUT; sw/apps/i2c/i2c.c (its messages)")
```

```check
(choice
  (ask "i2c write 0x48 0 read 1 prints 0x48: no answer (NACK). Where do you look first?")
  (right "At the device: is it powered, and is 0x48 really its address?")
  (wrong "At the pull-ups: the bus is stuck" "A stuck bus has its own message. A NACK means the bus worked and nobody answered at 0x48.")
  (wrong "At the i2c command: it is broken" "The command did its job and reported that no device answered. Look at the device.")
  (hint "Which of the two messages is this, and what does it mean?"))
```

---

## The usual causes, in order

When a device does not answer a scan, check, with the power off:

1. **Pull-ups** missing on SCL and SDA.
2. **SCL and SDA swapped.**
3. **No power** to the device, or no shared ground.
4. **A wrong address**: an 8-bit address from a data sheet, or address
   pins set differently.

The board's own pull-ups are weak. They make a short bus work, slowly;
for long wires or several devices, add 2.2k to 4.7k resistors from SCL
and SDA to 3V3.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connecting a Device")
(source "Zeitlos docs: i2c.md, Weak pull-ups cost speed, not correctness; sechs_app.md, Wiring")
```

```check
(choice
  (ask "A sensor on a 2 m cable does not answer, though it does on a short one. What do you try first?")
  (right "Add 2.2k to 4.7k pull-ups from SCL and SDA to 3V3")
  (wrong "Swap SCL and SDA" "It works on a short cable, so the wiring order is right. A long cable needs stronger pull-ups.")
  (wrong "Give the sensor 5 V for a stronger signal" "Never: 5 V on the bus would reach the board's 3.3 V pins. Stronger pull-ups to 3V3 are the fix.")
  (hint "The board's own pull-ups are weak. What does a long cable add?"))
```

---

## I2C from the REPL

The REPL has I2C too. `i2c-init 0 0 1` opens a bus on port 0 with SCL
on bit 0 and SDA on bit 1, and answers a handle, a number for that bus:
`0`. Then `i2c-scan 0` answers the addresses in decimal, as a list:
`(72)` for 0x48.

If a device was cut off in the middle of a byte, it may hold SDA low
for ever. `i2c-recover 0` clocks SCL up to nine times and sends a stop,
which frees it. If SDA is still low after that, the fault is in the
wiring, and no software can fix it.

```sources
(source "Zeitlos docs: i2c.md, Quick start; Recovering a wedged bus; Scheme API")
```

```check
(choice
  (ask "i2c-scan 0 answers (60). Which address in hex is that?")
  (right "0x3C")
  (wrong "0x60" "The REPL answers in decimal. 60 is 3 sixteens and 12: 0x3C.")
  (wrong "0x78" "0x78 is 120, twice 60: the 8-bit form. The REPL gives the 7-bit address.")
  (hint "60 = 3 x 16 + 12."))
```

---

```check
(choice
  (ask "After i2c-recover, SDA still reads low. What now?")
  (right "Power off and look at the wiring: a short or a device holding the line")
  (wrong "Run i2c-recover again until it works" "Nine clocks free any device stuck in a byte. If SDA is still low, the cause is not a stuck byte.")
  (wrong "Use another address" "Addresses do not matter while a line is held low: no transfer can start.")
  (hint "What does this lesson say no software can fix?"))
```

---

```check
(choice (several)
  (ask "A scan prints nothing answers for a new sensor. Which are worth checking? Choose all that are.")
  (right "SCL and SDA may be swapped")
  (right "The sensor may have no power or no shared ground")
  (wrong "The scan may have missed it because it was too quick" "A powered, wired device answers a scan every time.")
  (wrong "The sensor needs 5 V signals to be heard" "The board's pins are 3.3 V, and so must the sensor's signals be.")
  (hint "Two are wiring and power."))
```

So: a NACK means no device at that address; a stuck bus means wiring;
and pull-ups, the wire order, power and the address are checked in that
order.
