# Driving a virtual chip

```lesson
(objective "Drive the LEDs and read the buttons of panel.net's I/O expander with the i2c command.")
(minutes 25)
(keywords "I/O expander")
```

`panel.net` has one chip, a TCA9535 at address 0x20, on a bench bus
named `main`. A TCA9535 is an **I/O expander**: sixteen pins, P00 to
P07 and P10 to P17, that a master sets and reads over I2C. Four LEDs
are on P00 to P03 and a lamp on P04; two buttons and a switch are on
P10 to P12.

`i2c -b main` talks to the bench bus instead of a PMOD port:

```
$ i2c -b main scan
0x20
```

```sources
(source "Zeitlos docs: bench.md, Parts; sw/apps/bench/examples/panel.net")
```

```try
(try (app "term") (task "With panel.net open in bench, open term from the dock, choose POSIX and type i2c -b main scan. Then come back."))
```

```check
(choice
  (ask "Why does i2c -b main scan find 0x20 on a machine with nothing plugged in?")
  (right "main is a bus inside bench, and its virtual TCA9535 answers at 0x20")
  (wrong "0x20 is a chip on the board itself" "-b main names the bench bus. The answer comes from the virtual part in the netlist.")
  (wrong "Every scan finds 0x20" "A scan lists only what answers. On main, that is the TCA9535 from panel.net.")
  (hint "What does -b main choose?"))
```

---

## Its registers

The TCA9535 has its registers in pairs, one for each port of eight pins:

| Register | Holds |
|---|---|
| 0, 1 | the input levels of port 0 (P00-P07), port 1 (P10-P17) |
| 2, 3 | the output levels |
| 6, 7 | the direction: a 1 bit is an input, a 0 an output |

At first every pin is an input. So the LEDs need port 0 made outputs
first:

```
$ i2c -b main write 0x20 6 0x00
```

```sources
(source "Zeitlos docs: bench.md, Parts (the TCA9535 follows its datasheet)")
```

```check
(choice
  (ask "Why does i2c -b main write 0x20 2 0xFF alone not light the LEDs, right after bench starts?")
  (right "Port 0's pins are still inputs; register 6 must make them outputs first")
  (wrong "0xFF turns every LED off" "0xFF drives every output high, which would light them, but the pins are not outputs yet.")
  (wrong "Register 2 is port 1" "Register 2 is port 0's outputs. The trouble is the direction, in register 6.")
  (hint "What are the pins at first?"))
```

---

## A byte of LEDs

With port 0 as outputs, register 2 sets its eight pins: bit 0 is P00,
bit 1 P01, and so on. Move the slider to choose the byte.

```figure
(define v (slider 'v 0 255 85 "Register 2" 1))
(for-range 0 8
  (lambda (k)
    (let* ((i (- 7 k)) (x (+ 24 (* k 36))) (on (= 1 (hw-bit v i))))
      (if on (rect (+ x 2) 42 28 28 'ink))
      (frame x 40 32 32 'ink 1)
      (text (+ x 16) 61 (if on "1" "0") 'centre 'body (if on 'paper 'ink))
      (text (+ x 16) 32 (string-append "P0" (fmt i 0)) 'centre 'small))))
(for-range 0 4
  (lambda (i)
    (let ((x (- 292 (* i 36))))
      (if (= 1 (hw-bit v i)) (circle x 120 12 'ink) (ring x 120 12 'ink 1.5))
      (text x 150 (string-append "l" (fmt i 0)) 'centre 'small))))
(if (= 1 (hw-bit v 4)) (rect 132 108 32 24 'warm) (frame 132 108 32 24 'ink 1.5))
(text 148 150 "lamp" 'centre 'small)
(text 160 190 (string-append "write 0x20 2 " (fmt v 0) "  (" (hw-bin v 8) ")") 'centre 'body 'bold)
(caption "panel.net: LEDs on P00-P03, the lamp on P04")
```

```check
(number
  (ask "Which byte, written to register 2, lights l0 and l1 and nothing else?")
  (answer 3) (from "1 + 2")
  (wrong-if 12 "12 is bits 2 and 3: l2 and l3. l0 and l1 are bits 0 and 1.")
  (wrong-if 19 "19 lights the lamp too: bit 4 is P04.")
  (hint "l0 is bit 0, worth 1; l1 is bit 1, worth 2."))
```

---

```check
(text
  (ask "Type the command that lights only the lamp on P04, port 0 already being outputs.")
  (accept "i2c -b main write 0x20 2 16") (accept "i2c -b main write 0x20 2 0x10")
  (accept "i2c -b main write 32 2 16") (accept "i2c -b main write 32 2 0x10")
  (wrong-if "i2c -b main write 0x20 2 4" "4 is bit 2, P02, which is l2. P04 is bit 4, worth 16.")
  (wrong-if "i2c write 0x20 2 16" "Without -b main, i2c uses the PMOD port. The lamp is on the bench bus.")
  (hint "P04 is bit 4. What is bit 4 worth?")
  (hint "i2c -b main write 0x20 2 16"))
```

---

## Reading the buttons

The buttons pull their pins to ground while pressed, so a pressed
button reads 0. Register 1 holds port 1's inputs:

```
$ i2c -b main write 0x20 1 read 1
fe
```

`fe` is 1111 1110: bit 0, P10, is low, so button b0 is held down.

```check
(choice
  (ask "Reading register 1 gives fd. Which pin is low?")
  (right "P11 (bit 1)")
  (wrong "P10 (bit 0)" "fd is 1111 1101: bit 0 is 1. The 0 is in bit 1.")
  (wrong "P12 (bit 2)" "Bit 2 of fd is 1. Write it in binary: 1111 1101.")
  (hint "Write fd in binary and find the 0."))
```

So: make a port's pins outputs with its direction register, then each
bit of the output register drives one pin; the input register reads
them.
