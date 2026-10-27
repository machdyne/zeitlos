# Getting files back

```lesson
(objective "Get a file back from a backup, by copying it in files or with backup restore at the console.")
(minutes 20)
(keywords "restore (a file)")
```

You will learn to get a file back, as it was at any backup.

In the Backup window, **Show backups** (or B) opens files at
`/usb/backup`. Go into `zeitlos`: each folder there is one backup,
named by its date, and holds the changed files under their own paths,
such as `home/book.txt`.

The newest copy of a file is in the newest folder that has it. Copy it
back with **Copy**, the way you copy anything.

```sources
(source "docs/backup.md, The window and On the drive")
```

```check
(choice
  (ask "You want the newest backed-up copy of /home/notes.txt. The newest backup folder has no home/notes.txt in it. Why not?")
  (right "notes.txt had not changed since an earlier backup, which holds its copy")
  (wrong "notes.txt was never backed up"
    "A later backup holds only what changed. Look in the older folders: the newest one that has it holds the newest copy.")
  (wrong "The backup failed"
    "A backup that worked still holds only the changed files. That is how backup saves time and room.")
  (hint "What does each backup after the first hold?"))
```

---

## Beside, not over

Copy the old version **beside** the file on the card, under a new name,
rather than over it. Then you can compare the two, and nothing is lost
if you picked the wrong one.

```example
(example
  (problem "Yesterday's version of /home/book.txt was better. Yesterday's backup is 2026-10-20_09-15-40.")
  (step "In files, open /usb/backup/zeitlos/2026-10-20_09-15-40/home and select book.txt.")
  (step "Press Copy. The line holds the file's own path on the stick.")
  (step "Change it to /home/book-old.txt and press Enter.")
  (answer "/home/book-old.txt is yesterday's version, beside today's /home/book.txt."))
```

```check
(choice
  (ask "Why copy the old version to /home/book-old.txt rather than over /home/book.txt?")
  (right "Today's version stays too, in case the old one is not the one you wanted")
  (wrong "files cannot copy over a file"
    "It can: it asks first, then replaces it. Copying beside is simply safer.")
  (wrong "Because a backup cannot be copied to /home"
    "A backup is plain files, which copy anywhere.")
  (hint "After a copy over the file, what is left of today's version?"))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

## backup restore

At the console (term, then CONSOLE) one command finds the newest copy
for you. To **restore** a file is to put a backed-up copy of it back
on the card:

```
> run backup restore /home/book.txt
```

It does not write over the file on the card: the copy goes beside it,
named after its backup, such as `/home/book (2026-10-21 11-30).txt`.
Add a date to take an older backup, and give a folder to restore all of
it:

```
> run backup restore /home/book.txt 2026-10-19
> run backup restore /home
```

```check
(text
  (ask "Type the command, at the console, that restores the newest copy of /home/plan.txt.")
  (accept "run backup restore /home/plan.txt")
  (wrong-if "backup restore /home/plan.txt" "At the console a program is started with run: run backup restore /home/plan.txt.")
  (wrong-if "run backup /home/plan.txt" "Nearly: say what backup should do with it. The word is restore.")
  (wrong-if "run backup restore plan.txt" "Give the whole path, from the top of the card: /home/plan.txt.")
  (hint "run, then the app, then what it should do, then the file's path.")
  (hint "run backup restore, then /home/plan.txt."))
```

```sources
(source "docs/backup.md, Using it")
```

---

## Every copy of a file

`run backup history /home/book.txt` lists every copy of the file the
backups hold, with its date and size, so you can choose which one to
restore.

```check
(choice
  (ask "You restore /home/book.txt and a new file appears: /home/book (2026-10-19 18-00).txt. What happened to /home/book.txt?")
  (right "Nothing: the restored copy is beside it")
  (wrong "It was replaced by the old version"
    "restore puts the copy beside the file, named after its backup. The file on the card is left as it was.")
  (wrong "It was moved onto the stick"
    "Restoring only reads the stick and writes the copy beside your file.")
  (hint "Look at the name of the new file."))
```

```sources
(source "docs/backup.md, Using it")
```

---

## When the stick fills up

When the stick is too full for the next backup, backup makes room by
removing the **oldest copies first**, but only copies that have a newer
copy in a later backup. The newest copy of every file is kept, and so
are the 10 newest backups whole. So old backups shrink to the files
that never changed since.

```check
(choice
  (ask "The stick is full. Which copy of book.txt can backup remove to make room?")
  (right "An old copy, made long before the 10 newest backups, when a newer copy exists")
  (wrong "The newest copy, since it is the biggest"
    "The newest copy of a file is never removed: it may be the only good one.")
  (wrong "Any copy at all, oldest first"
    "Only copies that have a newer copy elsewhere, and never from the 10 newest backups.")
  (hint "Which copy of a file must never be lost?"))
```

```sources
(source "docs/backup.md, Making room")
```

---

```check
(choice
  (ask "Why get a file back from your backups now and then, even when nothing is lost?")
  (right "To be sure the backups can really be read before you need them")
  (wrong "Because backups expire unless they are opened"
    "Backups do not expire. Restoring now and then shows that they work, while it costs nothing to find out.")
  (wrong "To make the stick last longer"
    "Reading does not change how long a stick lasts. The point is to find a problem before you need the copy.")
  (hint "When would you rather find out that a backup cannot be read?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Computing That Lasts: Data")
```

So: the newest copy is in the newest folder that has it; copy it back
beside the file, or let `run backup restore` do it for you.
