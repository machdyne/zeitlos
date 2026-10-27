# Copying, moving and deleting

```lesson
(objective "Copy, move and delete files in files without losing anything by mistake, and know which places are emptied at power off.")
(minutes 20)
(keywords "drive (storage)" "RAM disk (a drive in memory)" "USB (a kind of socket)")
```

You will learn what **files** does when you copy, move or delete, so
that nothing is lost by mistake.

First, the places files can be. Each is a **drive**: a store of files
with folders of its own. The card is one. At the top of the card,
files also shows two others when they are there:

| Folder | The drive |
|---|---|
| `/ram` | the **RAM disk**: a drive made of memory, very fast, and emptied when the power goes off |
| `/usb` | a stick plugged into a **USB** socket: the standard socket for plugging things into a computer (the next unit) |

`/ram` and `/usb` look like folders, but they are separate drives, not
folders on the card.

```sources
(source "docs/layout.md, /tmp, /ram, /usb")
(source "docs/ramdisk.md")
```

---

## /tmp

`/tmp` is scratch space. On most boards it is the RAM disk under
another name, and on the others the system empties it when the
computer starts. Either way, nothing in `/tmp` is there after the next
start.

When you open a document from another system, such as a `.docx` file,
files converts it into `/tmp` and opens the copy, so the original is
never touched. The first time you save the copy, you are asked where to
keep it: choose a folder in `/home`, or the copy is lost at the next
start.

```check
(choice
  (ask "You open report.docx and edit the copy in text. When text asks where to save it, you choose /tmp. What happens to your edits when the computer is switched off and on again?")
  (right "They are gone: /tmp is emptied")
  (wrong "They are kept in report.docx"
    "The original is never touched: you were editing a converted copy in /tmp, and /tmp is emptied.")
  (wrong "They are kept, because they were saved"
    "Saving to /tmp is saving to scratch space. It does not outlast the next start. Save it in /home.")
  (hint "Which folder is emptied when the computer starts again?"))
```

```sources
(source "docs/file_browser.md, Opening a file")
(source "docs/layout.md, /tmp, /ram, /usb")
```

---

## Delete

**Delete** (or the Delete key) asks first, naming the file, and then
removes it at once. A folder goes with everything in it.

There is no wastebasket to take it back from: once you say yes, it is
gone from the card. Read the name in the question before you answer.

```check
(choice
  (ask "You deleted the wrong file in files a minute ago. Where can you get it back from?")
  (right "Only from a copy made before, somewhere else")
  (wrong "From the wastebasket"
    "files has no wastebasket: Delete removes the file from the card at once.")
  (wrong "Press Esc to undo it"
    "Esc stops a delete that is still running, but a file that is already deleted is gone.")
  (hint "files asks before it deletes, and then does not keep the file anywhere."))
```

```sources
(source "docs/file_browser.md, Using it")
```

---

## Copy

**Copy** asks "Copy to:" and starts from the file's own path, so that
the usual change is a small edit at the end: a new name, or another
folder.

- A path that is an **existing folder** means "into it".
- Copying onto a file that is already there **asks first** before it
  replaces it.
- A folder is copied with everything in it.

```example
(example
  (problem "Copy /home/book.txt into the folder /home/old, which already exists.")
  (step "Select book.txt in /home and press Copy.")
  (step "The line says /home/book.txt. Change it to /home/old.")
  (step "/home/old is a folder, so the copy goes into it.")
  (answer "The copy is /home/old/book.txt."))
```

```check
(text
  (ask "You copy /home/notes.txt and type /home/2026 at Copy to:, where /home/2026 is a folder. What is the path of the copy?")
  (accept "/home/2026/notes.txt")
  (wrong-if "/home/2026" "/home/2026 is a folder, so the copy goes into it, keeping its name.")
  (hint "A folder at Copy to: means into that folder."))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

## Move

**Move** asks "Move to:" in the same way.

- Within one drive, a move only changes where the file is listed. It is
  instant, whatever the size.
- **Between drives**, from the card to a stick say, a move is a copy
  followed by a delete, and the original is deleted only once the copy
  has succeeded.
- Moving onto a file that is already there is refused: files says the
  destination exists.

```check
(choice
  (ask "You move a large folder from the card to /usb. Part way through, the copy fails. What has happened to the folder on the card?")
  (right "It is still there, untouched")
  (wrong "It is half deleted"
    "The original is deleted only once the copy has succeeded. A copy that fails leaves it as it was.")
  (wrong "It is gone, and the copy is half done"
    "files never deletes before the copy is complete, so nothing can be lost that way.")
  (hint "Between drives, a move is two steps. Which comes first?"))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

## Stopping

A long copy, move or delete runs while the window stays usable, and
**Esc** (or **Stop**) ends it. A copy that is stopped keeps what it had
finished, and deletes the file it was writing: you never get half a
file.

```try
(try (app "files")
  (task "In /home, make a folder called old with New Dir. Then select any file in /home, press Copy, and look at the line before you change it. Press Esc to cancel, then come back.")
  (check (choice
    (ask "What did the Copy to: line start with?")
    (right "The file's own path")
    (wrong "An empty line"
      "Look again: files fills in the file's own path, so that you only change the end.")
    (wrong "/usb"
      "files does not guess where you want the copy. It starts from where the file is now."))))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

```check
(choice
  (ask "Which of these is still there after the computer has been switched off and on?")
  (right "A file in /home")
  (wrong "A file in /ram"
    "/ram is the RAM disk, made of memory, and memory is emptied at power off.")
  (wrong "A file in /tmp"
    "/tmp is emptied when the computer starts. Keep what matters in /home.")
  (hint "Which of these folders is neither memory nor scratch space?"))
```

So: delete is for ever, a move between drives copies before it deletes,
and only what is on the card, outside `/tmp`, survives a restart.
