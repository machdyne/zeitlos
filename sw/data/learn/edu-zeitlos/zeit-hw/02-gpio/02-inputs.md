# Reading pins

```lesson
(objective "Read a GPIO port's pins from the REPL, and explain why a pin with nothing on it reads 1.")
(minutes 20)
(keywords "REPL (shell)" "command (typed)")
```

You talk to the pins from the **REPL**, a shell where you type to
the system itself: open `term` from the dock and choose REPL. You type a **command**, a
line that asks for something, and press Enter; the REPL answers on the
next line.

`gpio-ports` answers how many GPIO ports the machine has. 0 means its
target built none, and the rest of this unit is then something to read,
not to do.

```sources
(source "Zeitlos docs: gpio.md, From Scheme; terminal.md, Starting out: the panel")
```

```try
(try (app "term") (task "Open term from the dock and choose REPL. Type gpio-ports and press Enter. Then come back."))
```

```check
(choice
  (ask "You type gpio-ports in the REPL and it answers 0. What does that mean?")
  (right "This machine's target has no GPIO port")
  (wrong "Port 0 is the GPIO port" "gpio-ports counts the ports. A count of 0 means there are none; a machine with one port answers 1, and that port is port 0.")
  (wrong "Nothing is plugged into the connector" "The answer depends on what the FPGA was built with, not on what is plugged in. Free pins with nothing on them are still a port.")
  (hint "The command answers how many ports there are."))
```

---

## A pin with nothing on it

Every GPIO pin starts as an **input**: the board drives nothing on it,
it only reads the level. And every pin has a weak **pull-up** inside the
FPGA, always on. So a pin with nothing connected reads high, 1.

`gpio-in 0` reads all eight pins of port 0 as a byte. With nothing
plugged in, it answers 255.

```sources
(source "Zeitlos docs: gpio.md, Pull-ups; Reset behaviour; From the console shell")
```

```check
(choice
  (ask "Nothing is plugged into port 0. Why does gpio-in 0 answer 255?")
  (right "The pull-ups hold every pin high, and 255 is all eight bits 1")
  (wrong "The pins are outputs driving high" "Every pin starts as an input. Nothing drives them: the weak pull-ups make them read high.")
  (wrong "255 means the port is not there" "A missing port is gpio-ports answering 0. 255 is a working port at rest, every bit 1.")
  (hint "What does each pin read when nothing pulls it low?"))
```

---

## A button

Wire a push button between a pin and ground. While it is open, the
pull-up holds the pin high. Press it, and the pin is joined to ground:
it reads low. Toggle the button in the figure.

```figure
(define pressed (toggle 'press "Pressed"))
(text 60 30 "3V3" 'centre 'small)
(line 60 36 60 50 'ink 1.5)
(hw-resistor-v 60 50 110 "pull-up")
(line 60 110 60 130 'ink 1.5)
(line 60 130 150 130 'ink 1.5)
(dot 60 130)
(text 160 134 "pin" 'small)
(hw-switch 60 130 pressed)
(text 80 160 "button" 'small)
(hw-level 240 140 (not pressed))
(text 240 104 "the pin reads" 'centre 'small)
(caption "A pull-up and a button to ground")
```

```check
(choice
  (ask "A button joins PMOD pin 1 to ground. The pull-up is inside the board. What does the pin read while the button is pressed?")
  (right "Low, 0")
  (wrong "High, 1" "The pull-up is weak: a button straight to ground wins against it, and the pin reads low.")
  (wrong "It cannot be known: the pin floats" "With the pull-up the pin never floats. Open, it reads high; pressed, it reads low.")
  (hint "Pressed, the button is a wire from the pin to ground."))
```

---

## One pin

`gpio-get 0 2` reads one pin: port 0, bit 2. It answers `#t` for high
and `#f` for low: the REPL's words for true and false.

```example
(example
  (problem "A button on PMOD pin 3 of port 0 is held down. Nothing else is connected. What do gpio-in 0 and gpio-get 0 2 answer?")
  (step "PMOD pin 3 is bit 2, worth 4.")
  (step "The button pulls it low; the other seven pins read high.")
  (step (number (ask "So gpio-in 0 answers?") (answer 251) (from "255 - 4")
          (wrong-if 247 "That is 255 - 8: bit 3 low. Pin 3 is bit 2, worth 4.")))
  (answer "gpio-in 0 answers 251, and gpio-get 0 2 answers #f."))
```

```check
(number
  (ask "Buttons on PMOD pins 1 and 2 are both held down. Nothing else is connected. What does gpio-in 0 answer?")
  (answer 252) (from "255 - 1 - 2")
  (wrong-if 253 "That is only pin 2 low. Pin 1 is bit 0, worth 1, and it is low too.")
  (wrong-if 3 "3 is the value of the two pressed pins. The port reads the pins that are high: the other six.")
  (hint "Start from 255. Pins 1 and 2 are bits 0 and 1, worth 1 and 2."))
```

---

## High from the start

Because every pin is pulled high from the moment the FPGA is
configured, a module input that switches something on when it sees high
(an enable, a reset, a mode pin) sees high at power-on. A 10k pull-down
on the module holds such a pin low until software drives it.

```sources
(source "Zeitlos docs: gpio.md, The one thing to check before plugging something in")
```

```check
(choice
  (ask "A motor driver module turns its motor on when its EN pin is high. You wire EN to a GPIO pin. What happens when the board starts?")
  (right "EN sees high from the pull-up, so the motor may start before any program runs")
  (wrong "Nothing: pins are inputs at start, so they cannot turn anything on"
    "An input with a pull-up still looks high to the module. To the motor driver, that is on.")
  (wrong "EN floats, so the motor stays off" "The board's pull-up stops it floating: it reads high.")
  (hint "What level does a GPIO pin show to whatever is connected to it, before a program sets it?"))
```

So: every pin starts as an input pulled high, so a free pin reads 1 and
a pin pulled to ground reads 0; `gpio-in` reads the port and
`gpio-get` one pin.
