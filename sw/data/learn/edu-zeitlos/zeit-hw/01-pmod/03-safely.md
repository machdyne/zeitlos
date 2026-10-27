# Connecting safely

```lesson
(objective "Check a connection before power is applied, so that neither the board nor you is hurt.")
(minutes 20)
```

```warn
(warn "Never mains" "Nothing in this course connects to a wall socket. Mains voltage can kill. The board runs from a 5 V USB supply; treat the power adapter as a sealed box and never open it or wire to it.")
```

A Zeitlos computer runs from a 5 V USB supply, and the signals on its
PMOD connectors are at 3.3 V. These are not a shock hazard. The dangers
here are to the board, and to your fingers when a short circuit makes a
wire hot.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Working Safely")
```

```check
(choice
  (ask "You want to switch a lamp that runs from the wall socket with a PMOD pin. What is the right plan?")
  (right "Do not wire it yourself: mains wiring is work for a trained person")
  (wrong "Wire the lamp's cable to the pin through a resistor"
    "A resistor does not make mains safe, and mains on a 3.3 V pin destroys the board at once. Mains voltage can kill.")
  (wrong "It is fine as long as the board is switched off while you wire it"
    "The danger is in the mains wiring itself, whenever it is connected. Leave it to someone trained to do it.")
  (hint "What does this lesson say about anything that plugs into the wall?"))
```

---

## Power off first

```warn
(warn "Power off before you connect" "Unplug the board's USB power before you plug in a module or move a wire. A wire that brushes the wrong pin while the board is on can short 3V3 to ground or put a voltage where it does not belong.")
```

Make every connection with the power off, check it, and only then plug
the power back in. The same goes for taking something out.

```check
(choice
  (ask "A module is plugged into the wrong row of a 12-pin connector, and the board is running. What do you do first?")
  (right "Unplug the board's power, then move the module")
  (wrong "Move the module quickly while it runs"
    "While it moves, its pins can touch the wrong pins of the connector, shorting the supply or feeding a signal where it does not belong. Power off first.")
  (wrong "Restart the computer from the desktop, then move it"
    "A restart does not take the power away from the connector. Unplug the USB power.")
  (hint "The first rule of this lesson is about the power."))
```

---

## 3.3 V only

```warn
(warn "Never 5 V on a pin" "The PMOD pins are 3.3 V pins. A 5 V signal, or a 5 V supply, on any of them can destroy the pin or the whole FPGA. Many hobby modules and boards run at 5 V: check before you connect. An old RS-232 serial port swings to 12 V: never connect it directly.")
```

Every pin of a PMOD connector is a 3.3 V pin: it reads and drives
signals between 0 and 3.3 V. A module for it must work at 3.3 V too,
powered from the connector's 3V3 pin. A 5 V module needs a level
shifter, a chip that converts between 5 V and 3.3 V signals, between
it and the board.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Logic Levels; Talking to the World: UART")
(source "Zeitlos docs: release/hw/pmods/gpio.spec (LVCMOS33, 3.3 V pins)")
```

```check
(choice
  (ask "A sensor module's data sheet says: supply 5 V, outputs 0 to 5 V. Can you plug it straight into a PMOD connector?")
  (right "No: its 5 V outputs would drive 3.3 V pins too high")
  (wrong "Yes, if you power it from the connector's 3V3 pin"
    "Its data sheet asks for 5 V, so at 3.3 V it may not work at all. Choose a 3.3 V sensor, or use a level shifter.")
  (wrong "Yes, because 5 V is not a shock hazard"
    "It is no danger to you, but it is to the board: 5 V on a 3.3 V pin can destroy it.")
  (hint "Compare the module's signal voltage with the pins' 3.3 V."))
```

---

## No short circuits

```warn
(warn "No shorts" "Never join 3V3 to ground, and never wire a pin straight to 3V3 or ground. A short lets a large current flow: it can burn the board's traces and make a wire hot enough to burn you.")
```

A short is easy to make by accident: a loose wire end, a module in the
wrong row, a dropped screwdriver. Two outputs joined together are a
short too, when one drives high and the other low.

```check
(choice
  (ask "You wire a push button between a GPIO pin and 3V3. Your program sets that pin as an output and drives it low. What happens when you press the button?")
  (right "A short: the pin drives low into the button's 3.3 V, and a large current flows")
  (wrong "The pin reads high while the button is pressed"
    "The pin is an output here, not an input. Driving low while the button joins it to 3.3 V is a short.")
  (wrong "Nothing: buttons cannot short anything"
    "A pressed button is a wire. Between an output driven low and 3.3 V, it is a short circuit.")
  (hint "A pressed button connects its two ends. What is on each end?"))
```

---

## Before the power goes on

Check every new connection with the power off:

1. **Ground is shared** between the board and whatever you connect.
2. **Voltages match**: 3.3 V signals and a 3.3 V supply.
3. **Polarity** is right: LEDs and modules the right way round.
4. **No short**: with a multimeter, the resistance between 3V3 and
   ground is not near zero.
5. **Pull-ups** are there where a bus needs them.

Touch a grounded metal object before handling a bare board, and hold
boards by their edges: a spark of static can destroy a chip.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Making Connections; Working Safely")
```

```check
(choice
  (ask "With the power off, your multimeter shows almost 0 ohms between the 3V3 pin and a ground pin of the connector. What does it mean?")
  (right "Something shorts the supply to ground: find it before you switch on")
  (wrong "All is well: low resistance means good connections"
    "Between the supply and ground you want a high resistance. Near zero means a short, which would let a large current flow.")
  (wrong "The meter needs the power on to measure"
    "Resistance is measured with the power off. This check is made before switching on, for that reason.")
  (hint "What does nearly 0 ohms between the supply and ground let flow when the power comes on?"))
```

---

## Practice

```check
(choice (several)
  (ask "Which of these are safe to do? Choose all that are.")
  (right "Unplug the USB power, then plug a 3.3 V module into the connector")
  (right "Measure the resistance from 3V3 to ground with the power off")
  (wrong "Plug a module in while the board runs, if you are careful"
    "Careful hands still slip, and the pins touch others on the way in. Power off first.")
  (wrong "Connect a 5 V module's output to a pin, since 5 V is not dangerous to people"
    "5 V is no danger to you, but it can destroy a 3.3 V pin.")
  (hint "Two are safe: one is about power, one about measuring."))
```

So: power off, 3.3 V only, no shorts, never mains, and check before
you switch on.
