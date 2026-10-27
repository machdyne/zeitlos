# Pins

```lesson
(objective "Set up a pin as an input or an output, change one pin without disturbing the others, and say why buttons need debouncing and motors need a driver chip.")
(minutes 20)
(keywords "GPIO" "general-purpose input/output" "push-pull" "open-drain" "direction register" "data register" "millisecond" "debouncing" "driver chip" "flyback diode")
```

A computer is useful only if it can sense or change something outside
itself. The simplest way is a single pin whose level a program can read
or set.

Such pins are **general-purpose input/output** pins, or **GPIO**. A
program can set each one up as:

- an **input**: the program reads whether the pin is high or low;
- a **push-pull** output: the pin drives itself high or low, actively,
  either way;
- an **open-drain** output: the pin can only pull itself low; when it
  lets go, a pull-up resistor takes it high.

```sources
(source "Timeless Computing (docs/tc.md), Talking to the World: General-Purpose Pins")
```

```check
(choice
  (ask "A pin reads a push button. How is it set up?")
  (right "As an input")
  (wrong "As a push-pull output"
    "An output sets the pin's level. Here the button sets it, and the program only wants to know what it is.")
  (wrong "As an open-drain output"
    "An open-drain output pulls the pin low itself. To find out what the button is doing, the program reads the pin: an input.")
  (hint "Who decides the pin's level: the program, or the button?"))
```

---

## Two registers

GPIO pins are almost always memory-mapped. Two registers control them,
with one bit for each pin:

- the **direction register** says whether each pin is an input or an
  output;
- the **data register** holds the levels: writing it sets the outputs,
  and reading it reports the inputs.

In this figure a 1 in the direction register makes a pin an output.
Pins 0 to 3 are outputs; pins 4 to 7 are inputs, and something outside
holds them at 1010. Move the slider to write to the outputs.

```figure
(define v (slider 'out 0 15 5 "Pins 0-3" 1))
(define data (+ 160 v))
(text 8 34 "pin" 'tiny)
(text 8 68 "direction" 'tiny)
(text 8 118 "data" 'tiny)
(for-range 0 8
  (lambda (k)
    (let* ((i (- 7 k)) (bx (+ 72 (* k 28))) (cx (+ bx 14)))
      (text cx 34 (fmt i 0) 'centre 'small)
      (wo-bitbox bx 50 28 (wo-bit 15 i))
      (wo-bitbox bx 100 28 (wo-bit data i))
      (if (< i 4) (arrow cx 140 cx 172 'ink) (arrow cx 172 cx 140 'ink))
      (text cx 188 (if (< i 4) "out" "in") 'centre 'tiny))))
(caption "Reading the data register gives every pin's level.")
```

```check
(choice
  (ask "A program reads this data register. What do bits 4 to 7 tell it?")
  (right "The levels on the input pins, set from outside")
  (wrong "What the program last wrote to them"
    "Pins 4 to 7 are inputs, so writing to them does not set them. Reading gives the levels that the outside world puts on those pins.")
  (wrong "Whether they are inputs or outputs"
    "That is the direction register's job. The data register holds levels.")
  (hint "Pins 4 to 7 are inputs. What does reading an input report?"))
```

---

## One pin at a time

The data register holds all the pins together. To change one pin
without disturbing the others, a program reads the register, changes
one bit and writes the whole register back.

```example
(example
  (problem "The data register reads 0000 0101. Set pin 1 high and leave the rest as they are.")
  (step "Read the register: 0000 0101.")
  (step "Pin 1 is bit 1, the second from the right. Change it to 1: 0000 0111.")
  (step "Write 0000 0111 back. Pins 0 and 2 are still high.")
  (answer "0000 0111"))
```

```check
(text
  (ask "The data register reads 1010 0110. Set pin 0 high and leave the other pins as they are. What do you write?")
  (accept "10100111") (accept "1010 0111")
  (wrong-if "00000001" "That sets pin 0 high, but it also takes pins 1, 2, 5 and 7 low. Start from what the register holds now.")
  (wrong-if "0000 0001" "That sets pin 0 high, but it also takes pins 1, 2, 5 and 7 low. Start from what the register holds now.")
  (wrong-if "11100110" "That changed bit 6. Pin 0 is bit 0, the rightmost.")
  (wrong-if "1110 0110" "That changed bit 6. Pin 0 is bit 0, the rightmost.")
  (hint "Copy the eight bits, then change only the rightmost one to 1."))
```

