# Voltage

```lesson
(objective "Say what voltage is, measure it in volts between two points, and use ground as the point of 0 V.")
(minutes 15)
(keywords "voltage" "volt (unit)" "power supply" "ground (electrical)" "cell (battery)")
```

You will learn what drives a current, and how it is measured.

Charge flows only when something drives it. That push is **voltage**,
measured in **volts** (V). A battery, or a **power supply** such as a
phone charger, makes a voltage between its two terminals. If a
conductor joins them, a current flows through it.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Voltage")
```

```check
(choice
  (ask "A battery sits in a drawer, connected to nothing. What does it have?")
  (right "A voltage between its terminals, but no current")
  (wrong "A current, but no voltage"
    "Current flows only round a complete circuit, and here there is none. The voltage, the push, is there all the time, waiting.")
  (wrong "Neither, until it is connected"
    "The voltage is there all along: it is what the battery makes. Only the current waits for a complete circuit.")
  (hint "Which one needs a complete circuit?"))
```

---

## Water in a hose

Water in a garden hose is a useful picture, though not a perfect one:

| Water | Electricity |
|---|---|
| the difference in pressure | voltage |
| how much water flows each second | current |
| a squeeze in the hose, holding the flow back | the subject of the next lesson |

```check
(choice
  (ask "In the water picture, what is the current?")
  (right "How much water flows each second")
  (wrong "The difference in pressure"
    "The pressure difference is what pushes the water: that is the voltage. The current is the flow it causes.")
  (wrong "How hard the hose is squeezed"
    "A squeeze holds the flow back. The current is the flow itself: how much passes each second.")
  (hint "Current is a flow."))
```

---

## Between two points

A voltage is always measured **between two points**. A wire is "at 3.3
volts" only compared with some other point. In a circuit, that point is
called **ground**, and it is 0 V by definition. When a single voltage
is given for a wire, it is the voltage between that wire and ground.

An alkaline **cell**, the kind sold as a single battery, gives 1.5 V. Cells stacked + to - add their voltages.
Move the slider and read each point's voltage from ground.

```figure
(define n (slider 'n 1 4 2 "Cells" 1))
(still (el-ground 110 206))
(for-range 0 n
  (lambda (k)
    (let ((y (- 200 (* 40 k))))
      (el-cell 110 (- y 40) y "cell")
      (dot 110 y)
      (text 190 (+ y 4) (string-append (fmt (* 1.5 k) 1) " V") 'small))))
(dot 110 (- 200 (* 40 n)))
(text 190 (- 196 (* 40 n)) (string-append (fmt (* 1.5 n) 1) " V") 'small 'bold)
(text 160 228 "each point's voltage from ground" 'centre 'small)
```

```check
(number
  (ask "Four 1.5 V cells are stacked + to -. What is the voltage between the top and ground, in volts?")
  (answer 6) (from "4 * 1.5")
  (wrong-if 1.5 "That is one cell. Stacked + to -, the voltages of the cells add.")
  (wrong-if 5.5 "Each cell adds 1.5 V: 1.5 + 1.5 + 1.5 + 1.5 is 6.")
  (hint "Use the slider: set it to 4 cells."))
```

---

```check
(number
  (ask "With four cells stacked, the top of the third cell is at 4.5 V and the top of the first is at 1.5 V. What is the voltage between those two points, in volts?")
  (answer 3) (from "4.5 - 1.5")
  (wrong-if 4.5 "4.5 V is measured from ground. Between the two points, take the difference: 4.5 - 1.5.")
  (wrong-if 6 "That adds them. The voltage between two points is their difference.")
  (hint "A voltage between two points is the difference of their voltages from ground."))
```

---

```check
(choice
  (ask "Someone says a wire \"is at 5 V\". What do they mean?")
  (right "There are 5 V between the wire and ground")
  (wrong "5 V flows along the wire"
    "Voltage does not flow: current does. A voltage is always between two points, and a single figure means between the wire and ground.")
  (wrong "The wire can take at most 5 V"
    "It says what the wire's voltage is now, measured from ground, not a limit.")
  (hint "A voltage is between two points. Which is the second one, when nobody names it?"))
```

So voltage is the push that drives a current, measured in volts between
two points, and ground is the point called 0 V.
