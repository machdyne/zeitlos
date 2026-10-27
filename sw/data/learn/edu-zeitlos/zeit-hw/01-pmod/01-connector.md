# The PMOD connector

```lesson
(objective "Find pin 1, the signal pins, ground and 3.3 V on a PMOD connector.")
(minutes 15)
(keywords "PMOD (connector)" "connector (on a board)" "3V3 (pin)")
```

A **connector** is a row of metal pins or sockets that another board
or a cable plugs into. Most Zeitlos computers have one or two
**PMOD connectors** on an edge of the board: a row of six pins, or two
rows of six. A PMOD connector is where you plug in a module or wire your own
circuit. The name comes from the Pmod standard, which many FPGA boards
and modules follow.

Of the six pins in a row, four carry **signals**, and two carry the
power that a module runs on: ground and 3.3 V. On a board, 3.3 V is
often printed **3V3**, with the V where the decimal point would be.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Connectors and Modules; Connector Pinouts: Pmod")
```

```check
(number
  (ask "A row of a PMOD connector has 6 pins. How many of them carry signals?")
  (answer 4) (from "6 - 2")
  (wrong-if 6 "Two pins of each row are not signals: they are ground and 3.3 V, the power for a module.")
  (wrong-if 2 "It is the other way round: 2 pins are power (ground and 3.3 V), and the other 4 carry signals.")
  (hint "Take away the two power pins: ground and 3.3 V."))
```

---

## The pins in order

The pins are numbered as in this figure. Pins 1 to 4 are the signals
of the top row, pin 5 is ground (GND) and pin 6 is 3V3. A 12-pin
connector adds a second row in the same order: signals on 7 to 10,
ground on 11 and 3V3 on 12.

Move the slider to pick a pin.

```figure
(define p (slider 'pin 1 12 1 "Pin" 1))
(hw-pmod 40 50 40 2
  '("" "" "" "" "GND" "3V3" "" "" "" "" "GND" "3V3") p)
(text 160 190
  (cond ((or (= p 5) (= p 11)) "ground: 0 V")
        ((or (= p 6) (= p 12)) "3V3: the 3.3 V supply")
        (else "a signal pin"))
  'centre 'body 'bold)
(caption "A 12-pin PMOD connector, pins seen from above")
```

```check
(choice
  (ask "On a 12-pin PMOD connector, which pins are ground?")
  (right "5 and 11")
  (wrong "1 and 7" "Pins 1 and 7 are the first signal pins of each row. Ground comes after the four signals.")
  (wrong "6 and 12" "Those are the 3V3 pins, the supply. Ground is the pin just before each of them.")
  (wrong "5 and 6" "Pin 6 is 3V3, not ground. On a 12-pin connector the second ground is in the other row.")
  (hint "In each row: four signals, then ground, then 3V3."))
```

---

## Finding pin 1

The numbers are seldom printed beside every pin. Pin 1 is marked
instead: by a square pad where the others are round, a dot, an arrow or
a small 1 printed on the board. Count from it.

A 6-pin module plugged into a 12-pin connector goes in the row of pins
1 to 6, so that its own ground and 3V3 meet the connector's.

```sources
(source "Timeless Computing (docs/tc.md), Connector Pinouts: introduction and Pmod")
```

```check
(choice
  (ask "You are looking at a PMOD connector on a board with no numbers printed beside it. How do you find pin 1?")
  (right "Look for the pin marked with a square pad, a dot, an arrow or a 1")
  (wrong "It is always the pin nearest the corner of the board"
    "Boards are laid out in many ways, so the corner tells you nothing. Pin 1 is marked on the board itself.")
  (wrong "It is always the pin on the left as you face it"
    "That depends on which way you hold the board. The mark on the board does not.")
  (hint "Pin 1 is the one pin that looks different on the board."))
```

---

## Practice

```check
(number
  (ask "On a 12-pin PMOD connector, which pin is directly below pin 3, in the second row?")
  (answer 9) (from "3 + 6")
  (wrong-if 4 "Pin 4 is next to pin 3 in the same row. The second row starts at 7, under pin 1.")
  (wrong-if 10 "Count again: the second row is 7, 8, 9, 10, so the third pin in it is 9.")
  (hint "The second row is numbered 7 to 12. Its first pin is under pin 1.")
  (hint "Pin 1 has 7 under it, pin 2 has 8, pin 3 has ..."))
```

---

```check
(choice
  (ask "A module needs 3.3 V and ground. Which pins of a 6-pin PMOD row give it them?")
  (right "Pin 6 for 3.3 V and pin 5 for ground")
  (wrong "Pin 1 for 3.3 V and pin 2 for ground"
    "Pins 1 and 2 are signal pins. The power pins are the last two of the row.")
  (wrong "Pin 5 for 3.3 V and pin 6 for ground"
    "The other way round: ground is pin 5 and 3V3 is pin 6.")
  (hint "The order of a row is four signals, GND, 3V3."))
```

So: in each row of a PMOD connector, pins 1 to 4 (or 7 to 10) are
signals, then come ground and 3V3, counted from the marked pin 1.
