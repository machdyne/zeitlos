# Files

```lesson
(objective "Write, read, list and delete files from Scheme.")
(minutes 15)
(keywords "path (of a file)" "RAM disk" "card (SD)")
```

A file is named by its **path**: the folders it is in, from the top,
then its own name, each after a `/`. `/ram/notes.txt` is the file
notes.txt in the folder `/ram`.

`/ram` is the **RAM disk**: a place for files in the computer's
memory. It is there with or without a card, and fast, but what is in
it is gone when the power goes off. With an **SD card**, the small
memory card some computers have, `/home` is yours to keep; `/usb` is a
USB stick.

```sources
(source "Zeitlos docs: repl.md, Writing a program in a file; ramdisk.md")
```

```check
(choice
  (ask "You saved a file in /ram, then switched the computer off and on. Where is the file?")
  (right "Gone: /ram is kept in memory, which forgets everything without power")
  (wrong "Still in /ram"
    "/ram is the RAM disk, kept in memory. Its files go when the power does.")
  (wrong "Moved to /home"
    "Nothing moves files by itself. To keep a file, save it in /home on the card.")
  (hint "What is /ram made of?"))
```

---

## Writing and reading

`write-file` makes a file with a string as its text, or replaces it.
`read-file` gives a file's text back as a string:

```
> (write-file "/ram/a.txt" "hi")
#t
> (read-file "/ram/a.txt")
"hi"
> (file-size "/ram/a.txt")
2
```

`write-file` answers `#t` when it worked and `#f` when it did not.
`read-file` gives `#f` for a file that is missing or empty.

```sources
(source "Zeitlos docs: zapi.md, Files")
```

```check
(choice
  (ask "What does `(read-file \"/ram/nothing-here.txt\")` give when there is no such file?")
  (right "#f")
  (wrong "\"\""
    "read-file does not make up an empty text: it answers #f for a missing file.")
  (wrong "A Scheme error"
    "A missing file is not a mistake in the program, only a no: read-file answers #f.")
  (hint "Look at what read-file gives for a file that is missing or empty."))
```

---

## Lists of files

`ls` gives the files in a folder as a list of strings, each a full
path. It is a list like any other. With only a.txt in `/ram`:

```
> (ls "/ram")
("/ram/a.txt")
> (length (ls "/ram"))
1
```

`(delete-file "/ram/a.txt")` deletes a file, and `(mkdir
"/ram/notes")` makes a folder; both answer `#t` or `#f`. As a bare
command, `ls /ram` does the same as `(ls "/ram")`.

```check
(choice
  (ask "Which expression gives how many files are in /ram?")
  (right "`(length (ls \"/ram\"))`")
  (wrong "`(ls \"/ram\")`"
    "That is the list of the files themselves. length counts them.")
  (wrong "`(file-size \"/ram\")`"
    "file-size is the size of one file. /ram is a folder; ls lists what is in it.")
  (hint "ls gives a list. Which procedure counts a list?"))
```

---

## Building a file's text

A file's text is a string, so the string procedures build it. `"\n"`
in a string is a new line:

```
(write-file "/ram/shop.txt"
  (string-append "tea\n" "cake\n"))
```

`(read-file "/ram/shop.txt")` then gives `"tea\ncake\n"`, written by
repl with the lines broken where each `\n` is.

```check
(number
  (ask "After `(write-file \"/ram/b.txt\" (string-append \"ab\" \"cd\"))`, what is `(file-size \"/ram/b.txt\")`?")
  (answer 4)
  (wrong-if 2 "The file holds the joined string, abcd: four characters.")
  (hint "The file's text is the joined string."))
```

---

## Try it

```try
(try (app "repl")
  (task "In repl, write a file with (write-file \"/ram/me.txt\" \"Hello\"), read it back with read-file, then type ls /ram. Open files from the dock and look in /ram too, then come back.")
  (check (number (ask "What does (file-size \"/ram/me.txt\") give?") (answer 5)
    (hint "Count the characters of Hello."))))
```

```check
(choice
  (ask "Which keeps a file after the computer is switched off?")
  (right "`(write-file \"/home/plan.txt\" text)`, on a computer with an SD card")
  (wrong "`(write-file \"/ram/plan.txt\" text)`"
    "/ram is the RAM disk, in memory: its files go when the power does.")
  (wrong "`(read-file \"/home/plan.txt\")`"
    "read-file only reads. write-file is what makes the file.")
  (hint "Which place is on the card, and which procedure writes?"))
```

So: files are named by paths, `write-file` and `read-file` move text
in and out of them, and `/ram` lasts only until the power goes.
