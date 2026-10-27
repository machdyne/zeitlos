# High and low

```lesson
(objective "Say whether a voltage reads as high or low, and why an input needs a pull-up resistor.")
(minutes 20)
(keywords "digital (electronics)" "signal (electrical)" "high (logic level)" "low (logic level)" "logic level" "floating (input)" "pull-up resistor" "pull-down resistor")
```

You will learn how a computer's wires hold information with only two
kinds of voltage.

**Digital** circuits, the kind a computer is made of, use voltages to
stand for just two values: **high** and **low**. High stands for 1 and
low for 0. A voltage that carries information like this is a
**signal**, and its two values are its **logic levels**: high and low.

For chips that run on 3.3 V, an input reads **low** below about 0.8 V
and **high** above about 2 V. In between, what it reads is not defined,
and a well-made circuit spends almost no time there.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Logic Levels")
```

```figure
(define v (slider 'v 0 3.3 2.9 "Input (V)" 0.1))
(define (y u) (- 200 (* 50 u)))
(rect 70 (y 2) 40 (- (y 0.8) (y 2)) 'grey)
(frame 70 (y 3.3) 40 (- (y 0) (y 3.3)) 'ink 1)
(text 64 (+ (y 0) 4) "0 V" 'small 'right)
(text 64 (+ (y 0.8) 4) "0.8 V" 'small 'right)
(text 64 (+ (y 2) 4) "2 V" 'small 'right)
(text 64 (+ (y 3.3) 4) "3.3 V" 'small 'right)
(text 120 (y 2.7) "reads high" 'small)
(text 120 (y 1.4) "not defined" 'small)
(text 120 (y 0.35) "reads low" 'small)
(line 66 (y v) 114 (y v) 'ink 3)
(text 290 120 (if (> v 2) "high" (if (< v 0.8) "low" "?")) 'centre 'big 'bold)
(text 290 140 (string-append (fmt v 1) " V") 'centre 'small)
```

```check
(choice
  (ask "A 3.3 V chip's input is at 1.2 V. What does it read?")
  (right "Nothing defined: it could read either")
  (wrong "Low, because 1.2 V is less than half of 3.3 V"
    "There is no half-way rule. Low means below about 0.8 V, high above about 2 V, and 1.2 V is in the gap between.")
  (wrong "High, because there is some voltage"
    "Any voltage is not enough: high means above about 2 V. 1.2 V is in the gap where the reading is not defined.")
  (hint "Put the slider at 1.2 V."))
```

---

## Why the gap helps

Because high and low are far apart, a signal can pick up some noise, or
lose some voltage along a long wire, and still be read correctly. A
stored number does not slowly drift, as a recording on tape does. Each
part of a circuit passes on a clean high or low again.

```check
(choice
  (ask "A high of 3.3 V loses 0.6 V along a long wire, and arrives at 2.7 V. What does the input read?")
  (right "High" (true "3.3 - 0.6 > 2"))
  (wrong "Low" (false "3.3 - 0.6 < 0.8")
    "Low is below about 0.8 V. 2.7 V is well above 2 V, so it still reads high: that is what the gap is for.")
  (wrong "Nothing defined" (false "3.3 - 0.6 < 2")
    "The gap is from 0.8 V to 2 V. 2.7 V is above it, so it reads high.")
  (hint "Is 2.7 V above 2 V?"))
```

---

## 5 V and 3.3 V

Chips of different families use different levels. Older chips and many
hobby boards use 5 V; FPGA pins usually use 3.3 V or less.

```warn
(warn "5 V into 3.3 V" "Never connect a 5 V output to a 3.3 V input unless the input is documented as able to take 5 V. The extra voltage can destroy the input, and sometimes the whole chip. A pair of resistors or a level-shifter chip converts between the two.")
```

```check
(choice
  (ask "A hobby board's 5 V output must drive an FPGA's 3.3 V input. What do you do?")
  (right "Put a level shifter, or a pair of resistors, between them")
  (wrong "Connect them: 5 V is clearly high, so it reads correctly"
    "It would read high for a moment, but the extra voltage can destroy the input, or the whole FPGA. Convert the level first.")
  (wrong "Connect them through a long wire, to lose some voltage"
    "A wire loses only a little, and not reliably. Use a level shifter or a pair of resistors.")
  (hint "What can 5 V do to a 3.3 V input?"))
```

---

## Floating inputs

An input pin connected to nothing is **floating**. It picks up stray
charge, and may read high, low, or flip between them. A button between
a pin and ground has the same problem while it is not pressed.

The cure is a **pull-up resistor**, often 2.2k to 10k, from the pin to
the supply. When nothing else drives the pin, the resistor pulls it
high. When the button joins the pin to ground, the pin reads low, and
the resistor lets only a small current through.

```figure
(define pu (toggle 'pu "Pull-up" #t))
(define pr (toggle 'pr "Pressed"))
(text 120 26 "3.3 V" 'small 'centre)
(line 100 32 140 32 'ink 1.5)
(if pu (el-res 120 32 120 92 "10k"))
(line 120 92 120 120 'ink 1.5)
(dot 120 104)
(path '(120 104 210 104) 'ink 1.5)
(frame 210 92 60 24 'ink 1)
(text 240 108 "pin" 'small 'centre)
(el-switch 120 120 120 165 pr)
(line 120 165 120 180 'ink 1.5)
(el-ground 120 186)
(text 160 220 (cond (pr (if pu "low: 0 V; 0.33 mA flows" "low: 0 V"))
                    (pu "high: 3.3 V; no current")
                    (else "floating: may read either")) 'centre 'small 'bold)
```

```check
(choice
  (ask "With the pull-up fitted and the button pressed, what does the pin read?")
  (right "Low")
  (wrong "High, because the resistor joins it to 3.3 V"
    "The button joins the pin straight to ground. The resistor only lets a small current through, so it cannot hold the pin up: it reads low.")
  (wrong "Floating"
    "A pin is floating only when nothing drives it. Here the button joins it to ground.")
  (hint "Toggle both in the figure."))
```

---

```check
(number
  (ask "A 4.7k pull-up to 3.3 V. The button is pressed. What current flows through the resistor, in milliamps?")
  (answer 0.70) (within 0.01) (from "3.3 / 4700 * 1000")
  (wrong-if 0.33 "That is for 10k. With 4.7k: 3.3 / 4700.")
  (wrong-if 0 "Pressed, the resistor has 3.3 V across it, from the supply to ground, so a current flows: 3.3 / 4700.")
  (hint "The whole 3.3 V is across the resistor. I = V / R.")
  (hint "3.3 / 4700 amps; times 1000 for milliamps."))
```

---

A **pull-down resistor** works the same way, towards ground: with
nothing else driving it, the pin reads low.

```check
(choice
  (ask "A pin has a pull-down resistor, and a button between it and 3.3 V. The button is not pressed. What does the pin read?")
  (right "Low")
  (wrong "High"
    "Nothing joins the pin to 3.3 V while the button is up, and the pull-down holds it at ground: low.")
  (wrong "Floating"
    "The pull-down is there to stop it floating: it holds the pin low.")
  (hint "Where does a pull-down pull?"))
```

So a digital input reads high above about 2 V and low below about
0.8 V, and a pull-up or pull-down keeps it from floating.
