# The folders on the card

```lesson
(objective "Name the main folders at the top of the card, say what each is for, and write the path of a file.")
(minutes 20)
(keywords "path (of a file)")
```

You will learn what is in each folder at the top of the card, and which
one is yours.

When **files** starts, it shows the top of the card. Every card has the
same few folders there. Move the slider to look at each one.

```figure
(define k (slider 'folder 0 7 0 "Folder" 1))
(define names '("home" "apps" "data" "sys" "docs" "media" "opt" "tmp"))
(define about
  '(("Yours. Empty on a new" "card. Save dialogs" "start here.")
    ("The programs: one file" "for each app on the" "card.")
    ("Each app's own files," "a folder for each app:" "data/learn, data/web...")
    ("The system's settings" "(zeitlos.cfg, which" "Settings writes).")
    ("The documentation, as" "it came with the" "system.")
    ("Example sounds and" "pictures, and a place" "for your own.")
    ("Large optional packs:" "the Ark library, and" "the courses in learn.")
    ("Scratch space, emptied" "when the computer" "starts again.")))
(text 20 24 "/  (the top)" 'small 'bold)
(for-range 0 8
  (lambda (i)
    (text 40 (+ 48 (* i 22)) (list-ref names i) 'body)))
(frame 30 (+ 33 (* k 22)) 80 21 'ink 2)
(line 120 30 120 214 'grey 1)
(text 132 60 (string-append "/" (list-ref names k)) 'body 'bold)
(for-range 0 3
  (lambda (j)
    (text 132 (+ 90 (* j 18)) (list-ref (list-ref about k) j) 'small)))
```

```sources
(source "docs/layout.md, The tree and Each top-level folder")
```

---

## A path

A **path** names a file by the folders you go through to reach it,
from the top of the card, with a `/` between each name. The first `/`
is the top itself.

So `book.txt`, in a folder `novel`, in `home`, has the path
`/home/novel/book.txt`.

```example
(example
  (problem "A file letter.txt is in a folder letters, which is in home. What is its path?")
  (step "Start at the top of the card: /")
  (step "Go into home: /home")
  (step "Then into letters: /home/letters")
  (step "Then the file itself: /home/letters/letter.txt")
  (answer "/home/letters/letter.txt"))
```

```check
(text
  (ask "A file plan.txt is in a folder garden, which is in home. Type its path.")
  (accept "/home/garden/plan.txt")
  (wrong-if "home/garden/plan.txt" "Nearly. A path starts with / for the top of the card.")
  (wrong-if "/garden/plan.txt" "garden is not at the top of the card: it is inside home. Go through home first.")
  (hint "Start with /, then the folders from the outside in, then the file.")
  (hint "/home comes first."))
```

---

## /home is yours

The system puts nothing of its own in `/home`, and never makes folders
there: it is yours, to arrange as you like. When you save a new file,
the save dialog starts there.

Apps write their own files only in their own folder in `/data`, in
`/tmp`, and wherever you tell them to with a save dialog. They never
write in `/apps`, `/docs` or `/opt`.

```check
(choice
  (ask "You start writing a novel. Where should you keep it?")
  (right "In /home, in a folder of your own such as /home/novel")
  (wrong "In /apps, next to text"
    "/apps holds the programs, one file each. Your work belongs in /home, which is yours.")
  (wrong "In /tmp, where it is quick to reach"
    "/tmp is emptied when the computer starts again. Your novel would be gone.")
  (wrong "In /data/text"
    "/data/text is the text app's own folder, for its own files. Your writing is yours: /home.")
  (hint "Which folder does the system leave entirely to you?"))
```

```sources
(source "docs/layout.md, The rules")
```

---

## /data: each app's own

Each app that keeps files of its own has one folder in `/data`, named
after it, for all of them: settings it keeps, games it has saved, files it came with.
learn keeps each learner's progress in `/data/learn`, and the casino
keeps its bank in `/data/casino`.

So an app is in two places at most: its program (in `/apps`, or in
flash for the core of the system), and its folder in `/data`.

```check
(choice
  (ask "Where does learn keep what you have done in its lessons?")
  (right "In /data/learn")
  (wrong "In /home"
    "/home is yours: apps do not put their own files there. learn keeps its records in its own folder in /data.")
  (wrong "In /apps/learn"
    "/apps/learn is the program itself, one file. Its records go in its own folder in /data.")
  (wrong "Nowhere: it is lost at power off"
    "learn writes your progress to the card as you go, so it is kept.")
  (hint "Every app has a folder of its own, named after it, in one place."))
```

```sources
(source "docs/layout.md, /data/<name>/")
(source "docs/learn.md, Learners")
```

---

## Practice

```check
(choice
  (ask "You set the time zone in Settings. Which folder holds the file it writes?")
  (right "/sys")
  (wrong "/home" "Settings belong to the system, not to your own files. They are kept in /sys/zeitlos.cfg.")
  (wrong "/docs" "/docs is the documentation, for reading. Settings are in /sys.")
  (hint "The system's own files have a short folder of their own."))
```

---

```check
(text
  (ask "Which folder at the top of the card holds the documentation? Type its path.")
  (accept "/docs") (accept "/docs/")
  (wrong-if "docs" "Nearly: a path starts with / for the top of the card.")
  (hint "It is a short name for documents, at the top of the card."))
```

---

```check
(choice
  (ask "Which of these folders is emptied when the computer starts again?")
  (right "/tmp")
  (wrong "/home" "/home is yours and is kept. Only the scratch folder is emptied.")
  (wrong "/data" "/data holds each app's own files, kept from one day to the next.")
  (hint "Its name is short for temporary."))
```

```sources
(source "docs/layout.md, /sys and /tmp, /ram, /usb")
```

So: your files go in `/home`, each app's own files in `/data`, and a
path names a file from the top of the card down.
