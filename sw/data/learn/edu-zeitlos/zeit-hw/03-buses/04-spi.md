# SPI on GPIO pins

```lesson
(objective "Open an SPI bus on a GPIO port from the REPL, select a device and exchange bytes with it.")
(minutes 20)
(keywords "SPI mode (of a bus)")
```

The REPL can make an SPI master out of any four pins of one port:

```
> spi-init 0 3 1 2 0
0
```

The numbers are the port, then the bits for SCK, MOSI, MISO and CS. It
answers a handle, here `0`, which the other SPI commands take. Without
more numbers, the bus runs at 1 MHz in **SPI mode** 0: the clock idles
low and data is read on its rising edge.

```sources
(source "Zeitlos docs: spi.md, Quick start; Clock modes; Scheme API")
```

```check
(choice
  (ask "An MMOD follows Pmod type 2: CS on pin 1, MOSI on 2, MISO on 3, SCK on 4. Which command opens it on port 0?")
  (right "spi-init 0 3 1 2 0")
  (wrong "spi-init 0 0 1 2 3" "That puts SCK on bit 0, pin 1. On an MMOD pin 1 is CS. The order is port, SCK, MOSI, MISO, CS.")
  (wrong "spi-init 0 4 2 3 1" "Those are PMOD pin numbers. spi-init takes bits: pin 4 is bit 3, pin 1 bit 0.")
  (hint "Bits are pin - 1 in the top row. The order is SCK, MOSI, MISO, CS.")
  (hint "SCK on pin 4 is bit 3; CS on pin 1 is bit 0."))
```

---

## Select, exchange, release

`spi-xfer` does not touch CS, because a device usually wants several
transfers inside one selection: a command, an address, then data.

```
> spi-select 0 #t
> (spi-xfer 0 '(159 0 0 0))
(255 239 64 24)
> spi-select 0 #f
```

This command is typed whole, in brackets, because it holds a list. Each byte
sent brings one back. 159 is 0x9F, the command many flash
chips answer with their maker and size; the three bytes after the first
are that ID.

```check
(choice
  (ask "You forget spi-select 0 #t and send (spi-xfer 0 '(159 0 0 0)). What happens?")
  (right "CS stays high, the chip is not selected, and every byte comes back 255")
  (wrong "spi-xfer selects the chip itself" "It does not: CS is left to you, so that one selection can hold several transfers.")
  (wrong "The chip answers its ID as usual" "An unselected chip ignores the clock. MISO is then held high by the pull-up: 255.")
  (hint "Which command pulls CS low?"))
```

---

## 255 means nothing there

With nothing driving MISO, the pull-up holds it high, so every byte
reads 255 (0xFF). A device that should answer and gives only 255 is not
selected, not powered, or not wired. When you only read, `spi-xfer 0 4`
sends four bytes of 255, which devices take as "nothing to say".

If every byte comes back shifted by one bit, so that 1 reads as 2, try
another mode: `spi-init 0 3 1 2 0 1` uses mode 1.

```sources
(source "Zeitlos docs: spi.md, Push-pull, unlike I2C; Clock modes; In-place transfers")
```

```check
(choice
  (ask "A sensor's ID should be 0x58, and you read 0xB0 every time. What do you try?")
  (right "Another SPI mode: the bits are shifted by one")
  (wrong "Swap MOSI and MISO" "Swapped data lines give 255 or nothing sensible. 0xB0 is 0x58 shifted left one bit: a clock-phase mistake.")
  (wrong "Add pull-ups as for I2C" "SPI lines are driven both ways. A shift by one bit is the clock mode.")
  (hint "Write 0x58 and 0xB0 in binary and compare."))
```

---

## The mmod app

For MMOD memory modules there is an app: **mmod**, in the dock's Tools
group on a machine with GPIO. Its DETECT button reads the chip's ID,
which proves all four wires at once; it keeps WRITE and ERASE off until
DETECT has passed.

```sources
(source "Zeitlos docs: mmod.md, DETECT first, always")
```

```check
(choice
  (ask "Why does mmod refuse to write before DETECT has passed?")
  (right "If CS cannot let go, the chip stays selected, and a write could land somewhere nobody chose")
  (wrong "DETECT erases the chip first" "DETECT only reads the ID. It proves the wires, including CS, before anything is written.")
  (wrong "Writing is slower without it" "It is about safety, not speed: an unproven CS can turn every clock into a command.")
  (hint "What does reading an ID prove about the wires?"))
```

So: `spi-init` names the pins, `spi-select` and `spi-xfer` do a
transfer, 255 means nothing answered, and a one-bit shift means the
wrong mode.
