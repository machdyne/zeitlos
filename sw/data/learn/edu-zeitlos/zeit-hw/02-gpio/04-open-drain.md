# Sharing a wire

```lesson
(objective "Use a GPIO pin as an open-drain output, and watch pins with the logic app.")
(minutes 20)
(keywords "logic (the app)")
```

When several devices share one wire, none of them may drive it high,
or one driving high and another driving low would make a short. So each
uses **open drain**: it can only pull the wire low, or let it go. A
pull-up takes the wire high when nobody pulls. Toggle the two devices.

```figure
(define a (toggle 'a "A pulls"))
(define b (toggle 'b "B pulls"))
(define low (or a b))
(text 160 24 "3V3" 'centre 'small)
(line 160 30 160 40 'ink 1.5)
(hw-resistor-v 160 40 90 "pull-up")
(line 40 100 280 100 'ink 2)
(line 160 90 160 100 'ink 1.5)
(dot 160 100)
(text 290 104 "wire" 'small)
(text 80 186 "A" 'centre 'body 'bold)
(hw-switch 80 100 a)
(text 240 186 "B" 'centre 'body 'bold)
(hw-switch 240 100 b)
(hw-level 160 180 (not low))
(caption "Open drain: anyone can pull low, nobody drives high")
```

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Floating Inputs and Pull-ups")
(source "Zeitlos docs: gpio.md, Open drain, and why DIRSET/DIRCLR exist")
```

```check
(choice
  (ask "Device A lets the wire go, and device B pulls it low. What level is the wire?")
  (right "Low")
  (wrong "High, because A lets it go" "Letting go is not driving high. Any one device pulling low makes the wire low.")
  (wrong "It is a short circuit" "Open drain never drives high, so a pull and a let-go cannot fight. That is the point of it.")
  (hint "With open drain, who can make the wire high?"))
```

---

## Open drain from the REPL

The board has no special hardware for this. Zeitlos does it by
switching the pin between an output driven low and an input:

```
> gpio-mode 0 2 od
> gpio-od 0 2 #f
> gpio-od 0 2 #t
```

`gpio-od 0 2 #f` pulls bit 2 low; `#t` lets it go. It answers with the
level the wire really has. On a wire that another device is holding
down, `gpio-od 0 2 #t` answers `#f`: that is how you see a stuck device
from the REPL.

```sources
(source "Zeitlos docs: gpio.md, From Scheme")
```

```check
(choice
  (ask "A pin is in od mode. You type gpio-od 0 2 #t and it answers #f. What does that tell you?")
  (right "Something else on the wire is holding it low")
  (wrong "The command failed" "It worked: the pin let go. The answer is the wire's real level, and something else keeps it low.")
  (wrong "The pin is driving the wire low" "#t means let go. The board is not pulling; another device is.")
  (hint "The answer is the level of the wire, not what you asked for."))
```

---

## A mode with nowhere to live

The pin itself only knows input and output, so after
`gpio-mode 0 2 od`, asking `gpio-mode 0 2` answers `in` or `out`,
depending on whether it is pulling low at that moment. I2C uses open
drain for both of its wires, which you meet in the next unit.

```check
(choice
  (ask "You set bit 2 to od, let it go, then ask gpio-mode 0 2. What does it answer?")
  (right "in")
  (wrong "od" "The hardware has no open-drain setting to report. A let-go pin is an input.")
  (wrong "out" "It answers out only while it is pulling low. Let go, it is an input.")
  (hint "A pin that lets go of the wire is, to the hardware, an input."))
```

---

## The logic app

**logic** is an app for watching and driving GPIO pins. It is not on
the dock: in the REPL, type `run logic`. Each row of its CHANNELS panel
is one bit: a lamp that is filled when the pin is high, a button that
goes IN, OUT and OD in turn, a level toggle, and the PMOD pin the bit
is on (`p1`, `p2` ... `p7`). Every pin starts as an input when it opens.

It can also capture all eight pins for a short time, draw them as
traces, and decode an I2C transfer.

```sources
(source "Zeitlos docs: logic_app.md, The panel; Channels")
```

```try
(try (app "logic") (task "On a board with a GPIO port, with the power off, wire PMOD pin 1 to a GND pin. Power on, type run logic in the REPL, and look at bit 0's lamp: it is dark, the pin low. Unplug the power before you remove the wire. Then come back."))
```

```check
(choice
  (ask "In logic, why does each channel row show a PMOD pin such as p7?")
  (right "Because bit order is not the order of pins along the connector: bit 4 is on pin 7")
  (wrong "Because p7 is the bit number" "The bit is the row's number. p7 is the pin on the connector, which differs from the bit for bits 4 to 7.")
  (wrong "Because logic can only use pin 7" "Every bit has its row. The p number tells you where on the connector it is.")
  (hint "Which bit is on pin 7?"))
```

---

```check
(choice
  (ask "In logic's CHANNELS panel, a row is set to OD and its level to HI. What is the pin doing?")
  (right "Letting go of the wire, so the pull-up can take it high")
  (wrong "Driving the wire high" "In OD, HI means released. An open-drain pin never drives high.")
  (wrong "Reading only, as an input" "It is in OD, and could pull low if set LO. HI lets the wire go.")
  (hint "What does open drain never do?"))
```

So: open drain only pulls low or lets go, so several devices can share
a wire; `gpio-od` and the logic app both do it.
