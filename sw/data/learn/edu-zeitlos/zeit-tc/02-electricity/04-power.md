# Power, supplies and batteries

```lesson
(objective "Work out power in watts, say how a computer's supply is made, and work out the energy in a battery.")
(minutes 25)
(keywords "power (electric)" "watt (unit)" "direct current" "DC (direct current)" "alternating current" "AC (alternating current)" "mains (electricity)" "regulator (voltage)" "linear regulator" "switching regulator" "brown-out" "noise (electrical)" "milliamp-hour (unit)" "watt-hour (unit)" "Wh (watt-hour)" "USB (plug)")
```

You will learn how much electricity a computer uses, and where it comes
from.

**Power** is how fast energy is used. It is measured in **watts** (W),
and it is the voltage times the current:

$$ P = V * I $$

A circuit drawing 0.5 A from a 5 V supply uses 0.5 times 5, which is
2.5 W. Almost all of that power ends up as heat. That is why a resistor
carrying too much current gets hot, and why a fast processor needs
cooling.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Power")
```

```check
(number
  (ask "A board draws 0.2 A from a 3.3 V supply. What power does it use, in watts?")
  (answer 0.66) (from "3.3 * 0.2")
  (wrong-if 16.5 "That divides 3.3 by 0.2. Power is voltage times current: P = V * I.")
  (wrong-if 3.5 "That adds them. Multiply: 3.3 times 0.2.")
  (hint "P = V * I."))
```

---

## A few watts

Power decides what a computer can run from. A small FPGA computer uses
a few watts, about what a phone charger with a **USB** plug, the small plug
of phones and computers, can give. A desktop
computer can use a hundred times as much.

A few watts can come from a modest battery or a small solar panel.
Several hundred watts needs a large installation.

```check
(choice
  (ask "A small solar panel gives about 10 W in sunshine. What could it run?")
  (right "A small FPGA computer using a few watts")
  (wrong "A desktop computer using 300 W"
    "300 W is thirty times what the panel gives. A desktop computer needs a large installation.")
  (wrong "Nothing: computers need electricity from a wall socket"
    "A small computer that uses a few watts can run from a battery or a small solar panel.")
  (hint "Compare the watts."))
