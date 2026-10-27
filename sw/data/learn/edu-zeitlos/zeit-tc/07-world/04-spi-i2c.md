# SPI and I2C

```lesson
(objective "Say what each wire of SPI and I2C does, how a transfer works on each, and when to choose which.")
(minutes 25)
(keywords "SPI" "master (of a bus)" "slave (of a bus)" "SCK" "MOSI" "MISO" "chip select" "I2C" "SCL" "SDA" "start condition" "stop condition" "acknowledge (I2C)" "scan (a bus)" "MMOD")
```

Between chips and small devices, two more serial connections are
everywhere. Both use a clock wire.

**SPI** (serial peripheral interface) is a fast, simple connection
between one **master**, which runs every transfer, and one or more
**slaves**. It uses four wires:

- **SCK**, the clock, driven by the master;
- **MOSI** (master out, slave in): data from the master;
- **MISO** (master in, slave out): data to the master;
- **CS**, **chip select**: one for each slave, pulled low to choose it.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: SPI")
```

```check
(number
  (ask "A master talks to three SPI slaves. How many wires does it need, not counting ground and power?")
  (answer 6) (from "3 + 3")
  (wrong-if 4 "4 is enough for one slave. Each slave needs its own chip select; SCK, MOSI and MISO are shared.")
  (wrong-if 12 "The clock and the two data wires are shared by all the slaves. Only chip select is one for each.")
  (hint "SCK, MOSI and MISO are shared. How many chip selects?"))
