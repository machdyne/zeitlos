# Storage that lasts

```lesson
(objective "Say how long cards, sticks and other media keep data, what shortens it, and which media suit keeping data for decades.")
(minutes 20)
(keywords "retention (of data)" "controller (in a stick)" "F-RAM" "ferroelectric RAM" "hard disk" "magnetic tape" "optical disc")
```

You will learn how long the things you store data on can be trusted,
and which suit keeping it for decades.

SD cards and USB sticks are made of flash memory. Three things about
flash decide how it is used:

- **It must be erased before it is written**, in large blocks.
- **It wears out**: each block survives a limited number of erases.
- **It slowly loses data**: the charge that holds the data leaks away.
  How long data is kept is its **retention**. For flash it is typically
  ten to twenty years, and less when it is hot or worn.

```check
(choice
  (ask "You copy your photos to a new stick and put it in a drawer. How long can you count on them being readable?")
  (right "Typically ten to twenty years, and less if the drawer is hot")
  (wrong "For ever, since nothing is writing to it"
    "Flash loses its data slowly even when nobody uses it: the charge leaks.")
  (wrong "About a year"
    "Flash typically keeps its data for ten to twenty years. The risk is in relying on it for longer.")
  (hint "Retention of flash is given in years, and it depends on heat."))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Flash Memory")
```

---

## The controller

Inside every card and stick is a small **controller**: a chip that
hides the erasing, spreads the writes across the flash to even out the
wear, and replaces bad blocks. If the controller fails, everything
behind it can become unreachable at once, without warning.

So cards and sticks are for **moving** data, and for backups that are
made again and again, but not for keeping data for decades.

```check
(choice
  (ask "Why is a stick a poor place to keep the only copy of a family history for thirty years?")
  (right "Its controller can fail without warning, and its flash slowly loses data")
  (wrong "Sticks can only be written a few times"
    "Wear matters, but a drawer stick is hardly written. The dangers are a failed controller and leaking charge.")
  (wrong "Nothing will be able to read FAT in thirty years"
    "FAT is the most widely read filesystem there is. The stick itself is the weak part.")
  (hint "Two things: the chip that runs the stick, and the charge in the flash."))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Flash Memory")
```

---

## Keep it cool

Heat is the main cause of ageing in electronics. A rough rule for many
components is that every 10 degrees Celsius more halves their life.
Media kept cool, and dry, last longest.

```check
(number
  (ask "By the rough rule, a part would last 20 years at 25 degrees Celsius. About how many years at 35 degrees?")
  (answer 10) (from "20/2")
  (wrong-if 40 "Heat shortens life. 10 degrees more halves it.")
  (wrong-if 5 "That is two halvings, for 20 degrees more. 35 is 10 degrees above 25.")
  (hint "35 is 10 degrees warmer than 25. What does 10 degrees more do?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: The Environment")
```

---

## Media built to last

**F-RAM** (**ferroelectric RAM**) keeps its data in tiny crystals. It
needs no erase, can be rewritten a million million times, and keeps its
data a long time: one maker specifies around 95 years at 55 degrees
Celsius, and more than 200 at 35. It is small and costly, so it suits
text, keys, settings and notes, not photos. Machdyne's Blaustahl and
Kaltstahl are F-RAM devices.

Such figures are makers' estimates from tests that speed up ageing, not
observations over centuries.

```check
(choice
  (ask "What is F-RAM best used for?")
  (right "Small, vital data such as keys, settings and important notes")
  (wrong "A large photo collection"
    "F-RAM holds little and costs a lot for its size. It is for small, vital data.")
  (wrong "Running programs, in place of memory"
    "It is used to keep data, a little of it, for a very long time.")
  (hint "Think about its size and its cost."))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Ferroelectric RAM")
```

---

## Other media

Not all storage is flash. A **hard disk** stores data magnetically on
spinning platters; **magnetic tape** stores it magnetically on a long
tape; an **optical disc** (a CD or DVD, say) stores it as marks read by
light.

| Medium | Strength | Keep in mind |
|---|---|---|
| hard disk | cheap and large | its motor and bearings wear out |
| magnetic tape | stable when stored well | needs a working tape drive |
| optical disc | decades, if of good quality | keep it dark and cool |
| paper | a century, kept dry | read with no machine at all |

A short program, a key or a set of instructions printed on paper is a
serious option for the most important things.

```check
(choice
  (ask "Which can still be read in a hundred years with no machine at all?")
  (right "A printout, kept dry")
  (wrong "An optical disc"
    "A good disc can last decades, but it needs a drive to read it.")
  (wrong "Magnetic tape"
    "Tape needs a working tape drive. Paper needs only eyes.")
  (hint "Which needs nothing but light to read?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Other Long-Term Media")
```

---

## Can it still be read?

Data is useful only while something can read it. Simple, widely known
interfaces and formats, such as FAT and plain text, and the
documentation kept with the data, make that likely for a long time.
Data that should not change is best protected from writing, for
example by a stick's or card's write-protect switch where it has one.

```check
(choice
  (ask "You keep your notes for the long term. Which way of storing them is most likely to be readable in thirty years?")
  (right "Plain text files, on a widely read filesystem")
  (wrong "The format of one particular program"
    "When that program is gone, so may be the way to read its files. Plain text needs only a way to show letters.")
  (wrong "Whatever is newest today"
    "New does not mean lasting. Simple, widely known formats outlast the programs that wrote them.")
  (hint "Which format can almost anything show?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Choosing Storage That Lasts")
```

So: cards and sticks are for moving data and for backups made again and
again; keep media cool, and keep the most vital things on media built
to last, and on paper.
