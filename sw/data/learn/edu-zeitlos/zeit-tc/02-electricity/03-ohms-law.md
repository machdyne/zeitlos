# Resistance and Ohm's law

```lesson
(objective "Use Ohm's law to work out a current, a voltage or a resistance from the other two.")
(minutes 20)
(keywords "resistance (electrical)" "ohm (unit)" "resistor" "Ohm's law" "kilohm (unit)" "megohm (unit)" "microamp (unit)" "millivolt (unit)")
```

You will learn the one rule that ties voltage, current and resistance
together.

Every material holds back current to some degree. That is its
**resistance**, measured in **ohms**. A **resistor** is a part made to
have a particular resistance.

**Ohm's law** relates the three. With V the voltage in volts, I the
current in amps and R the resistance in ohms:

$$ V = I * R $$

$$ I = V / R $$

$$ R = V / I $$

These are one rule, written three ways.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Resistance and Ohm's Law")
```

```check
(choice
  (ask "You know the voltage across a resistor and its resistance. Which form gives the current?")
  (right "I = V / R")
  (wrong "I = V * R"
    "Multiplying gives a bigger current for a bigger resistance, but resistance holds current back. Divide: I = V / R.")
  (wrong "I = R / V"
    "That would give less current for more voltage, but a bigger push drives more current. I = V / R.")
  (hint "More voltage, more current; more resistance, less current."))
```

---

## Try it

The figure is a battery and a resistor. The current is worked out by
Ohm's law, I = V / R. Change the voltage and the resistor and watch it.

```figure
(define v (slider 'v 0 12 6 "Battery (V)" 0.5))
(define k (choice 'r '("100" "220" "470" "1000") "Resistor (ohms)"))
(define r (list-ref '(100 220 470 1000) k))
(define i (/ v r))
(path '(60 60 120 60) 'ink 1.5)
(el-res 120 60 200 60 (string-append (fmt r 0) " ohms"))
(path '(200 60 270 60 270 150 60 150 60 140) 'ink 1.5)
(path '(60 70 60 60) 'ink 1.5)
(el-cell 60 70 140 (string-append (fmt v 1) " V"))
(if (> v 0) (begin (arrow 250 90 250 125 'ink 1) (text 244 110 "current" 'tiny 'right)))
(text 160 190 (string-append "I = " (fmt v 1) " / " (fmt r 0)) 'centre 'body)
(text 160 215 (string-append "= " (fmt i 4) " A = " (el-ma i)) 'centre 'body 'bold)
```

```check
(number
  (ask "9 V across a 1000 ohm resistor. What is the current, in milliamps?")
  (answer 9) (from "9 / 1000 * 1000")
  (wrong-if 0.009 "That is the current in amps. In milliamps, multiply by 1000.")
  (wrong-if 9000 "That multiplies 9 by 1000. Ohm's law divides: I = V / R.")
  (hint "I = V / R gives amps. Then multiply by 1000 for milliamps."))
```

---

## Double and half

Since I = V / R, doubling the voltage across a resistor doubles the
current through it. Doubling the resistance halves the current.

```check
(choice
  (ask "A resistor carries 4 mA. You swap it for one with twice the resistance, at the same voltage. What is the current now?")
  (right "2 mA")
  (wrong "8 mA"
    "More resistance holds the current back more, so it falls. Twice the resistance gives half the current.")
  (wrong "4 mA"
    "The voltage is the same, but the resistance is not. By I = V / R, twice the R gives half the I.")
  (hint "Does more resistance mean more current, or less?"))
```

---

## Big and small

Large resistances use prefixes. 1,000 ohms is 1 **kilohm**, written
1k; 1,000,000 ohms is 1 **megohm**, written 1M.

Small values use other prefixes, on any unit: a **millivolt** is a
thousandth of a volt, a **microamp** a millionth of an amp.

```check
(number
  (ask "A resistor is 4.7k. How many ohms is that?")
  (answer 4700) (from "4.7 * 1000")
  (wrong-if 47 "k means a thousand: 4.7 times 1000 is 4700.")
  (wrong-if 470 "That is 0.47k. 4.7 times 1000 is 4700.")
  (hint "k means times 1000."))
```

---

## Finding a resistance

```example
(example
  (problem "A 5 V supply should drive 10 mA through a resistor. What resistance is needed?")
  (step "Use R = V / I, with the current in amps: 10 mA is 0.01 A.")
  (step "R = 5 / 0.01.")
  (step (number (ask "What is R, in ohms?") (answer 500) (from "5 / 0.01")
          (wrong-if 0.5 "That divides by 10, not by 0.01. Turn 10 mA into amps first: 0.01 A.")))
  (answer "500 ohms."))
```

```check
(number
  (ask "3 V should drive 2 mA through a resistor. What resistance, in ohms?")
  (answer 1500) (from "3 / 0.002")
  (wrong-if 1.5 "That divides by 2, using milliamps as if they were amps. 2 mA is 0.002 A.")
  (wrong-if 0.006 "That multiplies. R = V / I.")
  (hint "Turn 2 mA into amps first: 0.002 A.")
  (hint "R = 3 / 0.002."))
```

---

```check
(number
  (ask "A current of 0.02 A flows through a 150 ohm resistor. What is the voltage across it, in volts?")
  (answer 3) (from "0.02 * 150")
  (wrong-if 7500 "That divides 150 by 0.02. The voltage is current times resistance: V = I * R.")
  (wrong-if 300 "That treats 0.02 A as 2. 0.02 times 150 is 3.")
  (hint "V = I * R."))
```

So Ohm's law, V = I * R, gives any one of voltage, current and
resistance from the other two.
