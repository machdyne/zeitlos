# Keeping files in order

```lesson
(objective "Make a folder, and rename, copy, move, delete and find files with the files app.")
(minutes 20)
```

This lesson is about the buttons along the bottom of files.

| Button | Key | Does |
|---|---|---|
| Open | Enter | opens the chosen row, as a double-click does |
| New Dir | | makes a new folder here, asking for its name |
| Rename | F2 | gives the chosen file or folder a new name |
| Copy | | makes a copy of it somewhere else |
| Move | | moves it somewhere else |
| Delete | Delete | deletes it, after asking |
| Find | F3 | looks for files by name |

**Tab** goes from the list to the buttons and along them; Enter or
Space presses the button with the ring around it.

```sources
(source "files (docs/file_browser.md), Using it")
(source "sw/apps/files/files.c: the buttons Open, New Dir, Rename, Copy, Move, Delete, Convert, Find")
```

```check
(text
  (ask "Which function key renames the chosen file in files? (Type its name.)")
  (accept "F2")
  (wrong-if "F1" "F1 is help, as in every app. Renaming is F2.")
  (hint "The second function key."))
```

---

## Copy and move

Copy and Move ask where to, starting from the file's own path. Change
the end of the line and press Enter. If you type the path of a folder
that is already there, the file goes into it: `/home/school` puts it
in your folder school.

A copy leaves the first file where it was; a move does not. A folder is
copied, moved or deleted with everything in it.

```sources
(source "files (docs/file_browser.md), Rename, copy and move")
```

```check
(choice
  (ask "You Move /home/map.png to /home/school, a folder that is there. Where is the file now?")
  (right "Only in /home/school")
  (wrong "In both /home and /home/school" "That is what Copy does. Move takes it out of /home.")
  (wrong "It is now named school" "The path of a folder that is already there means into it, so the file keeps its name and goes inside.")
  (hint "Move, not Copy."))
```

---

## Delete

Delete asks first, naming what it is about to delete. Answer with care:
**there is no wastebasket**, and a deleted file cannot be brought back.
A folder deleted goes with everything in it.

```warn
(warn "Deleting is for good" "A deleted file cannot be brought back. Read the name in the question before you answer yes.")
```

```sources
(source "files (docs/file_browser.md), Rename, copy and move: deleting uses fs_unlink() at once")
```

```check
(choice
  (ask "You deleted a file by mistake. Can you get it back from a wastebasket?")
  (right "No: on Zeitlos a deleted file is gone")
  (wrong "Yes, from the wastebasket" "Zeitlos has no wastebasket. That is why Delete asks first.")
  (wrong "Yes, by pressing F5" "F5 reads the folder again; it shows that the file is gone.")
  (hint "Why does Delete ask first?"))
```

---

```try
(try (app "files")
  (task "Press Super+2 and open files from the dock. Go into the folder home. Press New Dir and make a folder called practice. Choose it, press F2 and rename it to tries. Then choose it, press Delete and answer yes. Press Super+1 to come back here.")
  (check (choice (ask "Before deleting it, what did files ask?")
    (right "Whether to delete it, naming the folder")
    (wrong "Nothing: it was deleted at once" "Delete always asks first, with the name of what it would delete.")
    (wrong "For a password" "Delete asks only whether you are sure, naming the folder.")
    (hint "Delete asks before it deletes."))))
```

---

## Find

**F3** (or the Find button) asks what to look for, in the folder you
are in and every folder inside it. Type part of a name -- `map` finds
`map.png` and `old-maps.txt`. Put words in quotes, `"holiday"`, to find
files with those words inside them.

The results replace the list. Enter opens one; **Esc** goes back to the
folder (press it twice if files is still looking).

```sources
(source "files (docs/file_browser.md), Find")
```

```check
(choice
  (ask "You are in /home and want every file whose name has the word plan in it. What do you do?")
  (right "Press F3 and type plan")
  (wrong "Press F2 and type plan" "F2 renames the chosen file. F3 is find.")
  (wrong "Open every folder and look" "That works, but F3 looks through every folder inside /home for you.")
  (hint "Find is the third function key."))
```

So files makes folders, renames with F2, copies and moves, deletes for
good after asking, and finds with F3.
