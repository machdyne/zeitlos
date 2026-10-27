# Storage that lasts

```lesson
(objective "Compare F-RAM, disks, tape, optical discs and paper for keeping data, and choose storage that will still be readable.")
(minutes 20)
(keywords "ferroelectric RAM" "F-RAM" "FRAM" "MRAM" "hard disk" "magnetic tape" "optical disc" "write protection")
```

This lesson is about keeping data for decades, not just until next
week.

**Ferroelectric RAM** (**F-RAM**, or **FRAM**) keeps each bit in the
electric direction of a tiny crystal, which flips one way or the other
when a voltage is put on it and stays that way when the voltage is
taken off. It has the best of RAM and flash:

- it is non-volatile;
- it is written a byte at a time, at full speed, with no erasing;
- it can be rewritten a million million times or more, so in practice
  it does not wear out;
- it keeps data a long time: one maker states about 95 years at 55
  degrees Celsius, and more than 200 years at 35 degrees.

**MRAM** (magnetoresistive RAM) keeps bits magnetically, and is much
like F-RAM: non-volatile, fast, written a byte at a time, and durable.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Ferroelectric RAM")
```

```check
(choice
  (ask "Which of these does F-RAM do better than flash?")
  (several)
  (right "Change one byte with no erase step")
  (right "Survive far more rewrites")
  (wrong "Hold more data for the money"
    "That is flash's strength. F-RAM costs far more per bit and holds kilobytes to a few megabytes.")
  (hint "Think of the three troubles of flash: erasing, wear and fading. And of its one great strength."))
```

---

## Small, but enough

F-RAM's drawbacks are size and cost: chips hold from kilobytes to a
few megabytes, and cost far more per bit than flash. For text,
programs, keys, settings and logs that must last for generations, the
trade is worth it. Machdyne builds its long-term storage around F-RAM:

- **Blaustahl**, a USB device with 8 KB of F-RAM, 7,680 bytes of it
  for text, with its own text editor used from any terminal program;
- **Kaltstahl**, a larger one of 256 KB, which can also act as a USB
  drive;
- **FERRIT**, a system of removable F-RAM cards with room for hundreds
  of megabytes.

All three are open designs, so they can be repaired, rebuilt or read by
other means if their maker is gone.

```check
(number
  (ask "A page of plain text is about 2,000 characters, one byte each. How many whole pages fit in Blaustahl's 7,680 bytes?")
  (answer 3) (true "3 * 2000 <= 7680") (true "4 * 2000 > 7680")
  (wrong-if 4 "4 pages would be 8,000 bytes, more than 7,680.")
  (wrong-if 8 "8 KB is the whole chip, and a page is 2,000 bytes, not 1,000.")
  (hint "7,680 / 2,000, rounded down."))
```

---

## How sure are those years?

Two cautions apply to every retention figure:

- they are makers' estimates from speeded-up tests, often at high
  heat, not chips that have really been kept for two hundred years;
- they depend strongly on temperature: memory kept cool lasts much
  longer than memory kept warm.

```check
(choice
  (ask "A maker says a chip keeps data for 200 years. What is the best way to read that?")
  (right "An estimate from tests, and only at the temperature stated")
  (wrong "A fact: someone has stored such chips for 200 years"
    "The chips are far younger than that. The figure comes from tests that age them faster, usually with heat.")
  (wrong "The same at any temperature"
    "Retention depends strongly on temperature. Warmer storage means a shorter life.")
  (hint "How old can a chip made today be?"))
```

---

## Other media

Not all storage is electronic.

- A **hard disk** stores data magnetically on spinning platters. It is
  cheap and large, but its motor and bearings wear out.
- **Magnetic tape** is still used for archives, because it is cheap and
  stable when kept well, but it needs a working tape drive to read it.
- An **optical disc**, such as a CD, can last decades if it is of good
  quality and kept dark and cool.
- **Paper** is storage too. A printed program, key or set of
  instructions, kept dry, can be read in a century without any machine.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Other Long-Term Media")
```

```check
(choice
  (ask "Which of these can be read in a hundred years with no machine at all?")
  (right "A printout on paper, kept dry")
  (wrong "Magnetic tape"
    "Tape can keep data a long time, but reading it needs a tape drive that still works.")
  (wrong "An F-RAM chip"
    "F-RAM keeps its bits for a long time, but reading them needs a circuit that speaks to the chip.")
  (hint "Which one do you read with your eyes?"))
```

---

## Choosing storage that lasts

Besides how long it keeps its bits, two things decide how long data
stays usable:

- **Write protection**: data that should not change is guarded from
  being written by mistake. Many memory chips and devices have a
  write-protect pin or switch.
- **An interface that can still be read**: storage is useful only while
  something can still talk to it. Simple, well-documented interfaces,
  with the documents kept beside the data, make that likely.

```check
(choice
  (ask "An archive is on a drive whose maker kept its interface secret, and the maker is gone. What is the danger?")
  (right "The bits may be fine, but nothing may be left that can read them")
  (wrong "None, as long as the drive's memory keeps its charge"
    "Bits nobody can read are as good as lost. Storage is useful only while something can still talk to it.")
  (wrong "The drive will erase itself"
    "Nothing erases it. The trouble is reading it, with no documents and no maker.")
  (hint "Good bits are not enough. What else do you need to get them out?"))
```

So: F-RAM and paper outlast flash, retention figures are estimates, and
data lasts only while something can still read it.
