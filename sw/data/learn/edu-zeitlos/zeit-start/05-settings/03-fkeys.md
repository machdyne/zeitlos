# The function keys

```lesson
(objective "Say what each function key means, in any app.")
(minutes 15)
```

This lesson goes through the function keys, F1 to F12. Each means the
same thing in every app, so once you know them, you know them
everywhere. An app with no use for a key leaves it doing nothing,
rather than giving it another job.

| Key | Means | For example |
|---|---|---|
| F1 | help | the help box |
| F2 | rename, or change in place | files: rename; sheet: change the cell |
| F3 | find, then find the next | files, text, sheet, web |
| F4 | the line where you type the app's commands | sheet, slide, web's address |
| F5 to F8 | the app's own | learn: F5 a hint, F6 read aloud, F7 a mistake |
| F9 | kept for later | nothing yet |
| F10 | colour on or off | games that can show colour |
| F11 | full screen | any window that can change size |
| F12 | the app's home | files: the folder it started in |

[Function keys](docs:keys) has the whole list.

```sources
(source "Function keys (docs/keys.md)")
```

```check
(choice
  (ask "You want to look for a word in text. Which key do you try first?")
  (right "F3")
  (wrong "F2" "F2 is rename or change in place. Finding is F3, in every app that finds.")
  (wrong "F4" "F4 goes to the line where you type the app's commands. Finding is F3.")
  (hint "Find is the third key."))
```

---

## The app's own

F5 to F8 are each app's own, and its F1 box lists them. In files, F5
reads the folder again; in web, F5 loads the page again; in learn, as
you know, F5 is a hint.

```check
(choice
  (ask "Where do you find what F5 to F8 do in an app you have not used?")
  (right "In its help box: press F1")
  (wrong "They do the same in every app" "F1 to F4 and F9 to F12 do; F5 to F8 are each app's own, listed in its help box.")
  (wrong "Nowhere: try them and see" "You can, but the help box lists them. Press F1.")
  (hint "Which key always tells you an app's keys?"))
```

---

```check
(text
  (ask "Which function key takes an app back to where it started -- files to its first folder? (Type its name.)")
  (accept "F12")
  (wrong-if "F1" "F1 is help. The app's home is the last function key.")
  (hint "The last function key."))
```

---

```check
(text
  (ask "Which function key renames the chosen file in files? (Type its name.)")
  (accept "F2")
  (hint "Rename is the second function key."))
```

---

```check
(choice
  (ask "A window is in full screen. Which key brings back its titlebar?")
  (right "F11, or Esc")
  (wrong "F12" "F12 is the app's home. Full screen goes on and off with F11; Esc also comes out.")
  (wrong "F10" "F10 turns colour on or off where an app has colour. Full screen is F11.")
  (hint "The same key that made it full screen."))
```

---

## The end of the course

You can now find your way around the desktop and its windows and
workspaces, open apps from the dock, keep your work in files and
folders, change the settings, and get help with F1. The other
courses about Zeitlos -- writing, records and numbers, making things,
programming -- start from here.

```check
(choice
  (ask "You open an app you have never used. What is the first thing to press to find out how it works?")
  (right "F1")
  (wrong "F12" "F12 is the app's home. F1 opens its help box.")
  (wrong "Ctrl+Alt+Del" "That asks to restart the computer. F1 shows the app's help.")
  (hint "Help is the first function key."))
```

So the function keys mean the same everywhere: F1 help, F2 rename, F3
find, F4 the line for commands, F5 to F8 the app's own, F11 full screen and
F12 home.
