# Driving pins

```lesson
(objective "Make a GPIO pin an output, drive it high and low from the REPL, and light an LED through the right resistor.")
(minutes 20)
```

To drive a pin, the board makes it an output and holds it at a level,
high or low. Two commands do it, here for port 0,
bit 3 (PMOD pin 4):

```
> gpio-mode 0 3 out
> gpio-set 0 3 1
```

The first makes bit 3 an output; the second drives it high. `gpio-set 0
3 0` drives it low again, and `gpio-toggle 0 3` turns it over.

```sources
(source "Zeitlos docs: gpio.md, From Scheme")
```

```check
(choice
  (ask "You type gpio-set 0 5 1, but you never typed gpio-mode for bit 5. Is the board now driving PMOD pin 8 high?")
  (right "No: bit 5 is still an input, so nothing drives the pin")
  (wrong "Yes: gpio-set makes the pin an output by itself"
    "gpio-set only sets the level the pin will have once it is an output. The direction is gpio-mode's job.")
  (wrong "Yes, because the pull-up makes it high anyway"
    "It may read high from the pull-up, but the board is not driving it: a load on it would pull it down.")
  (hint "Which command makes a pin an output?"))
```

---

## 0 means low

In `gpio-set`, `1` (or `#t`) means high and `0` (or `#f`) means low.
It answers with the pin as it really is, not with what you asked. So if
you forget `gpio-mode`, `gpio-set 0 3 0` answers `#t`: the pin is still
an input, nothing drives it low, and the pull-up holds it high.

```check
(text
  (ask "Bit 2 of port 0 is an output. Type the command that drives it low.")
  (accept "gpio-set 0 2 0") (accept "gpio-set 0 2 #f")
  (accept "(gpio-set 0 2 0)") (accept "(gpio-set 0 2 #f)")
  (wrong-if "gpio-set 0 2 1" "1 means high. Low is 0.")
  (wrong-if "gpio-set 2 0 0" "The port comes first, then the bit: gpio-set PORT BIT LEVEL.")
  (hint "gpio-set, then the port, the bit and the level.")
  (hint "Port 0, bit 2, level 0."))
```

---

## An LED

A pin driven high gives 3.3 V. An LED needs a resistor in series, or it
draws far more current than it or the pin can take. A red LED drops
about 2 V, so the resistor has the other 1.3 V across it, and Ohm's law
gives the current. Move the slider.

```figure
(define r (slider 'r 150 1000 270 "Resistor (ohms)" 10))
(define ma (* 1000 (/ 1.3 r)))
(text 20 56 "pin, 3.3 V" 'small)
(line 20 70 80 70 'ink 1.5)
(hw-resistor 80 170 70 (string-append (fmt r 0) " ohms"))
(poly '(185 58 185 82 205 70) 'ink)
(line 205 58 205 82 'ink 2)
(text 195 100 "LED" 'centre 'small)
(line 170 70 185 70 'ink 1.5)
(line 205 70 240 70 'ink 1.5)
(line 240 70 240 110 'ink 1.5)
(hw-ground 240 110)
(text 160 160 (string-append "I = 1.3 V / " (fmt r 0) " ohms = " (fmt ma 1) " mA") 'centre 'body 'bold)
(caption "Current through the resistor and the LED")
```

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Resistance and Ohm's Law; Talking to the World: General-Purpose Pins")
```

```warn
(warn "Never an LED without its resistor" "An LED straight from a pin to ground draws far more current than the pin can supply, and one of them is damaged. Always put the resistor in series, and wire it with the power off.")
```

```check
(number
  (ask "You use a 330 ohm resistor with a red LED on a pin at 3.3 V. About how many milliamps flow? (One decimal place.)")
  (answer 3.9) (within 0.1) (unit "mA")
  (from "1000 * 1.3 / 330")
  (wrong-if 10 "That is 3.3 V / 330 ohms: the whole 3.3 V. The LED takes about 2 V, so the resistor has only 1.3 V.")
  (hint "The resistor has 3.3 - 2.0 = 1.3 V across it.")
  (hint "I = 1.3 / 330 amps. Multiply by 1000 for milliamps."))
```

---

## Small loads only

A pin can supply a few milliamps: enough for an LED through its
resistor, not for a motor, a relay or a lamp. Those need a transistor or
a driver chip that the pin switches, and a coil needs its flyback diode.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: General-Purpose Pins")
```

```check
(choice
  (ask "You want a GPIO pin to run a small 3 V motor. How do you connect it?")
  (right "The pin switches a transistor or a driver chip, which switches the motor's current")
  (wrong "Straight from the pin to the motor and ground"
    "A motor needs far more current than a pin can give: the pin could be damaged.")
  (wrong "From the pin through a resistor, as for an LED"
    "A resistor limits the current, so the motor gets too little to turn. A driver gives it the current from the supply instead.")
  (hint "A pin can give a few milliamps. Who gives the motor the rest?"))
```

---

## Practice

```example
(example
  (problem "Light an LED on PMOD pin 7 of port 0.")
  (step "With the power off, wire pin 7 to a 270 ohm resistor, the resistor to the LED's longer leg, and its other leg to a GND pin.")
  (step "Power on, open term from the dock and choose REPL.")
  (step (choice (ask "Pin 7 is which bit?") (right "Bit 4")
          (wrong "Bit 7" "Bit 7 is pin 10. Pin 7 starts the second row: bit 4.")
          (wrong "Bit 6" "Bit 6 is pin 9. Pin 7 is the first pin of the second row, bit 4.")))
  (step "Type gpio-mode 0 4 out, then gpio-set 0 4 1.")
  (answer "The LED lights; gpio-set 0 4 0 puts it out."))
```

```check
(text
  (ask "Type the command that makes bit 4 of port 0 an output.")
  (accept "gpio-mode 0 4 out") (accept "(gpio-mode 0 4 \"out\")")
  (wrong-if "gpio-set 0 4 1" "That sets the level. The direction comes first, with gpio-mode.")
  (wrong-if "gpio-mode 0 7 out" "That is bit 7, pin 10. Bit 4 is the one asked for.")
  (hint "gpio-mode PORT BIT out."))
```

So: `gpio-mode` makes a pin an output, `gpio-set` drives it high or
low, and an LED on it always has its resistor.