---

## Switches bounce

When a button's contacts close, they make and break several times over
a few **milliseconds** (thousandths of a second) before they settle. A program that counts every
change from high to low counts several presses for one push.

The remedy, **debouncing**, is to accept a new level only after it has
stayed the same for some time. This button pulls its pin low while it
is pressed. Move the slider to change how long the program waits.

```figure
(define w (slider 'wait 0 4 0 "Wait (ms)" 0.5))
(define (tx t) (+ 30 (* 13.5 t)))
(path (list (tx 0) 50 (tx 2) 50 (tx 2) 100 (tx 2.5) 100 (tx 2.5) 50 (tx 3) 50 (tx 3) 100
            (tx 3.8) 100 (tx 3.8) 50 (tx 4.3) 50 (tx 4.3) 100 (tx 16) 100 (tx 16) 50 (tx 20) 50)
      'ink 1.5)
(text 24 54 "high" 'tiny 'right)
(text 24 104 "low" 'tiny 'right)
(line (tx 0) 150 (tx 20) 150 'ink 1)
(for-range 0 5
  (lambda (k)
    (line (tx (* 5 k)) 150 (tx (* 5 k)) 155 'ink 1)
    (text (tx (* 5 k)) 168 (fmt (* 5 k) 0) 'centre 'tiny)))
(text (tx 20) 182 "ms" 'right 'tiny)
(define (edge t d)
  (if (> w 0) (rect (tx t) 112 (* 13.5 w) 6 'grey))
  (if (> d w) (dot (tx t) 128 'ink) 0))
(edge 2 0.5)
(edge 3 0.8)
(edge 4.3 11.7)
(define n (+ (if (> 0.5 w) 1 0) (if (> 0.8 w) 1 0) (if (> 11.7 w) 1 0)))
(text 160 206 (string-append "Presses counted: " (fmt n 0)) 'centre 'body 'bold)
(caption "A dot marks each press the program accepts.")
```

```check
(number
  (ask "With the slider at 0, the program counts every fall from high to low. How many presses does it count for this one push?")
  (answer 3)
  (wrong-if 1 "With no wait, each bounce counts. Count the times the line falls from high to low.")
  (hint "Follow the line from the left and count each drop to low."))
```

```check
(number
  (ask "What is the shortest wait on the slider that counts this push once?")
  (answer 1) (unit "ms")
  (wrong-if 0.5 "At 0.5 ms the second bounce, low for 0.8 ms, still counts. Try the slider.")
  (hint "The bounces stay low for 0.5 ms and 0.8 ms. The wait must be longer than both."))
```

---

## Pins supply little current

A pin can supply a few milliamps: enough for an LED through a resistor,
and not much more. A motor, relay, lamp or solenoid needs a transistor
or a **driver chip**, which the pin controls, to switch its current.

A coil, as in a relay or a motor, also needs a **flyback diode** across
it. When the current through a coil is switched off, the coil produces
a brief high voltage that can destroy the transistor; the diode gives
that current a safe way round.

```check
(choice
  (ask "You want a pin to switch a small motor on and off. What do you need?")
  (right "A transistor or driver chip controlled by the pin, and a flyback diode across the motor")
  (wrong "Nothing: connect the motor to the pin"
    "A pin supplies a few milliamps; a motor needs far more. The pin, and perhaps the chip, can be damaged.")
  (wrong "A transistor, and nothing else"
    "The transistor switches the current, but when it switches a coil off, the coil makes a brief high voltage. The flyback diode protects the transistor from it.")
  (hint "Two problems: how much current the pin can give, and what a coil does when its current stops."))
```

---

```check
(choice
  (ask "Several devices share one wire with a pull-up resistor. Any of them may pull it low. How should their pins be set up?")
  (right "As open-drain outputs")
  (wrong "As push-pull outputs"
    "A push-pull pin drives high actively. If one drives high while another drives low, the two fight and current flows between them. Open-drain pins only ever pull low.")
  (wrong "As inputs"
    "An input cannot pull the wire low, so no device could signal on it.")
  (hint "Which kind of output can only pull low, and lets go otherwise?"))
```

So: a GPIO pin is an input or an output set by registers; debounce
buttons, and give anything that needs real current a driver chip.
