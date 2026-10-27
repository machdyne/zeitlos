# What a backup keeps

```lesson
(objective "Say what backup copies, what it leaves out, and how its backups are laid out on the drive.")
(minutes 20)
(keywords "backup (a dated copy)" "mirror (a copy kept the same)")
```

You will learn what the **backup** app keeps for you, and where.

A **backup** is a copy of your files on a second drive, made at a known
time, so that if the card fails, or a file is deleted or spoiled, you
can get it back as it was then. The backup app makes backups on a USB
stick, or on an SD card in a USB card reader. Each one is kept, with
its date.

```note
(note "backup is new. As of October 2026 it has been tested on a PC, power cuts included, but not yet run on a Zeitlos board. Until you have seen it work on yours, look at your first backups with Show backups, and keep a copy of your own as well.")
```

```sources
(source "docs/backup.md")
```

```check
(choice
  (ask "Why does a backup go on a second drive, not in another folder on the card?")
  (right "So that it survives if the card itself fails")
  (wrong "Because the card has no room for copies"
    "Room is not the point. A copy on the same card is lost along with the card.")
  (wrong "Because a stick is faster than the card"
    "A stick is slower, not faster. It is used because it is a separate drive.")
  (hint "What happens to every folder on the card if the card stops working?"))
```

---

## Not a mirror

A **mirror** is a copy kept the same as the original. That sounds safe,
but whatever happens to the original happens to the mirror: delete a
file by mistake, and it is deleted from both.

backup is not a mirror. Each backup is kept as it was made. A file you
delete or spoil on the card stays as it was in every earlier backup.

```check
(choice
  (ask "On Wednesday you delete /home/plan.txt by mistake. You made a backup on Monday and another on Thursday. Where can you get plan.txt back from?")
  (right "Monday's backup")
  (wrong "Nowhere: the Thursday backup does not have it, so it is gone"
    "Thursday's backup does not have it, but Monday's is kept as it was made, and it does.")
  (wrong "Thursday's backup"
    "By Thursday the file had been deleted, so that backup did not see it. Monday's has it.")
  (hint "Every backup is kept as it was made. Which one was made while the file was still there?"))
```

```sources
(source "docs/backup.md")
```

---

## What is copied

As it comes, backup copies:

| | |
|---|---|
| `/home` | your own files |
| `/data` | each app's own files: learn's progress, saved games, the dock's layout if you changed it |
| `/sys/zeitlos.cfg` | the system's settings |

It leaves out `/data/ask` (large indexes that can be made again), any
file ending in `.tmp`, and always `/tmp`, `/ram` and `/usb`. The
programs in `/apps` and the documentation in `/docs` are not copied:
they come with the system.

Your password is kept in the board's flash, not on the card, so it is
in no backup.

```check
(choice (several)
  (ask "Which of these are in a backup made with the settings as they come? Choose all that are.")
  (right "/home/novel/chapter1.txt")
  (right "Your progress in learn, in /data/learn")
  (wrong "/tmp/report.md"
    "/tmp is always left out: it is scratch space, emptied at every start.")
  (wrong "/apps/sheet"
    "Programs in /apps come with the system, and backup does not copy them.")
  (hint "backup copies /home, /data and the settings file."))
```

```sources
(source "docs/backup.md, Settings")
(source "docs/kvstore.md")
```

---

## On the drive

Backups go in `/usb/backup`, in a folder for this computer, `zeitlos`.
Each backup is a folder named by the time it started, such as
`2026-10-21_18-02-31`: the date, then the time with dashes, because FAT
allows no colon in a name.

The **first** backup holds every file. Each later one holds only the
files that had changed since. In the figure, each column is one
backup's folder, and a box is a copy kept there.

```figure
(define j (choice 'file '("book" "notes" "budget") "File"))
(keep-backups j)
(caption (string-append "Its newest copy: in the "
                        (list-ref keep-days (keep-newest j)) " folder"))
```

```sources
(source "docs/backup.md, On the drive and Names")
```

---

## The newest copy

So **the newest copy of a file is in the newest folder that has it**.
A file that never changes keeps its copy in an old folder. (Now and
then backup copies such a file again, so that no file depends on one
old copy for ever.)

```check
(choice
  (ask "In the figure, which backup's folder holds the newest copy of notes.txt?")
  (right "19 Oct")
  (wrong "21 Oct"
    "The 21 Oct folder holds only what changed by then, and notes.txt had not. Its newest copy is in the newest folder that has it.")
  (wrong "20 Oct"
    "notes.txt did not change before 20 Oct, so that folder has no copy of it.")
  (hint "Choose notes in the figure, and look for the boxes on its row."))
```

---

```check
(choice
  (ask "Why does each backup after the first copy only the files that had changed?")
  (right "It is quicker and takes far less room, and the older copies are still there")
  (wrong "Because the other files are deleted from the drive"
    "Nothing is deleted: a file that did not change keeps its copy in an earlier folder.")
  (wrong "Because backup cannot read unchanged files"
    "It reads every file's size and date. An unchanged file already has a copy, so it is not copied again.")
  (hint "Where is the copy of a file that did not change?"))
```

```sources
(source "docs/backup.md, How a backup runs and Decisions")
```

---

## Plain files

Everything on the backup drive is plain files under their own names,
and `README.TXT` in `/usb/backup` says how to get them back by hand. Any
computer that reads FAT can open them, with or without Zeitlos.

```check
(choice
  (ask "Your Zeitlos computer is broken. Can you get your files from the backup stick?")
  (right "Yes: on any computer that reads FAT, they are plain files in dated folders")
  (wrong "No: only Zeitlos's backup app can read a backup"
    "backup stores plain files under their own names, so that any computer can read them.")
  (wrong "Only after the Zeitlos computer is mended"
    "The stick is a FAT drive of plain files. Another computer can open it now.")
  (hint "What filesystem does the stick use, and how widely is it read?"))
```

```sources
(source "docs/backup.md, On the drive and Getting files back on a PC")
```

So: a backup is a dated copy of `/home`, `/data` and the settings on a
second drive, and the newest copy of a file is in the newest folder that
has it.
