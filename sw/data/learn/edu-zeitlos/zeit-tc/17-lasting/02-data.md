# Spares, data and rebuilding

```lesson
(objective "Plan spares, copies of data and the means to rebuild a system, so that it can be restored after a failure.")
(minutes 20)
(keywords "recoverability" "offline library")
```

You will plan what to keep so that a system can always be brought back.

Replacements are hardest to find when they are needed, so get them
first:

- **spares of what wears out or gets lost**: storage cards,
  configuration modules, cables, power supplies, small modules;
- **standard connectors and voltages**, such as USB power, Pmod
  sockets and common batteries;
- **socketed and modular parts**, which can be replaced by hand;
- **basic tools**: a multimeter, a soldering iron, small screwdrivers
  and a magnifier;
- **schematics, data sheets and documents** for every board, in a form
  that can be read without the board working.

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Spares and Parts")
```

```check
(choice
  (ask "Why keep a board's schematic on paper, or on another machine?")
  (right "It is most needed when the board does not work, and then it cannot be read from the board")
  (wrong "Paper keeps data better than any electronic medium"
    "That is not the point here. A schematic is needed to repair the board, which may be the very machine that will not start.")
  (wrong "Schematics are too large to keep on the board"
    "Size is not the trouble. The board that holds it may be the one that is broken.")
  (hint "When is a schematic needed most?"))
```

---

## Copies of data

- Keep **at least three copies** of anything important, on at least
  two kinds of medium, with one copy in another place.
- Use **plain formats**: text for writing, and simple, well-documented
  formats for the rest.
- Record a **hash** of each important file, check the copies against
  it now and then, and replace any copy that has failed.
- **Refresh** copies onto new media before the old reach the end of
  their life. Flash cards and USB sticks are for moving data, not for
  keeping it for decades.
- Keep small, vital data, such as keys and essential notes, on durable
  media such as F-RAM, and on paper.

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

```check
(choice
  (ask "A copy's hash no longer matches the hash recorded when it was made. What does that mean?")
  (right "The copy has changed or failed, and should be replaced from a good copy")
  (wrong "The hash was recorded wrongly, so record a new one from the copy"
    "That would hide the damage. A changed hash means the file changed; replace the copy from one whose hash still matches.")
  (wrong "Nothing: hashes change as files age"
    "A hash depends only on the file's contents. If it changed, the contents changed.")
  (hint "A hash changes only when the contents change."))
```

---

## What counts as a backup

Work in progress is copied daily or after each session to a second
medium; everything is copied weekly or monthly to a medium that is then
disconnected and stored apart.

A copy that is always connected, or a folder kept in step with another
machine by itself, is not a backup in this sense: a file deleted,
damaged or encrypted by faulty or hostile software in one place is
changed in the other as well. And a backup is tested by restoring files
from it, now and then, because an unreadable copy is found out only
when it is needed.

```check
(choice
  (ask "A folder is copied to another machine automatically every time it changes. A faulty program scrambles a file. What happens to the other copy?")
  (right "It is scrambled too, as soon as the change is copied")
  (wrong "It stays safe, since it is on another machine"
    "The copy follows every change, including the damage. A backup is a copy that is disconnected and stored apart.")
  (wrong "The other machine notices and refuses the change"
    "Automatic copying does not know a good change from a bad one. It copies the scrambled file.")
  (hint "Does the copy follow every change?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

---

```check
(number
(ask "Using the rule of three copies, two media, one elsewhere: you have the original on a computer and one copy on a USB stick in the same room. How many more copies do you need, at least?")
(answer 1)
(wrong-if 2 "Three copies in all is the rule, and you have two. One more, kept in another place, completes it.")
(wrong-if 0 "Two copies are not three, and none is in another place.")
(hint "Count the copies you have, and the rule's number."))
```

---

## The means to rebuild

Software does not wear out, but it can become unusable: the programs
that build it stop running, the sites that held it vanish, and the
knowledge of how it was made is lost. So keep three things together,
offline, in more than one place:

- **the source code** of every program and design in use;
- **the tools** that build it, as source too if possible, in versions
  known to work;
- **written instructions** for building and restoring everything,
  tested by following them.

A machine that can rebuild itself, compiling its own software and its
own FPGA design, needs no other computer. Few systems do it, and
Zeitlos does not: its compiler and FPGA tools on the machine build
simple programs and designs, not the system itself. So the tools that
build it, and a computer that runs them, are part of what is kept.

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Software and the Means to Rebuild It")
(source "Zeitlos documentation: zcc (docs/zcc.md), zfpga (docs/zfpga.md)")
```

```check
(choice
  (ask "You keep the source code of every program, but not the tools that build them. What is the risk?")
  (right "In years to come, nothing may be able to turn the source into working programs")
  (wrong "None: source code is all that is needed"
    "Source has to be built. If the tools that build it stop running, or vanish, the source cannot become programs.")
  (wrong "The source code will slowly change"
    "Stored source does not change. The risk is losing the means to build it.")
  (hint "What turns source code into a program?"))
```

---

## Recoverability

**Recoverability** is a known good state that can be restored in
minutes. It needs a spare copy of the configuration flash and of the
system image, and a restore practised before it is needed.

An **offline library** helps too: reference works, manuals and books
kept where they can be read without a network. Machdyne's Ark project
prepares such collections, from a compressed text file of about 2 MB
to several gigabytes.

```check
(choice
  (ask "When is the right time to practise restoring the system?")
  (right "Before anything has gone wrong")
  (wrong "Only when the system has failed"
    "A restore tried for the first time during a failure is when its mistakes cost most. Practise it beforehand.")
  (wrong "Never: a spare image is enough"
    "An image no one has restored may not work, or no one may know how. Recoverability needs a practised restore.")
  (hint "When would you rather find a mistake in the restore?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Software and the Means to Rebuild It; Keeping Information")
```

So: keep spares, three tested copies of your data, and the source,
tools and instructions to rebuild, and practise the restore.