```

---

## A ring of shift registers

The master pulls one slave's chip select low, then pulses the clock.
On each pulse, one bit goes out on MOSI and one comes in on MISO.
Inside, the master and the slave each have a shift register, joined in
a ring. Move the slider to pulse the clock.

```figure
(define k (slider 'pulses 0 8 0 "Clock pulses" 1))
(define r0 (+ (* 177 256) 102))
(define p (expt 2 k))
(define r (+ (floor (/ r0 p)) (* (modulo r0 p) (/ 65536 p))))
(define (hx v) (string-append (substring "0123456789ABCDEF" (floor (/ v 16)) (+ 1 (floor (/ v 16))))
                              (substring "0123456789ABCDEF" (modulo v 16) (+ 1 (modulo v 16)))))
(text 80 40 "master" 'centre 'small 'bold)
(text 240 40 "slave" 'centre 'small 'bold)
(for-range 0 16
  (lambda (j)
    (wo-bitbox (+ 8 (* j 18) (if (> j 7) 16 0)) 60 18 (wo-bit r (- 15 j)))))
(arrow 152 69 168 69 'ink 1)
(text 160 54 "MOSI" 'centre 'tiny)
(path '(312 69 316 69 316 104 2 104 2 69) 'ink 1)
(arrow 2 69 8 69 'ink 1)
(text 160 118 "MISO" 'centre 'tiny)
(text 80 150 (string-append "master holds 0x" (hx (floor (/ r 256)))) 'centre 'small)
(text 240 150 (string-append "slave holds 0x" (hx (modulo r 256))) 'centre 'small)
(text 160 180 "at the start: master 0xB1, slave 0x66" 'centre 'tiny)
(caption "Each pulse moves every bit one place round the ring.")
```

Because the master makes the clock, the two ends need not agree on a
speed in advance, and SPI can run at tens of megahertz. SPI connects
flash memory chips, SD cards, displays and many sensors. Machdyne's
**MMOD** memory modules are SPI devices on a six-pin socket: the four SPI
signals, ground and 3.3 V.

```check
(choice
  (ask "After 8 clock pulses, what does the master's shift register hold?")
  (right "The byte the slave held at the start")
  (wrong "The byte it held at the start"
    "Every pulse moves one bit out of the master and one in from the slave. After 8 pulses all 8 have gone round: the master and the slave have swapped bytes.")
  (wrong "Nothing: its bits have all gone out"
    "For each bit that leaves on MOSI, one arrives on MISO. The register is always full.")
  (hint "Slide to 8 and compare with the start."))
```

---

## I2C

**I2C** (inter-integrated circuit, also written IIC, and called TWI,
two-wire interface, by some makers) connects many devices with only
two wires:

- **SCL**, the clock;
- **SDA**, the data.

Both wires are open-drain, with pull-up resistors: any device can pull
them low, and none can drive them high. The master starts every
transfer and drives the clock; the slaves answer when addressed. (The
two roles are also called controller and target.) Each slave has its
own **7-bit address**.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: I2C")
```

```check
(number
  (ask "How many different 7-bit addresses are there?")
  (answer 128) (from "2^7")
  (wrong-if 7 "7 is the number of bits. Each bit doubles the number of patterns.")
  (wrong-if 127 "127 is the largest address, but 0 is a pattern too: 0 to 127 is 128 addresses.")
  (hint "2 to the power 7."))
```

---

## A transfer

While the clock is high, the data must stay still; it changes only
while the clock is low. Two exceptions mark the ends of a transfer:

- a **start condition**: SDA falls while SCL is high;
- a **stop condition**: SDA rises while SCL is high.

After the start come the address and one bit saying read or write.
The slave with that address answers by pulling SDA low: it
**acknowledges**. Bytes follow, each acknowledged by the device that
receives it, and the stop condition ends the transfer.

```figure
(text 20 42 "SCL" 'small 'bold)
(wo-wave 20 50 16 30 '(1 1 1 1 0 0 1 1 0 0 1 1 0 0 1 1 1 1) 'ink)
(text 20 112 "SDA" 'small 'bold)
(wo-wave 20 120 16 30 '(1 1 0 0 0 1 1 1 1 0 0 0 0 0 0 0 1 1) 'ink)
(line 52 40 52 160 'grey 0.5)
(line 276 40 276 160 'grey 0.5)
(text 52 176 "start" 'centre 'small)
(text 276 176 "stop" 'centre 'small)
(text 132 176 "a 1" 'centre 'small)
(text 196 176 "a 0" 'centre 'small)
(caption "SDA changes while SCL is low, except at start and stop.")
```

```check
(choice
  (ask "SDA falls while SCL is high. What does that mean?")
  (right "A transfer starts")
  (wrong "A 0 bit is being sent"
    "Data bits change only while SCL is low. A change while SCL is high is never a data bit: falling is the start condition.")
  (wrong "The transfer stops"
    "A stop is SDA rising while SCL is high. Falling is the start.")
  (hint "Look at the figure: what happens on SDA, and where, at the start mark?"))
```

---

## Slower, but many devices

I2C is slower than SPI, typically 100 or 400 kilobits a second, but
its two wires reach dozens of devices: temperature sensors, clocks,
small displays, memory chips, and pin expanders that add sixteen pins
over the two wires.

Because every device has an address, a master can **scan** the bus to
find what is connected: it tries each address in turn and notes which
ones acknowledge.

```check
(choice
  (ask "A board has few pins left. It must read eight slow sensors. Which bus suits it better?")
  (right "I2C: two wires reach all eight, each by its address")
  (wrong "SPI: it is faster"
    "SPI is faster, but each slave needs its own chip select: 3 + 8 = 11 wires. Slow sensors do not need the speed.")
  (hint "Count the wires each bus needs for eight devices."))
```

---

```check
(choice
  (ask "A master scans an I2C bus. Address 72 acknowledges; every other address is silent. What has it learned?")
  (right "One device, at address 72, is connected and answering")
  (wrong "72 devices are connected"
    "72 is an address, not a count. Only one address acknowledged, so one device answered.")
  (wrong "Nothing: a scan only tests the wires"
    "A device acknowledges only its own address. So an answer at 72 means a device there is powered, wired and listening.")
  (hint "What does a device do when it hears its own address?"))
```

So: SPI is four wires and fast, a ring of shift registers; I2C is two
open-drain wires shared by many addressed devices.
