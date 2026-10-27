# Finding words in files

```lesson
(objective "Find the files that hold some words, and then find the words in them.")
(minutes 15)
```

To find files by what is **in** them, put the words in double quotes:
`"invoice"` finds every file with invoice in it, in capitals or small
letters, in the folder shown and below.

A name and words together narrow it: `*.txt "invoice"` finds only the
`.txt` files with invoice in them.

```check
(text
  (ask "What do you type into Find for the .md files that have the word tuesday in them?")
  (accept "*.md \"tuesday\"")
  (accept "*.md  \"tuesday\"")
  (accept "\"tuesday\" *.md")
  (wrong-if "*.md tuesday" "Without the quotes, tuesday is taken as part of the name. Put the words to look for inside double quotes.")
  (wrong-if "\"tuesday\"" "That finds every kind of file with tuesday in it. Put `*.md` in front to keep to Markdown files.")
  (hint "The name pattern first, then the words in double quotes."))
```

```sources
(source "Zeitlos docs: files (docs/file_browser.md), Find; sw/apps/files/files.c, do_find()")
```

---

## It takes longer

A name find only reads the folders. A find for words reads every file,
so it goes at the speed of the storage: the more files, the longer.
`files` keeps answering meanwhile, the line above counts the files
looked at, and **Esc** stops it.

To make it quick, start in the smallest folder that will do, such as
`/home` rather than `/`, and give a name pattern too.

```check
(choice
  (ask "Which find is likely to be quickest?")
  (right "In /home: `*.txt \"invoice\"`")
  (wrong "In /: \"invoice\""
    "That reads every file on the computer. A smaller folder and a name pattern mean fewer files to read.")
  (wrong "In /: `*.txt \"invoice\"`"
    "The pattern helps, but / still holds every folder. Start in /home.")
  (hint "Fewer files to read is quicker."))
```

---

## Capitals and accents

As with names, the letters A to Z match in either case. Any other
character, such as an accented letter, must be just as you typed it.

```check
(choice
  (ask "You search for \"zurich\". Which file is found?")
  (right "One with Zurich in it")
  (wrong "One with Zürich in it"
    "ü is not u: accented letters must match exactly. Search for \"zürich\" to find that one.")
  (hint "Case does not matter for A to Z. Accents do."))
```

---

## Then find the place

A result tells you which file holds the words, not where in it. Open
the file in `text` and press **Ctrl+F** to find the place. `read` has
no find, so open a `.md` file in `text` with Ctrl+O for this.

```example
(example
  (problem "Find where you wrote about the dentist.")
  (step "In files, go to /home and Find: \"dentist\".")
  (step "The result is notes/june.md. Enter would open it in read, which has no find.")
  (step "So open text from the dock, and open /home/notes/june.md with Ctrl+O.")
  (step (choice (ask "Which key now finds dentist in the document?")
    (right "Ctrl+F")
    (wrong "Ctrl+L" "Ctrl+L goes to a line by its number. Ctrl+F finds words.")))
  (answer "Ctrl+F, then F3 for each further match."))
```

---

```try
(try (app "files")
  (task "In files, go to /home. Find \"plan\" (with the quotes). Look at the results, press Esc twice, and come back here.")
  (check (choice
    (ask "plan.md has the word plan in its first line. Was it found?")
    (right "Yes")
    (wrong "No"
      "Check the quotes: \"plan\" looks inside files. Check too that files was showing /home, where plan.md is."))))
```

So: words in double quotes find what is inside files; then Ctrl+F in
`text` finds the place.
