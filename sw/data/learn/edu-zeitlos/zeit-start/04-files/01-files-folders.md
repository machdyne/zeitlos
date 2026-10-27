# Files and folders

```lesson
(objective "Say what files and folders are, read a path, and know which folder is yours.")
(minutes 15)
(keywords "file (on a computer)" "folder (of files)" "path (of a file)" "home folder")
```

This lesson is about where your work is kept.

Work you keep -- a letter, a table, a picture, a song, a program -- is
kept as a **file**: a piece of work with a name of its own, such as
`notes.txt`. Files are kept on the memory card in the computer, the SD
card, so they are still there when the computer is turned off and on
again.

An open app holds your work only while it runs. To keep the work, the
app saves it into a file.

```sources
(source "The card layout (docs/layout.md), The tree")
```

```check
(choice
  (ask "You write a letter in text and turn the computer off without saving it. Is the letter kept?")
  (right "No: only work saved into a file is kept")
  (wrong "Yes: everything on the screen is kept" "What is on the screen is held by the app only while it runs. Work is kept when it is saved into a file.")
  (wrong "Yes, in the dock" "The dock holds icons for opening apps, not your work. Work is kept in files.")
  (hint "Where is work kept when the computer is off?"))
```

---

## Folders

There are a great many files on the card, so they are sorted into
**folders**. A folder holds files, and other folders too, like a drawer
that can hold smaller boxes.

The folder at the very top of the card is written `/`. Some of the
folders in it:

| Folder | Holds |
|---|---|
| `home` | your own files |
| `apps` | the apps |
| `docs` | the Zeitlos documentation |
| `media` | pictures and music to try, and yours |
| `sys` | the system's own files, such as its settings |
| `tmp` | work in progress, wiped at the next start |

[The card layout](docs:layout) lists every folder.

```sources
(source "The card layout (docs/layout.md), The tree; Each top-level folder")
```

```check
(choice
  (ask "Which folder is the place for your own files?")
  (right "home")
  (wrong "apps" "apps holds the programs themselves. Your own files go in home.")
  (wrong "tmp" "Everything in tmp is wiped when the computer starts again. Your files belong in home.")
  (hint "The folder whose name is the place you live."))
```

---

## Paths

A file's **path** says where it is: the folders from the top, each
followed by `/`, then the file's name.

`/home/notes.txt` is the file `notes.txt` in the folder `home`, which
is in the top folder `/`. `/home/school/map.png` is `map.png` in the
folder `school`, which is in `home`.

```example
(example
  (problem "Write the path of the file song.mod in the folder audio, which is in the folder media.")
  (step "Start at the top folder: /")
  (step "Then the folders, each followed by /: media/audio/")
  (step (text (ask "Now add the file's name. What is the whole path?")
          (accept "/media/audio/song.mod")
          (hint "/media/audio/ and then the name.")))
  (answer "/media/audio/song.mod"))
```

```check
(text
  (ask "The file plan.txt is in the folder home. What is its path?")
  (accept "/home/plan.txt")
  (wrong-if "home/plan.txt" "Nearly: a path starts at the top folder, so it begins with /.")
  (wrong-if "/plan.txt" "That would be plan.txt in the top folder itself. It is in home, so home comes first: /home/plan.txt.")
  (hint "The top folder /, then home and /, then the name."))
```

---

## Your home folder

`/home` is your **home folder**. Zeitlos never puts anything there
itself; you arrange it as you like, with folders of your own. When an
app saves a file, it offers your home folder first.

`/tmp` is different: anything there is gone when the computer starts
again. It is for work in progress that does not need keeping.

```sources
(source "The card layout (docs/layout.md), /home; /tmp, /ram, /usb")
```

```check
(choice
  (ask "You save a drawing you want to keep in /tmp. What happens to it?")
  (right "It is gone the next time the computer starts")
  (wrong "It stays there for ever" "/tmp is wiped at every start. Save work you want to keep in your home folder.")
  (wrong "It moves to /home by itself" "Nothing moves your files for you. Anything left in /tmp is gone at the next start.")
  (hint "What happens to /tmp each time the computer starts?"))
```

---

```check
(choice
  (ask "What is the difference between a file and a folder?")
  (right "A file holds a piece of work; a folder holds files and other folders")
  (wrong "There is none" "A file is one piece of work, such as a letter; a folder holds files, to keep them in order.")
  (wrong "A folder is a big file" "A folder is not work itself: it holds files, and other folders.")
  (hint "Think of a letter and the drawer it is kept in."))
```

So your work is kept in files, sorted into folders; a path such as
`/home/notes.txt` says where a file is, and `/home` is yours.
