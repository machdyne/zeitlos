# Working safely and making connections

```lesson
(objective "Name the dangers of electrical work and how to avoid them, and check a connection before switching it on.")
(minutes 20)
(keywords "short circuit" "static electricity" "breadboard" "soldering" "solder" "flux (soldering)" "pad (for soldering)" "polarity")
```

You will learn what can hurt you, or the equipment, and the checks that
prevent most damage.

```warn
(warn "Mains kills" "Anything plugged into a wall socket has mains voltage somewhere inside it, and mains voltage can kill. Power supplies, monitors and chargers also hold capacitors that can keep a dangerous charge long after they are unplugged. Never open one: only a person trained for it should.")
```

The small computers in this course run from a 5 V USB supply, and their
boards have only small voltages on them. The mains danger stays inside the
power adapter. Treat it as a sealed unit.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Working Safely")
```

```check
(choice
  (ask "Your computer's power adapter has stopped working. What do you do?")
  (right "Replace it, and never open it")
  (wrong "Unplug it, wait a minute, then open it to look"
    "Its capacitors can keep a dangerous charge long after it is unplugged. Only a trained person opens a mains device; replace the adapter.")
  (wrong "Open it while plugged in, to see which part has failed"
    "That puts your hands near mains voltage, which can kill. Never open it; replace it.")
  (hint "What can still be inside, even unplugged?"))
```

---

## Current and batteries

```warn
(warn "Short circuits burn" "The 5 V of a USB port or the 12 V inside a PC is too low to give you a dangerous shock. But a short circuit across a battery or a large supply can heat a wire, a tool or a ring until it burns you.")
```

```warn
(warn "Batteries can catch fire" "A lithium battery can catch fire if it is punctured, crushed, overcharged or short-circuited. Charge it only with a charger made for it, and never damage it.")
```

A **short circuit** is a connection with almost no resistance straight across
a supply, from one terminal to the other. By Ohm's law, almost no
resistance means a very large current.

```check
(choice
  (ask "You keep a 9 V battery in a pocket with your keys. What is the danger?")
  (right "The keys can short its terminals, and get hot enough to burn")
  (wrong "None: 9 V is too small to hurt anyone"
    "9 V is too low to give a dangerous shock, but keys across its terminals are a short circuit. The large current can heat them until they burn.")
  (wrong "It gives you an electric shock through the pocket"
    "9 V is too low to give a dangerous shock. The danger is heat: the keys can short the terminals and get hot.")
  (hint "What happens when metal joins a battery's two terminals?"))
```

---

## Protecting the equipment

- **Static electricity**: walking across a carpet can charge you to
  thousands of volts. You may not feel the spark, but it can destroy a
  chip. Touch a grounded metal object before you handle a bare board,
  and hold boards by their edges.
- **Short circuits**: a wire or tool joining a supply to ground can
  burn components and the board's copper tracks. Disconnect the power
  before you change any connection.
- **Pin limits**: a chip's output gives only a few milliamps. A motor
  or other large load needs a transistor to switch it.

```check
(choice
  (ask "Before you pick up a bare circuit board, what should you do?")
  (right "Touch a grounded metal object, then hold the board by its edges")
  (wrong "Nothing: you would feel any charge on you"
    "A charge that can destroy a chip is often too small to feel. Touch grounded metal first.")
  (wrong "Rub your hands together to warm them"
    "Rubbing can build up charge, not remove it. Touch a grounded metal object first.")
  (hint "How do you get rid of static charge safely?"))
```

---

## Breadboards and soldering

A **breadboard** is a plastic block with rows of spring contacts:
components and wires push in without soldering. It suits trying a
circuit out, but its contacts loosen over time.

A circuit meant to last is soldered. **Soldering** joins metal parts
with melted metal, the **solder**. Heat the pin and the pad together
with the iron for a second or two (the **pad** is the patch of copper
on the board that the pin is soldered to), and feed the solder to them,
not to the iron, so it flows onto both. A good joint is smooth and
spreads onto both the pin and the pad; a ball of solder sitting on them
may not conduct reliably. Most solder wire has a core of
**flux**, which cleans the metal so that the solder flows.

```warn
(warn "Soldering burns" "The iron's tip is at about 330 to 380 degrees Celsius, and burns at a touch. Always put it back in its stand. Wear safety glasses: hot solder can spit. Do not breathe the fumes of the flux: work with air moving away from your face. Wash your hands after using solder with lead in it. If you are new to it, learn with someone who solders.")
```

```check
(choice
  (ask "Where do you feed the solder when making a joint?")
  (right "To the heated pin and pad, so it flows onto both")
  (wrong "To the tip of the iron, then take it to the joint"
    "Solder melted on the iron does not wet the cold pin and pad, and makes a ball that may not conduct. Heat the joint and feed the solder to it.")
  (wrong "It does not matter, as long as it melts"
    "It matters: solder flows onto what is hot. Heating the pin and pad, then feeding the solder to them, makes it wet both.")
  (hint "Solder flows onto hot metal."))
```

---

## Before you switch on

A few checks prevent most damage to a new connection:

- **Ground** is shared between every device that exchanges signals.
- **Voltages match**: an output's high suits the input it drives.
- **Polarity** is right: batteries, diodes, LEDs and some capacitors
  work only one way round, and so does a chip in its socket.
- **No short circuit**: with the power off, the resistance between the
  supply and ground is not near zero.
- **Pull-ups** are there where outputs need them.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Making Connections")
```

```check
(choice
  (ask "With the power off, you measure 0.2 ohms between a board's supply and ground. What does it mean?")
  (right "There is probably a short circuit: find it before switching on")
  (wrong "All is well: a small resistance is good"
    "Almost no resistance between supply and ground is a short circuit. Switched on, a very large current would flow.")
  (wrong "Nothing: resistance only matters with the power on"
    "Measuring with the power off is exactly how you find a short circuit before it does damage.")
  (hint "By Ohm's law, what current would 5 V drive through 0.2 ohms?"))
```

---

```check
(choice
  (ask "An LED is in a circuit back to front. What is wrong?")
  (right "Its polarity: an LED works only one way round")
  (wrong "Its resistance is too large"
    "Nothing changed its resistance. An LED is a diode, which lets current through one way only: turned round, it blocks it.")
  (wrong "Its ground is not shared"
    "Ground matters between devices that exchange signals. A back-to-front LED is a polarity mistake.")
  (hint "A diode lets current flow in one direction only."))
```

So mains and short circuits are the dangers to you, static and shorts
the dangers to the equipment, and a few checks before switching on
prevent most damage.
