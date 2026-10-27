# The files app

```lesson
(objective "Use the files app to go into folders, come back up, and see what is in them.")
(minutes 20)
```

This lesson is about files, the app for finding your way among folders.

files is the second icon on the dock, after term. Its window has three
parts:

- at the top, the path of the folder you are looking at;
- in the middle, a list of what is in it: a row for each folder and
  file, with a small picture of a folder or a page, and its name;
- at the bottom, a row of buttons.

files starts in the top folder, `/`. A folder's row says `<dir>` on the
right (short for directory, another word for folder); a file's row
shows its size and the date it was last saved.

```figure
(zs-win 20 10 280 190 "files" #t #t)
(text 28 38 "/" 'small)
(frame 26 44 268 112 'ink 1)
(text 34 60 "apps" 'small) (text 288 60 "<dir>" 'small 'right)
(text 34 76 "data" 'small) (text 288 76 "<dir>" 'small 'right)
(text 34 92 "docs" 'small) (text 288 92 "<dir>" 'small 'right)
(text 34 108 "home" 'small) (text 288 108 "<dir>" 'small 'right)
(text 34 124 "..." 'small)
(text 28 180 "Open  New Dir  Rename  Copy ..." 'small)
(caption "files: the path, the list, the buttons.")
```

```sources
(source "files (docs/file_browser.md), Using it")
(source "The card layout (docs/layout.md), /home: the file browser starts at the top of the card")
```

```check
(choice
  (ask "In files, a row says <dir> on its right. What is it?")
  (right "A folder")
  (wrong "A file that is damaged" "<dir> is short for directory, another word for folder. Nothing is wrong with it.")
  (wrong "An app" "Apps are files too, and their rows show a size and a date. <dir> marks a folder.")
  (hint "dir is short for directory."))
```

---

## Going in and coming back

- **Click** a row to choose it: it is shown lit.
- **Double-click** a folder, or press **Enter**, to go into it. The path
  at the top grows: `/home`.
- To go back up, double-click the `..` row at the top of the list, or
  press **Backspace**.
- **Up** and **Down** move from row to row; the scroll wheel moves the
  list without changing the row you chose.
- **F12** takes you back to the folder files started in.

```sources
(source "files (docs/file_browser.md), Using it")
(source "Function keys (docs/keys.md): F12, the app's home")
```

```check
(choice
  (ask "You are in /home/school and want to see /home. What do you do?")
  (right "Press Backspace, or double-click the .. row")
  (wrong "Double-click the folder school" "You are already in school. Going up to home is Backspace, or the .. row.")
  (wrong "Press Enter" "Enter goes into the row you chose. Backspace goes up one folder.")
  (hint "The key that goes back one."))
```

---

```try
(try (app "files")
  (task "Press Super+2 and click the second icon on the dock to open files. Double-click the folder docs to go into it, and look at the path at the top. Press Backspace to come back up. Then press Super+1 to come back here.")
  (check (text (ask "What did the path at the top say while you were in the folder docs?")
    (accept "/docs") (accept "/docs/")
    (wrong-if "docs" "Nearly: the path starts at the top folder, /.")
    (hint "The top folder, then docs."))))
```

---

```check
(choice
  (ask "You have gone down several folders and want to start again where files began. Which key takes you there?")
  (right "F12")
  (wrong "Esc" "Esc stops something that is running, such as a search. F12 goes back to where files started.")
  (wrong "F1" "F1 shows help. F12 is the app's home: for files, the folder it started in.")
  (hint "The last function key."))
```

```check
(choice
  (ask "You turn the scroll wheel over the list in files. What changes?")
  (right "The part of the list you see; the chosen row stays chosen")
  (wrong "The chosen row moves down" "The wheel moves only the view. Up and Down move the chosen row.")
  (wrong "files goes into the chosen folder" "Turning the wheel never opens anything. Double-click or Enter does.")
  (hint "The wheel moves the view, not what you chose."))
```

So files shows the path, a list and buttons: double-click or Enter to go
into a folder, Backspace or `..` to come back, and F12 to go home.
