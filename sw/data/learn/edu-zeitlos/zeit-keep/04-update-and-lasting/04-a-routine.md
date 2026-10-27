# A routine for keeping data

```lesson
(objective "Plan how many copies of your work to keep, where, and how often to make and check them.")
(minutes 20)
(keywords "refresh (copies)" "plain format (of a file)")
```

You will learn a routine that keeps important work safe for years.

Keep **at least three copies** of anything important, on at least two
kinds of medium, with **one copy in another place**. Choose what goes
wrong in the figure, and count the copies left. Then tick Mirror, to make the
stick a mirror, and choose file again.

```figure
(define e (choice 'event '("card" "file" "fire") "Loss"))
(define m (toggle 'mirror "Mirror"))
(keep-copies-fig e m)
(caption (string-append (list-ref '("The card stops working." "A file is deleted by mistake."
                     "A fire at home.") e)
                        (if m " The stick is a mirror." "")))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

```check
(choice
  (ask "A fire destroys everything at home. Which copy of your work is left?")
  (right "Only the copy kept somewhere else")
  (wrong "The backup stick, since it is a separate drive"
    "A separate drive in the same house burns with the computer. That is why one copy is kept elsewhere.")
  (wrong "None: a fire takes every copy"
    "Not the copy kept in another place.")
  (hint "Choose fire in the figure."))
```

---

## Not always connected

A copy that is always connected, or kept the same as the original by
itself, is not a backup in this sense: a file deleted, spoiled or
scrambled by faulty or hostile software in one place is changed in the
other as well. That is why backup makes dated backups, and why the stick
is best unplugged between backups.

```check
(choice
  (ask "In the figure, the stick is a mirror and a file is deleted by mistake. How many copies of the file are left?")
  (right "1: only the copy kept elsewhere")
  (wrong "2: the stick and the copy elsewhere"
    "A mirror follows the card, so the file is deleted from the stick too.")
  (wrong "3: nothing was lost"
    "The file is gone from the card, and the mirror followed it.")
  (hint "Tick Mirror in the figure, and choose file."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

---

## A routine

A routine makes it happen:

- **after each session**, back up your work in progress to a stick:
  Back up now in the Backup window;
- **every week or month**, copy everything to a medium that is then
  unplugged and kept apart, in another place: for example a second stick, copied
  with files;
- **keep a list** of what is stored where, so that every copy can be
  found.

```check
(choice
  (ask "How often does the routine back up your work in progress?")
  (right "After each session")
  (wrong "Once a year"
    "A year's work could be lost. Work in progress is backed up after each session.")
  (wrong "Only when something has gone wrong"
    "By then it is too late: the copy has to exist before the loss.")
  (hint "How much work are you willing to lose?"))
```

---

## Check, and refresh

A copy nobody has read may be unreadable just when it is needed:

- **read it back**: backup reads every copy back as it makes it, and
  `run backup check -a` at the console reads every copy on the stick
  and checks it;
- **restore a file now and then**, to see that it works;
- **refresh**: copy your data onto new media before the old reach the
  end of their expected life.

```check
(choice
  (ask "Your second stick has held your archive for eight years. What does the routine say to do?")
  (right "Copy the archive onto a new medium before the stick reaches the end of its life")
  (wrong "Nothing, while it still reads"
    "Flash loses data slowly and can fail at once. Refresh onto new media in good time.")
  (wrong "Plug it in every day to keep it fresh"
    "Being plugged in does not renew it. Copying onto new media does.")
  (hint "Flash typically keeps data ten to twenty years."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
(source "docs/backup.md, Using it")
```

---

## Plain formats

Use **plain formats**: plain text for writing, and simple, widely
documented formats for the rest. They remain readable long after any
particular program is gone. Keep the smallest, most vital things, such
as passwords and essential notes, on paper or on media built to last.

```check
(choice
  (ask "Which of these will most likely still be readable in fifty years?")
  (right "A book written as plain text, in several copies")
  (wrong "A book in the newest format of one word processor"
    "A program's own format needs that program, or one that understands it. Plain text needs almost nothing.")
  (wrong "A book kept on one stick, in a safe"
    "One copy, on flash, is one failure away from nothing. Plain text in several copies is far safer.")
  (hint "Think of both the format and the number of copies."))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

---

```check
(choice (several)
  (ask "Which of these are part of keeping data safe for years? Choose all that are.")
  (right "Three copies, one of them in another place")
  (right "Reading copies back, and restoring a file now and then")
  (right "Copying onto new media in good time")
  (wrong "One mirror, always connected"
    "A mirror follows every mistake on the original. Dated copies, kept apart, do not.")
  (hint "Three of the four come from the routine."))
```

So: three copies, two kinds of medium, one elsewhere, in plain formats,
read back and refreshed in good time.
