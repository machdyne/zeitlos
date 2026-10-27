# Eight pins, eight bits

```lesson
(objective "Find the PMOD pin of each bit of a GPIO port, and read a port's value as eight pins.")
(minutes 15)
```

A GPIO port is the eight signal pins of one 12-pin PMOD connector.
Software sees the port as one byte, and each **bit** of it is one pin:
bits 0 to 3 are pins 1 to 4, the top row, and bits 4 to 7 are pins 7 to
10, the row under it.

Move the slider to see where each bit is.

```figure
(define b (slider 'bit 0 7 0 "Bit" 1))
(define pin (if (< b 4) (+ b 1) (+ b 3)))
(hw-pmod 40 50 40 2
  '("b0" "b1" "b2" "b3" "GND" "3V3" "b4" "b5" "b6" "b7" "GND" "3V3") pin)
(text 160 196 (string-append "bit " (fmt b 0) " is PMOD pin " (fmt pin 0)) 'centre 'body 'bold)
(caption "A GPIO port on a 12-pin connector: b0 is bit 0")
```

```sources
(source "Zeitlos docs: gpio.md, Pins and numbering")
```

```check
(number
  (ask "Which PMOD pin is bit 4 of a GPIO port?")
  (answer 7)
  (wrong-if 5 "Pin 5 is ground: it never carries a signal. After bit 3 on pin 4, the bits go on in the second row.")
  (wrong-if 4 "Pin 4 is bit 3. Bits count from 0, so bit 4 is the fifth signal pin, the first of the second row.")
  (hint "Bits 0 to 3 fill the top row. Where does the second row start?"))
```

---

## Bit order is pin order

Pin 4 is bit 3, and the next bit is not beside it: **bit 4 is pin 7**,
the first pin of the second row. This is the thing to remember when you
wire. Read the pins as printed, row by row, skipping the power pins,
and the bits go 0 to 7.

```check
(number
  (ask "You wire an LED to PMOD pin 9. Which bit of the port drives it?")
  (answer 6) (from "9 - 3")
  (wrong-if 8 "Pin 9 is not bit 8: a port has bits 0 to 7, and pins 5 and 6 are power, so the second row starts at bit 4.")
  (wrong-if 9 "A port has only bits 0 to 7. Count the second row from 7: pin 7 is bit 4, pin 8 bit 5, pin 9 ...")
  (wrong-if 2 "Bit 2 is pin 3, in the top row. Pin 9 is in the second row, which holds bits 4 to 7.")
  (hint "Pin 7 is bit 4. Count on from there."))
```

---

## A port as a number

Read all eight pins at once and you get a byte: bit 0 is worth 1, bit 1
is worth 2, and so on up to bit 7, worth 128. A pin that is high adds
its bit's worth. With every pin high, the port reads 255.

```example
(example
  (problem "Only pins 1 and 3 of a port are high. What does the port read?")
  (step "Pin 1 is bit 0, worth 1.")
  (step "Pin 3 is bit 2, worth 4.")
  (step (number (ask "So the port reads?") (answer 5) (from "1 + 4")
          (wrong-if 13 "That counts pin 3 as bit 3, worth 8. Pin 3 is bit 2.")))
  (answer "The port reads 5: 0000 0101."))
```

```check
(number
  (ask "Every pin of a port is high except PMOD pin 4. What does the port read?")
  (answer 247) (from "255 - 8")
  (wrong-if 251 "251 is 255 - 4, pin 3 low. Pin 4 is bit 3, worth 8.")
  (wrong-if 239 "239 is 255 - 16, bit 4 low, which is pin 7. Pin 4 is bit 3, worth 8.")
  (hint "Start from 255, every pin high. Pin 4 is bit 3: how much is it worth?"))
```

---

## A six-pin connector

A port needs both rows for all eight bits. On a connector with a single
row of six, only four pins exist, so a port there has only bits 0 to 3.

```sources
(source "Zeitlos docs: gpio.md, Ports with fewer than eight pins")
```

```check
(choice
  (ask "A GPIO port is on a 6-pin connector. Which bits have pins?")
  (right "Bits 0 to 3")
  (wrong "Bits 0 to 5" "Two of the six pins are ground and 3V3, which never carry a signal.")
  (wrong "Bits 4 to 7" "The single row is the row with pin 1, so it holds the first four bits, 0 to 3.")
  (hint "A 6-pin connector is the top row: pins 1 to 4, then power."))
```

So: bits 0 to 3 are pins 1 to 4, bits 4 to 7 are pins 7 to 10, and the
port reads as one byte.
