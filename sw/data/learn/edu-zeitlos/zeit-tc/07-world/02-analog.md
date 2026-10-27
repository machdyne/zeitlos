# Analog signals

```lesson
(objective "Turn an ADC reading into a voltage, and say how pulse-width modulation makes an in-between level from a digital pin.")
(minutes 20)
(keywords "analog" "analog-to-digital converter" "ADC" "digital-to-analog converter" "DAC" "pulse-width modulation" "PWM" "duty cycle" "microcontroller")
```

A GPIO input only says high or low. Many things in the world vary
smoothly instead, with every value in between: temperature, light,
sound, the position of a knob. A signal that varies smoothly like this
is **analog**.

An **analog-to-digital converter**, or **ADC**, measures a voltage and
reports it as a number. A 10-bit ADC divides its range into 1,024
steps. With a range of 3.3 V, each step is about 3.2 millivolts, and a
reading of 512, half of 1,024, means about 1.65 V: half of 3.3 V.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: Analog Signals")
```

```check
(number
  (ask "A 10-bit ADC with a range of 3.3 V reads 256. About what voltage is that, in volts?")
  (answer 0.825) (within 0.01) (unit "V")
  (from "256 * 3.3 / 1024")
  (wrong-if 1.65 "1.65 V is a reading of 512, half of 1,024. 256 is a quarter.")
  (wrong-if 256 "256 is the number of steps. Each step is 3.3 / 1,024 volts.")
  (hint "256 is what fraction of 1,024?")
  (hint "A quarter of the range: 3.3 / 4."))
```

---

## How fine are the steps?

An ADC with more bits has more steps, so each step is smaller and the
reading finer.

```example
(example
  (problem "An 8-bit ADC has a range of 3.3 V. How big is each step?")
  (step "8 bits make 2 to the power 8 = 256 numbers, so 256 steps.")
  (step "3.3 V is 3,300 millivolts. 3,300 / 256 is about 12.9.")
  (answer "About 12.9 millivolts a step: four times coarser than a 10-bit ADC."))
```

```check
(number
  (ask "A 12-bit ADC: how many steps does it divide its range into?")
  (answer 4096) (from "2^12")
  (wrong-if 12 "12 is the number of bits. Each bit doubles the number of steps.")
  (wrong-if 1024 "1,024 is what 10 bits give. Two more bits double it twice.")
  (hint "2 to the power 12."))
```

---

## Reading a sensor

Many sensors give a voltage that changes with what they measure. A
light-dependent resistor, whose resistance falls as the light grows,
in series with a fixed resistor, makes a voltage that changes with the
light. An ADC input can read it directly. Many **microcontrollers**, small
computers on a single chip, have
several ADC inputs built in.

```check
(choice
  (ask "A sensor's voltage rises smoothly from 0 to 3.3 V as it warms. Why is a GPIO input not enough to measure it?")
  (right "A GPIO input only says whether the voltage is high or low")
  (wrong "A GPIO input cannot take 3.3 V"
    "3.3 V is an ordinary high level for such a pin. The trouble is that it reports only two states, not the values between.")
  (wrong "A GPIO input is too slow for temperature"
    "Temperature changes slowly; speed is not the trouble. The pin can only answer high or low.")
  (hint "How many different answers can a GPIO input give?"))
```

---

## Out again: PWM

A **digital-to-analog converter**, or **DAC**, does the reverse: it
makes a voltage from a number.

A cheaper way gives nearly the same effect with an ordinary pin:
switch it on and off rapidly, and vary the fraction of the time it is
on. This is **pulse-width modulation**, or **PWM**. The fraction of the
time the pin is on is its **duty cycle**. An LED at a 25 percent duty
cycle gives about a quarter of the light, and a motor turns at about a
quarter of its speed.

```figure
(define d (slider 'duty 0 100 25 "On (%)" 5))
(define f (/ d 100))
(define (period k)
  (let ((x0 (+ 32 (* 64 k))) (x1 (+ 32 (* 64 k) (* 64 f))))
    (list x0 130 x0 50 x1 50 x1 130 (+ x0 64) 130)))
(path (append (period 0) (period 1) (period 2) (period 3)) 'ink 1.5)
(line 32 (- 130 (* 80 f)) 288 (- 130 (* 80 f)) 'cool 2)
(text 26 54 "on" 'tiny 'right)
(text 26 134 "off" 'tiny 'right)
(text 160 164 "the dashed line: the average" 'centre 'tiny)
(text 160 196 (string-append "On " (fmt d 0) "% of the time") 'centre 'body 'bold)
(caption "A filter or a slow eye sees only the average.")
```

```check
(number
  (ask "A pin switches between 0 and 3.3 V with a 25 percent duty cycle. About what is its average voltage, in volts?")
  (answer 0.825) (within 0.01) (unit "V")
  (from "0.25 * 3.3")
  (wrong-if 3.3 "The pin reaches 3.3 V, but only for a quarter of the time. The average is a quarter of 3.3 V.")
  (wrong-if 2.475 "That is the off time's share. On for 25 percent gives a quarter of 3.3 V.")
  (hint "A quarter of the time at 3.3 V, the rest at 0."))
```

---

## Smoothing

A resistor and a capacitor make a simple filter that smooths a fast
PWM signal into a steady voltage at its average. Change the duty cycle
over time, and the voltage follows: the pin becomes a rough DAC. Many
simple computers make their sound this way.

```check
(choice
  (ask "An LED is driven by PWM. Which duty cycle makes it dimmest while still lit?")
  (right "10 percent")
  (wrong "90 percent"
    "At 90 percent the LED is on nearly all the time, so it is nearly at full brightness.")
  (wrong "50 percent"
    "At 50 percent it is on half the time: half the light. Less on-time gives less light.")
  (hint "The light you see follows the fraction of the time the LED is on."))
```

---

```check
(number
  (ask "A 10-bit ADC with a range of 3.3 V reads 1,023, its largest number. About what voltage is on its input, in volts?")
  (answer 3.3) (within 0.01) (unit "V")
  (from "1023 * 3.3 / 1024")
  (wrong-if 1023 "1,023 is the reading, in steps. Turn it into volts: each step is 3.3 / 1,024 V.")
  (hint "1,023 is nearly all of 1,024 steps."))
```

So: an ADC turns a voltage into a number, and PWM, smoothed, turns a
number back into a voltage.
