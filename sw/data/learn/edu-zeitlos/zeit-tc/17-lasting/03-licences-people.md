# Licences and people

```lesson
(objective "Tell the main kinds of open licence apart, and say why people are the most durable part of a lasting system.")
(minutes 15)
(keywords "permissive licence" "copyleft" "public domain")
```

You will see how openness is made lawful, and who keeps a system alive.

A design or program that can be read may still not be lawfully copied
or changed. Openness is given legal force by a **licence**: the terms
on which the owner of a work's copyright, the right in law to copy it,
lets others use, copy, change
and share it.

- **Permissive licences**, such as the MIT, BSD and Apache licences,
  and the Lone Dynamics Open License used for most of Machdyne's
  designs and software, allow nearly any use, even in closed products,
  as long as the original notice is kept.
- **Copyleft** licences, such as the GNU General Public License (GPL),
  allow the same, on condition that changed versions which are passed
  on are released under the same licence, with their source.

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Licences")
```

```check
(choice
  (ask "You change a program under the GPL and give copies of your version to others. What must you do?")
  (right "Release your version under the GPL too, with its source")
  (wrong "Nothing: open software can be used any way at all"
    "That is closer to a permissive licence. The GPL is copyleft: changed versions passed on must stay under it, with their source.")
  (wrong "Ask the original author's permission"
    "The licence already gives permission, on its conditions. The condition here is releasing your version under the GPL, with its source.")
  (hint "The GPL is a copyleft licence."))
```

---

## The public domain

A work can also be **dedicated to the public domain**, with no
conditions at all. The book this course follows is, under CC0, the
Creative Commons public domain dedication.

Patents and trademarks are separate from copyright: a patent can limit
building an invention whatever its licence, and a trademark protects a
product's name, not its design.

For the owner of a lasting computer, the test is simple: the licences
of its designs and software must let anyone repair, change and rebuild
it, without asking permission.

```check
(choice
  (ask "A program is under a permissive licence. A company puts it, changed, into a closed product. Is that allowed?")
  (right "Yes, as long as the original notice is kept")
  (wrong "No: open software must stay open"
    "That is the copyleft condition. A permissive licence allows use in closed products, keeping the original notice.")
  (wrong "Only if the program is in the public domain"
    "A permissive licence already allows it. The one condition is keeping the original notice.")
  (hint "What does a permissive licence ask for?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Licences")
```

---

## People

The most durable store of knowledge is people. A machine that only one
person understands depends on that person. So knowledge is taught,
written down and shared among people who use and look after the same
kinds of systems. Open designs make that possible: anyone can study
them, and no permission is needed to repair, copy or improve them.

```check
(choice
  (ask "A town's water pump controller is understood by one engineer, who is retiring. What most helps it last?")
  (right "Writing down how it works, and teaching others to look after it")
  (wrong "Buying a spare controller"
    "A spare helps with a failed part, but no one would know how to fit or program it. The knowledge must be passed on.")
  (wrong "Keeping it switched on so it never needs restarting"
    "It will need attention one day. What keeps it alive is people who understand it.")
  (hint "What leaves with the engineer?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: People")
```

---

## A checklist

- Know how much power the equipment needs, and have a way to supply it
  without the grid.
- Keep equipment cool, dry, clean and in an enclosure.
- Keep spares of the parts that wear out, and the tools and schematics
  to repair the rest.
- Keep at least three copies of important data, in plain formats, and
  check them.
- Keep the source, the tools and the instructions to rebuild the
  system, and test them.
- Keep a known good system image and a spare configuration flash.
- Keep an offline library of what the work depends on.
- Learn how the machines work, and teach others.

```check
(choice (several)
  (ask "Which of these are on the checklist?")
  (right "Keep a spare configuration flash and a known good system image")
  (right "Learn how the machines work, and teach others")
  (wrong "Keep every machine connected to the Internet for updates"
    "The checklist plans for when the Internet is not there: an offline library, copies, the means to rebuild.")
  (wrong "Replace the computer every few years"
    "The aim is equipment that lasts and can be repaired, not replaced.")
  (hint "Two are on it; the others work against lasting."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: A Checklist")
```

---

## Where to begin

Each step towards timeless computing is useful by itself, and none
needs special equipment. You can begin with what you have: write a
short program in BASIC or Scheme, keep your writing in plain text files
and make a checked copy of them, or study how one familiar device works
inside. With more experience: build a small controller that does one
job for years, read the source of a system you use, add a device to an
FPGA design, or move a design to another board.

```check
(choice
  (ask "Which of these is a good first step towards computing that lasts?")
  (right "Keep your writing in plain text files, with a checked copy")
  (wrong "Wait until you can build a whole FPGA computer"
    "Every step is useful by itself. Plain text and a checked copy can start today.")
  (wrong "Buy the newest computer available"
    "Newness is not what lasts. Plain formats, copies and understanding are.")
  (hint "Which needs nothing special and helps at once?"))
```

So: a computer lasts when its licences let anyone repair it and the
people who depend on it understand how it works.
