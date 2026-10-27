# Serial lines and the UART

```lesson
(objective "Say why connections are serial, and read a byte as a UART sends it: start bit, data bits least significant first, stop bit.")
(minutes 20)
(keywords "parallel (connection)" "serial (connection)" "UART" "baud rate" "start bit" "stop bit" "serial console" "RS-232")
```

A byte can go from one chip to another over eight wires, one for each
bit, all at once: a **parallel** connection. Or it can go over one
wire, one bit after another: a **serial** connection.

Parallel connections were common when chips were slow. Serial ones have
replaced them almost everywhere: they need fewer wires and pins, and at
high speeds it is hard to keep many wires exactly in step. A shift
register at each end turns the byte into a row of bits and back.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Serial and Parallel")
```

```check
(choice
  (ask "Why have serial connections replaced parallel ones almost everywhere?")
  (right "They need fewer wires, and one wire is easier to run fast than many kept in step")
  (wrong "A single wire carries more bits at once"
    "A single wire carries one bit at a time. Serial wins because one wire can be run very fast, while many parallel wires drift out of step.")
  (wrong "Parallel connections cannot carry whole bytes"
    "A parallel connection carries a whole byte at once, one bit on each wire. Its trouble is the number of wires and keeping them in step.")
  (hint "Think about the number of pins, and about many signals that must all arrive at the same moment."))
```

---

## The UART

A **UART** (universal asynchronous receiver-transmitter) sends bytes
over one wire in each direction, with no clock wire shared between the
two ends. Instead, both ends agree in advance on the speed, the
**baud rate**, in bits per second. Common rates are 9600 and 115200.

```check
(choice
  (ask "One board sends at 115200 baud; the other listens at 9600. What happens?")
  (right "The receiver reads nonsense, because it times the bits wrongly")
  (wrong "The receiver slows the sender down"
    "A UART has no shared clock and no way for one end to set the other's speed. Both must be set to the same rate beforehand.")
  (wrong "It works, only more slowly"
    "The receiver measures each bit by its own idea of the bit time. At the wrong rate, it samples in the wrong places and gets the wrong bits.")
  (hint "With no shared clock, how does the receiver know where one bit ends and the next begins?"))
```

---

## A byte on the wire

The wire is high when idle. To send a byte, the transmitter pulls it
low for one bit time, the **start bit**, which tells the receiver to
start timing. The eight data bits follow, **least significant first**:
b0, then b1, up to b7. Last comes a high **stop bit**.

Choose a character to see it sent. Time runs from left to right.

```figure
(define c (choice 'ch '("A" "B" "a" "0") "Character"))
(define n (list-ref '(65 66 97 48) c))
(define hx (list-ref '("41" "42" "61" "30") c))
(define bn (list-ref '("0100 0001" "0100 0010" "0110 0001" "0011 0000") c))
(text 160 30 (string-append (list-ref '("A" "B" "a" "0") c) " = 0x" hx " = " bn) 'centre 'body 'bold)
(wo-uart 16 90 24 n)
(text 4 94 "1" 'tiny)
(text 4 124 "0" 'tiny)
(arrow 40 170 280 170 'ink 1)
(text 160 186 "time: b0 is sent first" 'centre 'small)
(caption "The bits go out in the reverse of how we write them.")
```

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: UART")
```

```check
(choice
  (ask "The letter A is 0100 0001. Just after the start bit, what level is the wire for the first data bit?")
  (right "High: b0 is 1")
  (wrong "Low: the leftmost digit is 0"
    "The leftmost digit is b7, the most significant bit, and it is sent last. A UART sends the least significant bit, b0, first, and b0 of A is 1.")
  (hint "Which end of 0100 0001 is b0?"))
```

---

## Ten bits for a byte

Each byte takes ten bit times on the wire: the start bit, eight data
bits and the stop bit. After the stop bit the wire stays high, idle,
until the next start bit.

```check
(number
  (ask "At 9600 baud, how many bytes a second can a UART send at most?")
  (answer 960) (from "9600 / 10")
  (wrong-if 1200 "9600 / 8 counts only the data bits. Each byte also has a start bit and a stop bit: ten bit times in all.")
  (wrong-if 9600 "9600 is bits a second, not bytes.")
  (hint "How many bit times does one byte take, with its start and stop bits?"))
```

```check
(choice
  (ask "A UART has sent one byte and has nothing more to send. What level is the wire?")
  (right "High, as when idle")
  (wrong "Low, until the next byte"
    "A low wire would look like a start bit. The idle wire is high, and only a start bit takes it low.")
  (hint "What does the receiver take as the sign that a byte begins?"))
```

---

## Timing and wiring

The receiver samples each bit near its middle, using its own clock.
Because timing starts again at every start bit, the two clocks need
only agree to within a few percent.

A UART connection needs three wires: transmit, receive and ground.
Each device's transmit wire goes to the other's receive wire.
Connecting them the wrong way round is the most common reason a serial
link shows nothing.

```check
(choice
  (ask "You join board A to board B: A's transmit to B's transmit, A's receive to B's receive, and ground to ground. Nothing arrives. Why?")
  (right "Each transmit must go to the other's receive")
  (wrong "A UART needs a clock wire as well"
    "A UART has no clock wire: both ends agree on the baud rate instead. The trouble is that two transmitters are joined, and nothing listens to either.")
  (wrong "The ground wire is not needed and confuses the signal"
    "The ground wire is needed: a voltage is measured against ground, so both boards must share it. The two transmit wires are the trouble.")
  (hint "Who listens to what A sends?"))
```

---

## The serial console

A **serial console** is a text screen and keyboard connected to a
computer over a UART. It is often the first thing a new board shows,
and the last way in when everything else has failed: a UART is the
oldest and most forgiving way to talk to a computer.

```warn
(warn "RS-232 voltages"
  "Older computers' serial ports follow the RS-232 standard, whose voltages swing to plus and minus 12 V. Never connect one directly to a 3.3 V pin: it can destroy the pin. Use an adapter that converts between the two.")
```

Old serial ports follow **RS-232**, a standard for the same kind of
signal at much higher voltages.

```check
(choice
  (ask "You want to connect an old computer's RS-232 serial port to a board's 3.3 V UART pins. What do you do?")
  (right "Put an adapter between them that converts the voltages")
  (wrong "Join them directly: it is the same kind of signal"
    "The bits are framed the same way, but RS-232 swings to plus and minus 12 V, which can destroy a 3.3 V pin.")
  (wrong "Lower the baud rate so the voltages are safe"
    "The baud rate sets how fast bits are sent, not their voltage. RS-232 still swings to plus and minus 12 V.")
  (hint "The two agree on the bits, but not on the voltages."))
```

So: a UART sends each byte as a low start bit, eight data bits least
significant first, and a high stop bit, at a speed both ends agree on.
