# Capacitors and other components

```lesson
(objective "Say what the common components do, work out a time constant, and choose the resistor for an LED.")
(minutes 25)
(keywords "component (electronic)" "capacitor" "capacitance" "farad (unit)" "microfarad (unit)" "nanofarad (unit)" "picofarad (unit)" "millisecond (unit)" "time constant" "decoupling capacitor" "inductor" "diode" "light-emitting diode" "LED" "crystal (electronic)" "integrated circuit")
```

You will meet the parts that nearly every circuit board is made of, and
use Ohm's law to choose a part for one of them.

The parts of a circuit are its **components**. A **capacitor** stores
charge: the more voltage across it, the more charge it holds. How much
it holds for each volt is its **capacitance**, measured in **farads**
(F). On a circuit board they range from a few **picofarads** (pF,
millionths of a millionth of a farad) to a few thousand **microfarads**
(uF, millionths). A **nanofarad** (nF) is a thousandth of a microfarad.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Capacitors and Time; Components")
```

```check
(number
  (ask "How many nanofarads make 1 microfarad?")
  (answer 1000)
  (wrong-if 1000000 "A million is how many picofarads make a microfarad. A nanofarad is a thousandth of a microfarad.")
  (hint "A nanofarad is a thousandth of a microfarad."))
```

---

## Charging takes time

Charge a capacitor through a resistor, and its voltage does not jump to
the supply's. As it rises, the current through the resistor falls, so
it rises ever more slowly.

The **time constant**, R times C, sets the pace. After one time
constant the capacitor reaches about 63 percent of the supply voltage;
after five, it is within 1 percent of it. Here, 10k times 100 nF:

$$ 10000 * 0.0000001 = 0.001 $$

seconds, which is 1 ms (a **millisecond**).

```figure
(define t (slider 't 0 5 1 "Time (ms)" 0.1))
(define f (- 1 (exp (- t))))
(graph 50 30 240 150 0 5 0 4)
(graph-axes 1 1 "ms" "V")
(line (gx 0) (gy 3.3) (gx 5) (gy 3.3) 'grey 1)
(text (gx 5) (- (gy 3.3) 5) "supply 3.3 V" 'tiny 'right)
(plot (lambda (x) (* 3.3 (- 1 (exp (- x))))) 'ink)
(plot-point t (* 3.3 f) 'warm)
(text 160 222 (string-append (fmt t 1) " ms: " (fmt (* 3.3 f) 2) " V, "
                             (fmt (* 100 f) 0) "% of the supply") 'centre 'small)
```

```check
(number
  (ask "A 10k resistor charges a 1 uF capacitor. What is the time constant, in milliseconds?")
  (answer 10) (from "10000 * 0.000001 * 1000")
  (wrong-if 0.01 "That is the time constant in seconds. Multiply by 1000 for milliseconds.")
  (wrong-if 10000 "That forgets the capacitor is a millionth of a farad. 10,000 times 0.000001 is 0.01 s.")
  (hint "R times C, in ohms and farads, gives seconds: 10,000 times 0.000001.")
  (hint "That is 0.01 s. How many milliseconds?"))
```

---

```check
(number
  (ask "The supply is 3.3 V. About what voltage does the capacitor reach after one time constant? Use the figure, or 63 percent.")
  (answer 2.08) (within 0.05) (from "3.3 * 0.63")
  (wrong-if 3.3 "That is the supply. After one time constant the capacitor has only reached about 63 percent of it.")
  (wrong-if 1.65 "That is half. After one time constant it is about 63 percent: 3.3 times 0.63.")
  (hint "63 percent of 3.3 V: 3.3 times 0.63."))
```

---

## What capacitors are for

A resistor and capacitor on one of a chip's pins can hold the chip
waiting for a few milliseconds after it is switched on, until the
supply is steady.

Capacitors placed right next to a chip give it the brief bursts of
current it needs each time it switches, and quieten the noise on its
supply. These are **decoupling capacitors**, and nearly every chip needs
them.

```check
(choice
  (ask "Why is there a small capacitor right beside nearly every chip?")
  (right "It gives the chip brief bursts of current, and quietens the supply's noise")
  (wrong "It stores the chip's program"
    "A capacitor holds a little charge, not a program. Beside a chip it supplies the bursts of current the chip needs when it switches.")
  (wrong "It limits the chip's current, like a resistor"
    "That is a resistor's job. A decoupling capacitor does the opposite: it gives current in quick bursts.")
  (hint "What does a capacitor store?"))
```

---

## The other components

- **Inductors** store energy in a magnetic field and resist changes in
  current. They are mostly found in power supplies.
- **Diodes** let current flow one way only. A **light-emitting diode**,
  or **LED**, is a diode that gives off light when current flows.
- **Crystals** vibrate at a precise rate, and give a computer the
  steady beat its steps keep time to.
- An **integrated circuit** is a chip: a complete circuit, sometimes of
  billions of tiny parts, made on one piece of silicon.

```check
(choice
  (ask "Which component lets current through in one direction only?")
  (right "A diode")
  (wrong "An inductor"
    "An inductor resists changes in current, in either direction. A diode is the one-way part.")
  (wrong "A capacitor"
    "A capacitor stores charge. A diode lets current flow one way only.")
  (hint "An LED is one kind of it."))
```

---

## A resistor for an LED

A chip's output pin gives 3.3 V when it is on. A red LED needs about
2 V across it to light, and is bright enough at 5 mA. A resistor in
the same loop takes the rest of the voltage, and sets the current.

```figure
(define k (choice 'r '("none" "100" "270" "1000") "Resistor (ohms)"))
(define r (list-ref '(0 100 270 1000) k))
(text 20 64 "pin" 'small)
(text 20 80 "3.3 V" 'small)
(path '(60 70 90 70) 'ink 1.5)
(if (= r 0) (path '(90 70 160 70) 'ink 1.5) (el-res 90 70 160 70 (string-append (fmt r 0) " ohms")))
(el-led 160 70 240 #t)
(path '(240 70 270 70 270 110) 'ink 1.5)
(el-ground 270 116)
(if (= r 0)
    (begin (text 160 160 "no resistor: far more current" 'centre 'small 'bold)
           (text 160 180 "than the LED or the pin can take" 'centre 'small 'bold))
    (begin (text 160 150 "resistor: 3.3 - 2.0 = 1.3 V" 'centre 'small)
           (text 160 175 (string-append "I = 1.3 / " (fmt r 0) " = " (el-ma (/ 1.3 r))) 'centre 'body 'bold)))
```

```example
(example
  (problem "Choose a resistor for a red LED, 2 V at 5 mA, on a 3.3 V pin.")
  (step "The resistor takes the rest of the voltage: 3.3 - 2.0 = 1.3 V.")
  (step "R = V / I = 1.3 / 0.005 = 260 ohms.")
  (step "Resistors come in standard values. Take the next one above: 270 ohms.")
  (answer "270 ohms."))
```

```check
(number
  (ask "Same pin and LED, but you want 10 mA. What resistance, in ohms?")
  (answer 130) (from "(3.3 - 2) / 0.01")
  (wrong-if 330 "That uses the whole 3.3 V. The LED takes 2 V of it, so the resistor has only 1.3 V.")
  (wrong-if 200 "That uses the LED's 2 V. The resistor gets the rest: 3.3 - 2 = 1.3 V.")
  (hint "The resistor's voltage is 3.3 - 2 = 1.3 V.")
  (hint "R = 1.3 / 0.01."))
```

---

```check
(choice
  (ask "What happens if the LED is joined to the pin with no resistor?")
  (right "Far too much current flows, and the LED or the pin is damaged")
  (wrong "The LED is brighter, and nothing else changes"
    "Nothing then limits the current. It rises far beyond what the LED or the pin can take, and one of them is damaged.")
  (wrong "No current flows, because the loop is not complete"
    "The loop is complete: pin, LED, ground. The resistor is what limits the current; without it, far too much flows.")
  (hint "What was the resistor doing to the current?"))
```

So capacitors store charge and take time to charge, diodes, inductors,
crystals and chips each have their job, and a resistor sets an LED's
current by Ohm's law.
