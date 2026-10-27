# Writing a netlist

```lesson
(objective "Read and change a netlist: parts, their pins, buses and pull-ups.")
(minutes 20)
```

A netlist is plain text, one statement a line; `#` starts a comment.
Here is most of `panel.net`:

```
tca9535  x1  addr=0x20             # a part: TYPE NAME, settings
bus      main  x1                  # an I2C bus, and the parts on it
led      l0  x1.P00                # joined to x1's pin P00
led      l1  x1.P01  active=low    # lit when the pin is low
button   b0  x1.P10  pullup        # a pull-up on its net
load     lamp  "Grow light"  x1.P04
```

A part line is its type, its name, an optional label in quotes, the
pins it joins (`PART.PIN`), then settings such as `addr=`. A part must
be declared before another line names it.

```sources
(source "Zeitlos docs: bench.md, Netlists")
```

```check
(choice
  (ask "Which line puts an LED named l3 on the TCA9535's pin P03?")
  (right "led l3 x1.P03")
  (wrong "led x1.P03 l3" "The name comes before the pins: TYPE NAME PIN.")
  (wrong "l3 led x1.P03" "The type comes first: led, then the name.")
  (hint "TYPE NAME PART.PIN"))
```

---

## The parts

| Type | Pins | Settings |
|---|---|---|
| `tca9535` | P00-P07, P10-P17, INT | `addr=` 0x20 to 0x27 |
| `tca9555` | the same | the same; pull-ups on its inputs |
| `led` | one | `active=high` (the default) or `low` |
| `load` | one | a label, `active=` |
| `button` | one | `to=gnd` (the default) or `vcc` |
| `switch` | one | `to=`, and it stays put |

`pullup PART.PIN` and `pulldown PART.PIN` put a resistor on a pin's
net; the word `pullup` at the end of a part's line does the same for
its pins.

```check
(choice
  (ask "A button pulls its pin to ground while pressed. Without a pull-up on its net, what does the pin show while it is not pressed?")
  (right "Floating: nothing drives it, a dotted square with a ?")
  (wrong "High" "Only a pull-up would make it high. With the button open and no pull-up, nothing sets the level.")
  (wrong "Low" "The button pulls low only while pressed. Open, it leaves the net alone.")
  (hint "Which line in panel.net gives b0 its high level?"))
```

---

## Floating inputs

The TCA9535 has no pull-ups of its own, so its pins with nothing on
them float, and bench shows them so. The same chip with pull-ups is the
TCA9555. Open `panel.net` in `text`, change `tca9535` to `tca9555`,
save, and press F5 in bench: the free pins now read high.

```sources
(source "Zeitlos docs: bench.md, Parts; sw/apps/bench/examples/panel.net (its comments)")
```

```try
(try (app "text") (task "Open /data/bench/examples/panel.net in text (from files), change tca9535 to tca9555, save it, and press F5 in bench. Look at P05 to P07. Then come back."))
```

```check
(choice
  (ask "Why does bench show floating pins at all, when a real chip's floating input just reads 1 or 0?")
  (right "Because on the real chip that reading is unpredictable, and a floating input is a mistake worth seeing")
  (wrong "Because bench cannot work out a level" "It could pick one. It shows the ? on purpose: on the TCA9535 that floating pin is a known trap.")
  (wrong "Because floating pins are short circuits" "A floating pin is nothing connected, the opposite of a short. It reads whatever stray charge says.")
  (hint "What does a real floating input read?"))
```

---

## Mistakes stop the load

A line bench cannot use stops the load, and the status line says where:
`panel.net line 4: x2: no such part (declared later?)`. A setting a part
does not have is an error too, so a typo cannot pass unnoticed.

```check
(choice
  (ask "You write led l0 x1.P00 actve=low. What does bench do when you press F5?")
  (right "It stops, and names the line with the unknown setting")
  (wrong "It ignores actve and lights the LED when high" "A setting a part does not have is an error, so a misspelt one is caught.")
  (wrong "It guesses that you meant active" "bench never guesses: it says where the mistake is.")
  (hint "Is actve a setting an LED has?"))
```

---

```check
(choice
  (ask "You add a second TCA9535 to panel.net, and its name to the bus line. Which part line is right?")
  (right "tca9535 x2 addr=0x21")
  (wrong "tca9535 x2 addr=0x20" "x1 already answers at 0x20. Two chips on one bus need two addresses.")
  (wrong "tca9535 x2 addr=0x30" "A TCA9535's address is 0x20 to 0x27.")
  (hint "A different address, from 0x20 to 0x27."))
```

So: a netlist is parts, their pins and their settings, one a line, and
bench reads it again with F5.
