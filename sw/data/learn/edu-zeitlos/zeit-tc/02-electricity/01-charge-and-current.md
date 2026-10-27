# Charge and current

```lesson
(objective "Say what electric current is, measure it in amps and milliamps, and tell when a circuit carries it.")
(minutes 20)
(keywords "atom (of matter)" "nucleus (of an atom)" "electron (particle)" "charge (electric)" "conductor (electric)" "insulator (electric)" "current (electric)" "ampere (unit)" "amp (unit)" "milliamp (unit)" "terminal (of a battery)" "circuit (electric)" "conventional current" "loop (of a circuit)" "switch (electrical)")
```

A computer is an electrical machine. You will learn what moves in its
wires, and how much of it there is.

All matter is made of **atoms**. An atom has a small, heavy centre, its
**nucleus**, with a positive electric **charge**. Around it are much
lighter **electrons**, each with a negative charge.

Opposite charges attract each other; like charges push each other away.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Charge and Current")
```

```check
(choice
  (ask "Two electrons are close together. What do they do?")
  (right "Push each other away")
  (wrong "Pull towards each other"
    "Both electrons have a negative charge. Like charges push apart; only opposite charges, negative and positive, attract.")
  (wrong "Nothing: charges do not act on each other"
    "Charges always act on each other: opposite ones attract and like ones push apart.")
  (hint "Are their charges alike or opposite?"))
```

---

## Conductors and insulators

In some materials, mostly metals, the outer electrons of each atom are
held loosely and can drift from atom to atom. These are
**conductors**. In others, such as glass, plastic and dry air, the
electrons are held tightly. These are **insulators**.

A wire is a conductor, usually copper, wrapped in an insulator, usually
plastic.

```check
(choice
  (ask "Which of these is a conductor?")
  (right "A copper wire")
  (wrong "The plastic coat of a wire"
    "Plastic holds its electrons tightly, so it is an insulator. That is why it is wrapped round the wire: to keep the charge inside.")
  (wrong "A glass window"
    "Glass is an insulator: its electrons are held tightly and cannot drift.")
  (wrong "Dry air"
    "Dry air is an insulator. If it were not, charge would leak from every wire into the air around it.")
  (hint "Conductors are mostly metals."))
```

---

## Current

When electrons drift along a conductor in one direction, charge flows.
That flow is **current**. It is measured in **amperes** (A), usually
called **amps**.

One amp is a large current in electronics. A chip's pin can usually
give only a few thousandths of an amp: **milliamps** (mA).

$$ 1 mA = 0.001 A $$

```check
(number
  (ask "A current is 5 mA. How many amps is that?")
  (answer 0.005) (from "5 / 1000")
  (wrong-if 5000 "That multiplies by a thousand. A milliamp is a thousandth of an amp, so divide by 1000.")
  (wrong-if 0.05 "That is 50 mA. Divide 5 by 1000: the 5 moves three places right of the point.")
  (hint "A milliamp is a thousandth of an amp.")
  (hint "5 / 1000."))
```

---

## A circuit

Charge flows only round a complete **loop**: from a battery, through the
wires and parts, and back to the battery. That loop is a **circuit**.
A battery has two **terminals**, its connection points, marked + and -.

A **switch** opens or closes a gap in the loop. Close the switch in
the figure. The loop is complete, current flows,
and the lamp lights. Open it, and the loop is broken: no current flows
anywhere in it.

```figure
(define on (toggle 'sw "Switch closed"))
(path '(60 70 120 70) 'ink 1.5)
(el-switch 120 70 170 70 on)
(path '(170 70 260 70 260 108) 'ink 1.5)
(path '(260 132 260 170 60 170 60 160) 'ink 1.5)
(path '(60 80 60 70) 'ink 1.5)
(el-cell 60 80 160 "battery")
(el-lamp 260 120 on)
(text 290 124 "lamp" 'small)
(if on (begin (arrow 195 60 235 60 'ink 1) (arrow 215 180 175 180 'ink 1)
              (text 215 52 "current" 'tiny 'centre)))
(text 160 215 (if on "Closed: current flows, the lamp lights."
                  "Open: no current, the lamp is dark.") 'centre 'small)
```

```check
(choice
  (ask "The switch is open. Which part of the circuit carries current?")
  (right "None of it")
  (wrong "The wire from the battery's + terminal to the switch"
    "Current flows only round a complete loop. A break anywhere stops it everywhere, even in the wire before the break.")
  (wrong "The lamp, but too little to light it"
    "With the loop broken, no current flows at all, through the lamp or anywhere else.")
  (hint "Is the loop complete?"))
```

---

## Which way?

For historical reasons, current is said to flow out of the + terminal,
round the circuit, and back into the - terminal. The electrons really
drift the other way. Circuit diagrams use this
**conventional current**, and so does this course. For every circuit here, the
difference does not matter.

```check
(choice
  (ask "Conventional current flows from the + terminal, through a lamp, to the - terminal. Which way do the electrons drift?")
  (right "From the - terminal, through the lamp, to the + terminal")
  (wrong "From the + terminal, through the lamp, to the - terminal"
    "That is the direction of conventional current. Electrons have a negative charge and drift the opposite way.")
  (wrong "They do not move; only the current does"
    "Current is charge moving, and in a wire the charges that move are electrons. They drift opposite to conventional current.")
  (hint "Electrons are negative, and are drawn towards the + terminal."))
```

---

```check
(number
  (ask "A circuit carries 0.02 A. How many milliamps is that?")
  (answer 20) (from "0.02 * 1000")
  (wrong-if 2 "That is 0.002 A. Multiply 0.02 by 1000: 20.")
  (wrong-if 0.00002 "That divides. To go from amps to milliamps, multiply by 1000: there are a thousand milliamps in an amp.")
  (hint "There are 1000 milliamps in one amp."))
```

So current is charge flowing round a closed circuit, measured in amps
or, in electronics, milliamps.
