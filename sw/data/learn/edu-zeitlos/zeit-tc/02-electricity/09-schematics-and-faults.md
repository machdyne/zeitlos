# Schematics, data sheets and finding faults

```lesson
(objective "Read a simple schematic and a data sheet's limits, and find a fault in a fixed order.")
(minutes 25)
(keywords "schematic" "reference designator" "net (in a schematic)" "data sheet" "absolute maximum rating" "multimeter" "continuity (electrical)" "oscilloscope" "logic analyser" "fault (in a circuit)")
```

You will learn to read the drawings and documents that describe a
circuit, and to find what is wrong with one.

A **schematic** is a drawing of a circuit: which components it has, and
how they are connected. It does not show where they sit on the board.
To repair anything electronic, it is the most important document.

- Lines are wires. Where lines meet at a **dot**, they are connected;
  where they cross without a dot, they are not.
- Each component is a symbol with a label such as R1 or D1, its
  **reference designator**, and its value. The same label is printed
  beside the part on the board.

```figure
(text 30 50 "3V3" 'small 'centre)
(path '(30 56 30 70 70 70) 'ink 1.5)
(el-res 70 70 140 70 "R1 270")
(el-led 140 70 210 #f)
(text 175 98 "D1" 'small 'centre)
(path '(210 70 250 70 250 150) 'ink 1.5)
(el-ground 250 156)
(path '(30 120 200 120) 'ink 1.5)
(path '(110 90 110 150) 'ink 1.5)
(path '(160 100 160 150) 'ink 1.5)
(dot 160 120)
(text 110 168 "not joined" 'tiny 'centre)
(text 160 168 "joined" 'tiny 'centre)
(text 160 215 "R1 and D1: labels also printed on the board" 'centre 'small)
```

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Reading a Schematic")
```

```check
(choice
  (ask "On a schematic, two wires cross with no dot where they meet. Are they connected?")
  (right "No")
  (wrong "Yes: they touch on the drawing"
    "Crossing lines are drawn over each other without being joined. Only a dot where lines meet means a connection.")
  (hint "Look at the two crossings in the figure."))
```

---

## Letters and values

The letter of a reference designator says what kind of part it is:

| Letter | Part | Letter | Part |
|---|---|---|---|
| R | resistor | Q | transistor |
| C | capacitor | U | integrated circuit |
| L | inductor | Y | crystal |
| D | diode, LED | J | connector |

Values often put the prefix where the point would be, so that a
point lost in printing cannot change the value: 4k7 is 4.7k, and 2R2 is
2.2 ohms.

```check
(number
  (ask "A resistor is marked 2k2. How many ohms is it?")
  (answer 2200) (from "2.2 * 1000")
  (wrong-if 22 "The k stands where the point would be: 2k2 is 2.2k, which is 2.2 times 1000.")
  (wrong-if 2.2 "2R2 would be 2.2 ohms. The k means thousands: 2.2k.")
  (hint "2k2 means 2.2k."))
```

---

## Nets

Every set of pins joined together is a **net**. A net can be given a
name, and every place on the schematic with the same name is joined,
even on another sheet. Supply symbols such as 3V3 and GND are named nets
too: every GND symbol is the same ground.

To find why an LED does not light, find it on the schematic, follow its
net back through its resistor to the pin that drives it, and check each
of those points on the board.

```check
(choice
  (ask "A schematic shows GND symbols on sheet 1 and on sheet 3. Are they connected?")
  (right "Yes: every GND symbol is the same ground net")
  (wrong "No: they are on different sheets"
    "Names join nets across sheets. Every symbol with the same name, GND included, is the same net.")
  (hint "What does a net's name tell you?"))
```

---

## Data sheets

A **data sheet** is the maker's description of a component. Its parts
come in a standard order:

- **pin descriptions**: what each pin is for;
- **absolute maximum ratings**: the limits beyond which the part may be
  damaged, such as the most voltage on any pin. They are not working
  conditions;
- **recommended operating conditions**: the supply voltage and
  temperatures within which the part is sure to work;
- **electrical characteristics**: guaranteed figures, such as the
  voltages read as high and low. Rely on their minimum or maximum, not
  the typical value.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Reading a Data Sheet")
```

```check
(choice
  (ask "A chip's absolute maximum rating for its supply is 4 V. Is it fine to run it at 3.9 V?")
  (right "No: use the recommended operating conditions, not the maximum")
  (wrong "Yes: 3.9 V is below 4 V"
    "An absolute maximum is the edge of damage, not a working condition. Run the chip within its recommended operating conditions.")
  (hint "Is an absolute maximum a working condition?"))
```

---

## Measuring

A **multimeter** measures:

- voltage, with its two probes on two points;
- current, placed in the loop, so that the current flows through it;
- resistance;
- **continuity**: it beeps when two points are connected.

An **oscilloscope** draws a voltage as it changes over time, too fast
for a multimeter. A **logic analyser** records many digital signals at
once, as highs and lows.

```check
(choice
  (ask "How do you connect a multimeter to measure the current through an LED?")
  (right "In the loop, so that the current flows through the meter")
  (wrong "With its probes on the LED's two legs"
    "That measures the voltage across the LED. To measure current, the current must flow through the meter: put it in the loop.")
  (hint "Current is what flows round the loop."))
```

---

## Finding a fault

A **fault** is whatever stops a circuit working as it should. Find it
in a fixed order:

1. **Look**: burnt parts, swollen capacitors, cracked joints, rust and
   loose connectors cause many faults.
2. **Power**: measure each supply voltage at the chips that use it.
3. **Timing**: the crystal is working, at its rated speed.
4. **Starting**: the chip has started, and an FPGA has loaded its
   configuration.
5. **Signals**: follow each one, with the schematic, from where it is
   made to where it is used.

Change one thing at a time, and write each result down.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Measuring and Finding Faults")
```

```check
(choice
  (ask "A board does nothing at all. You have looked it over and seen nothing wrong. What do you check next?")
  (right "The supply voltages, measured at the chips")
  (wrong "Each signal, one by one"
    "Signals come last. Without power nothing works, so check the supply voltages first.")
  (wrong "Replace the main chip"
    "Replacing parts on a guess is slow and can add faults. After looking, measure the power.")
  (hint "Nothing works without it."))
```

---

```check
(choice
  (ask "While finding a fault, you change two things at once and the board works. What is the trouble?")
  (right "You do not know which change fixed it")
  (wrong "None: the board works"
    "It works, but you cannot tell which change mattered, or whether the other one did harm. Change one thing at a time.")
  (hint "Why change only one thing at a time?"))
```

So a schematic shows how parts connect, a data sheet gives their
limits, and a multimeter and a fixed order find most faults.
