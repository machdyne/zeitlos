# The transistor as a switch

```lesson
(objective "Say how a transistor works as a switch, and how two of them make an inverter.")
(minutes 20)
(keywords "transistor" "MOSFET" "gate (of a transistor)" "source (of a transistor)" "drain (of a transistor)" "bipolar transistor" "n-type (transistor)" "p-type (transistor)" "inverter (circuit)" "CMOS" "open-drain (output)")
```

You will learn the part that every computer is built from.

A **transistor** is a switch worked by electricity. The kind in
digital chips is the **MOSFET**. A voltage on one of its connections,
the **gate**, decides whether current can flow between two others, the
**source** and the **drain**.

It has no moving parts, and can switch on and off billions of times a
second without wearing out.

Another kind, the **bipolar transistor**, is worked instead by a small
current into one of its connections. On circuit boards it often
switches larger loads, such as motors.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: The Transistor as a Switch")
```

```check
(choice
  (ask "In a MOSFET, what decides whether current flows between the source and the drain?")
  (right "The voltage on the gate")
  (wrong "A small lever inside it"
    "A transistor has no moving parts. A voltage on its gate switches it, which is why it can switch billions of times a second.")
  (wrong "The current through the gate"
    "That is how a bipolar transistor is worked. A MOSFET is worked by the voltage on its gate.")
  (hint "Which connection is named after a door?"))
```

---

## Two kinds

MOSFETs come in two forms that work in opposite ways:

- an **n-type** transistor conducts when its gate is high (near the
  supply voltage);
- a **p-type** transistor conducts when its gate is low (near ground).

```check
(choice
  (ask "The gate of a p-type transistor is high. Does it conduct?")
  (right "No")
  (wrong "Yes"
    "A p-type transistor conducts when its gate is low. With a high gate it is off. It is the n-type that conducts with a high gate.")
  (hint "p-type: conducts when the gate is low."))
```

---

## The inverter

Put one of each in a line between the supply and ground, with their
gates joined as the input. The point between them is the output.
Change the input and see which transistor conducts.

```figure
(define hi (toggle 'in "Input high"))
(text 190 22 "3.3 V" 'small 'centre)
(line 170 28 210 28 'ink 1.5)
(line 190 28 190 50 'ink 1.5)
(el-switch 190 50 190 95 (not hi))
(line 190 95 190 140 'ink 1.5)
(el-switch 190 140 190 185 hi)
(line 190 185 190 196 'ink 1.5)
(el-ground 190 202)
(text 182 76 "p-type" 'small 'right)
(text 182 166 "n-type" 'small 'right)
(dot 190 118)
(path '(190 118 280 118) 'ink 1.5)
(text 60 122 "input" 'small 'centre)
(path '(84 118 100 118 100 72 140 72) 'cool 1.5)
(path '(100 118 100 162 140 162) 'cool 1.5)
(text 280 108 (if hi "output: low" "output: high") 'small 'right 'bold)
(text 60 140 (if hi "high" "low") 'small 'centre 'bold)
```

When the input is low, the top transistor conducts and the bottom one
does not: the output is joined to the supply, so it is high. When the
input is high, the output is joined to ground: low. The output is
always the opposite of the input. This is an **inverter**.

```check
(choice
  (ask "The inverter's input is high. Which transistor conducts, and what is the output?")
  (right "The n-type; the output is low")
  (wrong "The p-type; the output is high"
    "A p-type conducts when its gate is low. With a high input the n-type conducts, joining the output to ground: low.")
  (wrong "Both; the output is half-way"
    "Only one conducts at a time: that is the point of having one of each. With a high input, it is the n-type.")
  (hint "Toggle the input in the figure."))
```

---

## CMOS

This way of pairing the two kinds is called **CMOS**. In either state
one transistor is off, so almost no current flows from the supply to
ground. A CMOS chip uses power mainly while it switches: a chip that
switches less often uses less, and an idle one draws very little.

The circuits in the units after this one, and every circuit inside an
FPGA, are built from transistors arranged like these.

```check
(choice
  (ask "When does a CMOS chip use most of its power?")
  (right "While its transistors are switching")
  (wrong "While it sits still, holding a high output"
    "Holding still, one transistor of each pair is off, so almost no current flows from the supply to ground.")
  (wrong "All the time, the same amount"
    "An idle CMOS chip draws very little. Its power goes mainly into switching.")
  (hint "In either steady state, one transistor of each pair is off."))
```

---

## Open-drain outputs

An **open-drain** output has only the n-type transistor. It can join
its wire to ground, making it low, or let go of it. A pull-up resistor
makes the wire high when nothing pulls it low.

Several open-drain outputs can share one wire. If any one pulls low,
the wire is low. Since none ever drives the wire high, two of them can
never fight.

```check
(choice
  (ask "Three open-drain outputs share a wire with a pull-up. Two let go; one pulls low. What is the wire?")
  (right "Low")
  (wrong "High, because most of them let go"
    "It is not a vote. Any one pulling low joins the wire to ground, and the pull-up only lets a small current through.")
  (wrong "A fight: the outputs are damaged"
    "Open-drain outputs never drive high, so they cannot fight. The one pulling low wins safely.")
  (hint "What does one output pulling low do to the wire?"))
```

So a transistor is a switch worked by a voltage, and a p-type and an
n-type together make an inverter, the simplest CMOS circuit.