```

---

## DC and AC

A battery gives **direct current** (**DC**): its + terminal stays +.
The electricity from a wall socket, the **mains**, is
**alternating current** (**AC**): its voltage reverses, from one direction to the
other, many times a second. That makes it easy to change from one
voltage to another, and to send over long distances.

```figure
(define k (choice 'kind '("DC" "AC") "Kind"))
(graph 40 30 260 150 0 10 -10 10)
(graph-axes 100 5 "time" "V")
(if (= k 0)
    (plot (lambda (t) 9) 'ink)
    (plot (lambda (t) (* 9 (sin (* 2 pi (/ t 2.5))))) 'ink))
(text 160 222 (if (= k 0) "DC: the voltage keeps one direction"
                  "AC: the voltage reverses again and again") 'centre 'small)
```

```check
(choice
  (ask "Which gives alternating current?")
  (right "The mains, from a wall socket")
  (wrong "A battery"
    "A battery's + terminal stays +: its voltage never reverses, so it gives direct current.")
  (wrong "A USB port"
    "A USB port gives a steady 5 V that does not reverse: direct current.")
  (hint "Which one's voltage reverses many times a second?"))
```

---

## Regulators

Electronic circuits run on DC. A power adapter turns mains AC into DC,
usually 5 V for USB. Inside the computer, **regulators** turn that into
the exact voltages each chip needs: 3.3 V for most of the chips'
connections, and often around 1 V for the inside of an FPGA or
processor.

A **linear regulator** drops the extra voltage across itself, and the
power that wastes becomes heat. It is simple, cheap and quiet. A **switching regulator** switches its input
on and off very fast and smooths the result. It wastes only 5 to 15
percent, but is more complex and makes some electrical **noise**: small,
unwanted changes in a voltage.

```sources
(source "Timeless Computing (docs/tc.md), Electricity: Direct and Alternating Current; Power Supplies and Batteries")
```

```example
(example
  (problem "A linear regulator turns 5 V into 3.3 V for a board drawing 0.3 A. How much power does it waste?")
  (step "The extra voltage is 5 - 3.3 = 1.7 V.")
  (step "The whole current flows through the regulator, so it wastes 1.7 V times 0.3 A.")
  (step (number (ask "How many watts is that?") (answer 0.51) (from "(5 - 3.3) * 0.3")
          (wrong-if 0.99 "That is the board's own power, 3.3 times 0.3. The regulator wastes the extra 1.7 V times 0.3 A.")))
  (answer "0.51 W: about a third of the 1.5 W drawn from the 5 V supply."))
```

```check
(number
  (ask "A linear regulator turns 5 V into 3.3 V at 0.1 A. How many watts does it waste?")
  (answer 0.17) (from "(5 - 3.3) * 0.1")
  (wrong-if 0.33 "That is the power the board uses, 3.3 times 0.1. The regulator wastes the difference, 1.7 V, times the current.")
  (wrong-if 0.5 "That is all the power drawn, 5 times 0.1. The waste is only the extra 1.7 V times 0.1 A.")
  (hint "The extra voltage is 5 - 3.3 = 1.7 V. Times the current."))
```

---

## When the supply is not enough

A supply can give only so much current. A USB 2.0 port must be able to
give 0.5 A at 5 V to a device it has recognized, and a USB 3.0 port
0.9 A. Chargers often give 2 A or more.

If a device draws more than its supply can give, the voltage sags. When
it falls below what a chip needs, the chip stops working correctly: a
**brown-out**. Many chips notice and hold themselves stopped until the
voltage recovers.

Crashes that nobody can explain, above all when a motor starts, are
often a weak supply or a poor cable.

```check
(choice
  (ask "A small computer crashes each time a motor beside it starts. What is the likely cause?")
  (right "The supply or its cable cannot give enough current, so the voltage sags")
  (wrong "The motor is too loud"
    "Sound does not stop a chip. Starting a motor draws a large current, which can make a weak supply's voltage sag: a brown-out.")
  (wrong "The computer's software has a mistake"
    "Then it would crash without the motor too. That it crashes just when the motor starts points to the supply.")
  (hint "What does a motor draw when it starts?"))
```

---

## Batteries

A battery's **capacity** is usually given in **milliamp-hours** (mAh):
the current it can give, times how many hours. Its energy in
**watt-hours** (**Wh**) is its capacity in amp-hours times its voltage. A
3,000 mAh cell at 3.7 V holds 3 times 3.7, about 11 Wh.

| Type | Cell volts | Rechargeable |
|---|---|---|
| alkaline | 1.5 | no |
| nickel-metal hydride | 1.2 | yes |
| lithium-ion | 3.0 to 4.2 | yes, with a protection circuit |
| lithium iron phosphate | 3.2 | yes, long life |
| lead-acid | 2.0 | yes; heavy |

A rechargeable cell must be charged with a charger made for its kind.

```check
(number
  (ask "A 2,000 mAh battery at 3.7 V. How many watt-hours does it hold?")
  (answer 7.4) (from "2 * 3.7")
  (wrong-if 7400 "That multiplies milliamp-hours by volts. Turn 2,000 mAh into amp-hours first: 2 Ah.")
  (wrong-if 540.54 "That divides. Energy is capacity times voltage.")
  (hint "2,000 mAh is 2 Ah. Then times the voltage."))
```

---

```check
(number
  (ask "A battery holds 10 Wh. A small computer uses 2 W. At most, for how many hours can the battery run it?")
  (answer 5) (from "10 / 2")
  (wrong-if 20 "That multiplies. Watt-hours divided by watts gives hours: 10 / 2.")
  (hint "A watt-hour is one watt for one hour.")
  (hint "10 / 2."))
```

So power is voltage times current, a computer's supply is made from
mains AC by an adapter and regulators, and a battery's energy is its
capacity times its voltage.
