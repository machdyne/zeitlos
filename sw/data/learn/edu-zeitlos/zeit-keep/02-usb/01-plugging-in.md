# Plugging in a stick

```lesson
(objective "Plug in a USB stick, find it in files, and tell why a stick might not appear.")
(minutes 15)
(keywords "USB stick (a drive)" "filesystem (of a drive)" "FAT (filesystem)" "exFAT")
```

You will learn how to reach a USB stick from Zeitlos, and which sticks
it can read.

A **USB stick** is a small drive that plugs into a USB socket. Plug one
in, and after a moment Zeitlos makes it the drive `/usb`, by itself:
there is nothing to click. An SD card in a USB card reader works the
same way.

In **files**, `/usb` then appears at the top of the card, beside the
folders there. If files was already open, press **F5** to read the
folder again.

```sources
(source "docs/filesystem.md, Volumes")
(source "docs/file_browser.md, Using it")
(source "docs/backup.md, Drives")
```

```check
(text
  (ask "You plug in a stick. What is the path of the drive it becomes?")
  (accept "/usb") (accept "/usb/")
  (wrong-if "usb" "Nearly: a path starts with / for the top.")
  (wrong-if "/ram" "/ram is the RAM disk, made of memory. A stick is the other drive at the top.")
  (hint "It is named after the kind of socket."))
```

---

## Filesystems

A drive is one long row of blocks. A **filesystem** is the set of rules
that turns those blocks into named files and folders: which blocks
belong to which file, which are free, and each file's name, size and
date.

**FAT** is the most widely understood filesystem in the world. Nearly
every computer and camera can read it. The card in a
Zeitlos computer is FAT, and Zeitlos reads sticks formatted as FAT (as
FAT12, FAT16 or FAT32).

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Filesystems")
(source "docs/backup.md, Drives")
```

```check
(choice
  (ask "Why is FAT a sensible filesystem for a stick that carries your files?")
  (right "Nearly every computer can read it")
  (wrong "It is the newest filesystem"
    "FAT is old: it was first made for floppy disks. Its strength is that almost everything can read it.")
  (wrong "It makes the stick hold more"
    "A filesystem arranges the space; it does not add to it. FAT is chosen because it is so widely read.")
  (hint "Think of taking the stick to another computer."))
```

---

## A stick that does not appear

Many sticks larger than 32 GB come formatted as **exFAT**, a newer
filesystem that Zeitlos cannot read. Neither can it read NTFS. Such a
stick is never made `/usb`: it simply does not appear.

A stick can be formatted as FAT32 to make it readable, but **formatting
erases everything on it**. Copy off anything you need first.

```check
(choice
  (ask "You plug in a new 64 GB stick. /usb does not appear in files, even after F5. What is the likely reason?")
  (right "The stick is formatted as exFAT, which Zeitlos cannot read")
  (wrong "The stick is full"
    "A full stick still appears: you could open its files. A stick that never appears is one Zeitlos cannot read.")
  (wrong "Sticks have to be opened from the dock"
    "There is nothing to open: Zeitlos makes a stick /usb by itself, if it can read it.")
  (hint "Many large sticks come with a filesystem other than FAT."))
```

```sources
(source "docs/backup.md, Drives")
```

---

## Try it

```try
(try (app "files")
  (task "Plug a FAT stick into a USB socket. In files, go to the top of the card and look for usb (press F5 if it is not there yet). Open it, look at what is on it, then come back.")
  (check (choice
    (ask "files was open before you plugged the stick in, and usb is not in the list. What do you press?")
    (right "F5, to read the folder again")
    (wrong "F1"
      "F1 shows the help box. F5 reads the folder again, and then usb is listed.")
    (wrong "F2"
      "F2 renames the selected file. F5 reads the folder again."))))
```

```sources
(source "docs/keys.md")
```

---

## Carrying your progress

A stick also carries things between computers. learn, for example, can
keep a learner on a stick, in `/usb/learn`: plug the stick in and learn
offers that learner; take it out and nothing of theirs is left behind.

```check
(choice
  (ask "A class shares one computer, and each learner keeps their progress on their own stick. Where does learn write a learner's progress?")
  (right "On that learner's stick, in /usb/learn")
  (wrong "In /data/learn on the card"
    "That is where learners made on the machine are kept. A learner on a stick is kept on the stick.")
  (wrong "Only in memory, until the stick is plugged in again"
    "learn writes to the stick as the learner goes, so it is kept even if the power goes off.")
  (hint "The point is that nothing of theirs is left on the shared computer."))
```

```sources
(source "docs/learn.md, Learners")
```

So: a FAT stick becomes `/usb` as soon as it is plugged in, and a stick
that never appears is most likely exFAT.
