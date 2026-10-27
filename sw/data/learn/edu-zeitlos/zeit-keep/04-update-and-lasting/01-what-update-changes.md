# What update changes

```lesson
(objective "Say what update changes on the card, what it never touches, and why a release can be trusted.")
(minutes 20)
(keywords "release (of Zeitlos)" "update (of the card)" "signature (of a release)" "app set")
```

You will learn what happens to your card, and to your own files, when
the system is brought up to date.

Machdyne publishes Zeitlos in **releases**: versions with names such as
`v0.0.7`, each a complete set of the card's programs, documentation
and data. An **update** brings the card in line with a release by
changing only the files that differ, without writing a new card. The
app that does it is called **update**.

```sources
(source "docs/update.md")
(source "docs/distribution.md")
```

```check
(choice
  (ask "A new release changes 3 programs. What does update write to your card?")
  (right "Those 3 programs, and nothing that is already the same")
  (wrong "The whole card, from the beginning"
    "update compares the card with the release and writes only the files that differ.")
  (wrong "Nothing until you format the card"
    "update needs no new card and no formatting. It changes only what differs.")
  (hint "update compares each file on the card with the release's."))
```

---

## What it never touches

An update leaves alone everything that is yours:

- **`/home`**, and any file the system did not put there;
- a settings file **you have edited**, such as `/sys/zeitlos.cfg`: it
  is replaced only while it is still exactly as it came;
- a file the new release no longer has, **if you changed it**: it is
  yours now, and is left where it is.

Programs and documentation that came with the system are replaced with
the new release's.

```check
(choice (several)
  (ask "You edited /sys/zeitlos.cfg last month. Which of these does an update leave exactly as they are? Choose all that it leaves.")
  (right "/home/novel/book.txt")
  (right "/sys/zeitlos.cfg")
  (wrong "/apps/sheet, when the release has a new sheet"
    "Programs are the release's: a newer one replaces the old.")
  (hint "update never touches what is yours, or what you have changed."))
```

```sources
(source "docs/update.md, What it does with each file and Policies")
```

---

## The card, not the flash

update writes the card only. The core of the system in the board's
flash (the desktop, files, text, term and the others) is not changed by
it: flash is written separately, as the release notes say.

A release's programs talk to that core. If a release is built for a
different core than your flash has, update **refuses** and changes
nothing, and says to write the flash first.

```check
(choice
  (ask "update says it is refused: the release is built for a different core than the one in flash. What has it changed on the card?")
  (right "Nothing")
  (wrong "Half of the files"
    "update checks this before it writes anything, and changes nothing when it refuses.")
  (wrong "Only the programs"
    "When it refuses, it refuses before any file is written.")
  (hint "Is the check made before or after writing?"))
```

```sources
(source "docs/update.md, ABI")
(source "docs/releases.md, Flashing the parts separately")
```

---

## Can it be trusted?

update fetches a release from Machdyne, over the internet. Each
release carries a **signature**: a check value made with a secret key
that only Machdyne has. update checks it against the key it was built
with **before believing anything** in the release, and checks every
file it fetches against the release's list.

So a single change to a file, on the way or at Machdyne's end, stops the update
or that file, and nothing unsigned reaches the card. update also
refuses a release older than the card's when it is told to take the
latest one.

```check
(choice
  (ask "Someone changes a program on the way between Machdyne and your computer. What does update do?")
  (right "Refuses that file: it does not match the release's list, which the signature covers")
  (wrong "Installs it: the file came from the right place"
    "Where a file comes from proves nothing. update checks every file against the release's list, which the signature covers.")
  (wrong "Installs it, then warns you"
    "Nothing that fails the check reaches the card.")
  (hint "What is each file checked against before it is put in place?"))
```

```sources
(source "docs/distribution.md, Why plain HTTP is safe")
```

---

## App sets

Each card file belongs to an **app set**: base, work, audio, net, dev,
embedded, games or arklite. A card has the sets that were chosen for
it; every card has base. An update keeps the sets the card has, and
can add more, but never takes one away.

A program you delete by hand from `/apps` comes back at the next update.

```check
(choice
  (ask "You deleted /apps/chess to make room. What happens at the next update?")
  (right "chess comes back")
  (wrong "chess stays deleted"
    "The card's list still names chess, so update puts it back. Sets are never taken away.")
  (wrong "update refuses to run"
    "A missing file is simply written again.")
  (hint "Does an update ever remove a set's programs?"))
```

```sources
(source "docs/update.md, Sets")
```

So: update changes only what differs, keeps everything that is yours,
leaves the flash alone, and trusts nothing without its signature.
