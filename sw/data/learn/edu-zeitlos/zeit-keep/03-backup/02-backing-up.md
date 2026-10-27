# Backing up

```lesson
(objective "Make a backup with the backup window, stop one safely, remove the drive, and have each save backed up as you work.")
(minutes 20)
```

You will learn to make a backup in one press, and what is safe while it
runs.

Open **Backup** from the dock: it is in the **Tools** group, its icon
three dated copies and a clock. Its window answers one question, "am I
backed up?", in its top line:

| It says | Meaning |
|---|---|
| Plug in the backup drive | no stick is plugged in |
| No backups yet | a stick with no backups on it |
| Backed up today 14:40 | the newest backup, and when |

```sources
(source "docs/backup.md, Using it and The window")
```

```check
(choice
  (ask "You open Backup and it says: Plug in the backup drive. What is wrong?")
  (right "No USB stick is plugged in, or one Zeitlos cannot read")
  (wrong "There are no backups on the stick yet"
    "Then it would say No backups yet. Plug in the backup drive means it finds no drive at all.")
  (wrong "The card is full"
    "Backups go on the stick, not the card. The window is asking for the stick.")
  (hint "Which drive does backup write to?"))
```

---

## Back up now

Press **Back up now** (or Enter). The top line says "Backing up...",
with a bar and "Keep the power on". It looks at every file, copies
the ones that changed, and reads each copy back to check it before the
backup counts. Then it says when it was backed up.

A backup only **reads** the card: it writes nothing there. **Esc**, or
Stop, ends it at once, and that is safe: a backup stopped part way does
not count, and the next one tidies up what it left.

```note
(note "The boards have no battery for their clock. Without the internet to set it, a backup is named after the one before it, with +0001 added, and shown as after that time (clock not set).")
```

```check
(choice
  (ask "A backup is half way through, and you need the computer for something else. What do you do?")
  (right "Press Esc: stopping is safe, and the next backup carries on from the backups that are complete")
  (wrong "Nothing: a backup cannot be stopped"
    "Esc (or Stop) ends a backup at once.")
  (wrong "Switch the computer off and on"
    "Stopping is what Esc is for. The power should stay on while a backup writes to the stick.")
  (hint "Which key stops something in most windows?"))
```

```sources
(source "docs/backup.md, The window, How a backup runs and Names")
```

---

## If the power goes

backup is built so that a power cut costs nothing that was already
safe:

- the card is only read, so it is as your programs left it;
- on the stick, nothing that is already there is ever changed: a backup
  only adds files;
- a backup counts only when its list of files is complete, written
  last.

```check
(choice
  (ask "The power fails in the middle of a backup. What about the backups made last week?")
  (right "They are untouched: a backup only adds files")
  (wrong "They may be damaged, because the stick was being written"
    "A backup never changes what is already on the stick, so earlier backups are left as they were.")
  (wrong "They are lost, because the newest backup did not finish"
    "Each backup is kept as it was made. One cut short simply does not count.")
  (hint "Does a backup ever change files already on the stick?"))
```

```sources
(source "docs/backup.md, The rules that make a power cut harmless")
```

---

## Remove drive

When the backup is done, **Remove drive** lets go of the stick, as
`usbunmount` does, and the window says "You can pull out the drive
now". While a backup is still writing, it says "Not yet" instead.

```check
(choice
  (ask "The backup has finished. What is the safe way to take the stick out?")
  (right "Press Remove drive, wait for You can pull out the drive now, then pull it out")
  (wrong "Pull it out: the backup has finished, so nothing is writing"
    "Nothing should be writing, but letting the system finish with the drive first is the safe habit.")
  (wrong "Close the Backup window, then pull it out"
    "Closing the window does not let go of the drive. Remove drive does.")
  (hint "One of the window's buttons is for exactly this."))
```

```sources
(source "docs/backup.md, The window")
```

---

## Each save, as you work

In **Settings** (S) in the backup window, **Back up each save** takes
folders you work in, such as `/home/book`. While the window is open,
each file saved there is backed up on its own, 10 seconds after the
last save. Make the window small with its **Aa** icon and leave it in a
corner.

It works only while the window is open, and only while the stick is
in. Back up now still backs up everything.

```check
(choice
  (ask "You set Back up each save to /home/book, then close the Backup window and write all afternoon. Are the afternoon's saves backed up?")
  (right "No: it works only while the window is open")
  (wrong "Yes: it was set, so it runs in the background"
    "Nothing runs in the background for it. Closing the window stops it.")
  (wrong "Yes, the next time the computer starts"
    "Nothing is kept for later. The next Back up now will copy them.")
  (hint "Where does the watching happen?"))
```

```sources
(source "docs/backup.md, Watch")
```

---

```try
(try (app "files")
  (task "Plug in a FAT stick. Open Backup from the dock's Tools group and press Back up now. When it says Backed up, press Show backups: files opens at /usb/backup. (Or open files here and go to /usb/backup.) Look in it, then come back.")
  (check (choice
    (ask "What is in /usb/backup, beside the folder of backups?")
    (right "README.TXT, which says how to get files back")
    (wrong "Nothing else"
      "Look again: backup puts README.TXT there, saying how to get files back by hand.")
    (wrong "A copy of /apps"
      "Programs are not backed up: they come with the system."))))
```

```sources
(source "docs/backup.md, The window")
```

So: plug in the stick, press Back up now at the end of each session,
and Remove drive before you take it out.
