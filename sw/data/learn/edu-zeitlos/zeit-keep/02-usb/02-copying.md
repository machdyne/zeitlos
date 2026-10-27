# Copying to and from a stick

```lesson
(objective "Copy files and folders between the card and a USB stick, and open files from a stick that came from another computer.")
(minutes 15)
```

You will learn to take your work with you on a stick, and to bring work from a
stick onto the card.

Copying to a stick is a copy like any other in files. Select the file
or folder, press **Copy**, and change the start of the line from where
it is to where it should go.

```example
(example
  (problem "Copy your folder /home/novel onto the stick.")
  (step "In files, select novel in /home and press Copy.")
  (step "The line says /home/novel. Change it to /usb.")
  (step "/usb is an existing folder, the top of the stick, so the copy goes into it.")
  (answer "The stick now has /usb/novel, with everything that was in the folder."))
```

```check
(text
  (ask "You copy /home/letter.txt and type /usb at Copy to:. What is the path of the copy?")
  (accept "/usb/letter.txt")
  (wrong-if "/usb" "/usb is the top of the stick, an existing folder, so the copy goes into it and keeps its name.")
  (hint "A folder at Copy to: means into that folder, under the same name."))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

## A big copy

A stick is slower than the card. While a folder is copied, the line
above the list counts the files and the amount copied, and you can go
on using the window. **Esc** or **Stop** ends the copy: what was
finished stays, and the file being written is removed, so the stick
never holds half a file.

```check
(choice
  (ask "You stop a copy of a folder of photos to a stick part way. What is on the stick?")
  (right "The photos that were finished, each one whole")
  (wrong "Nothing: a stopped copy is undone"
    "A stopped copy keeps what it had finished. Only the photo being written is removed.")
  (wrong "Every photo, some of them cut short"
    "files removes the one it was writing when you stop, so you never get half a file.")
  (hint "files tells you it keeps what it had finished."))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
(source "docs/backup.md, Drives (a stick through FatFs: about 180 KB/s)")
(source "docs/update.md (the card through FatFs: about 600 KB/s)")
```

---

## Copy, or move?

**Move** to a stick is a copy followed by a delete: afterwards the
file is on the stick only. To take work somewhere, **copy** it, so
that the card keeps its own copy too.

```check
(choice
  (ask "You want to take your novel to a friend's computer. Which is safer?")
  (right "Copy it to the stick, so the card keeps it as well")
  (wrong "Move it to the stick, so there is only one version"
    "After a move, the stick holds the only copy. If the stick is lost or fails, so is the novel.")
  (wrong "Either: they are the same"
    "A move deletes the original once the copy is done. A copy leaves it on the card.")
  (hint "After each one, how many copies of the novel are there?"))
```

---

## Files from another computer

A stick written by another computer is read the same way. Long names
with spaces and accents are kept as they are.

Double-click a file on the stick to open it. A document or table from
another system (`.docx`, `.odt`, `.xlsx` ...) is converted into `/tmp`
and the copy opened, so the file on the stick is never changed. The
first time you save the copy, choose a folder in `/home` to keep it.

```check
(choice
  (ask "You double-click accounts.xlsx on a stick, and it opens in sheet. Where is the copy you are looking at?")
  (right "In /tmp, converted from the file on the stick")
  (wrong "On the stick: sheet changes accounts.xlsx itself"
    "The original is never touched. files converts it into /tmp and opens that copy.")
  (wrong "In /home, as accounts.xlsx"
    "Nothing goes to /home until you save it there. The converted copy starts in /tmp.")
  (hint "Opening another system's table converts it first. Where does a converted copy go?"))
```

```sources
(source "docs/file_browser.md, Opening a file")
(source "docs/sdcard.md, Long file names")
```

---

## Copying again

A file copied onto one already there asks first, then replaces it. A
**folder is never copied over a folder**: if `/usb/novel` is already
on the stick, copying `/home/novel` to `/usb` again is refused, and
files says the destination exists.

So copy it under a new name, such as `/usb/novel-2`, or delete the old
folder on the stick first.

```check
(choice
  (ask "/usb/novel is already on the stick. You select /home/novel, press Copy and type /usb. What happens?")
  (right "files refuses: the destination exists")
  (wrong "The folder on the stick is replaced by the new one"
    "A folder is never copied over a folder. Give the copy a new name, or delete the old one first.")
  (wrong "The two folders are merged"
    "files does not merge folders. It refuses, and says the destination exists.")
  (hint "Copy to /usb means into /usb, as /usb/novel. Is that free?"))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
```

---

```check
(choice
  (ask "You copied /home/novel to /usb an hour ago and have written another chapter since. What is on the stick?")
  (right "The novel as it was an hour ago")
  (wrong "The novel with the new chapter"
    "A copy is the folder as it was when you copied it. The stick changes only when you copy again.")
  (wrong "Nothing: the copy is undone when the card changes"
    "The copy stays as it was. It just does not have the new chapter.")
  (hint "Does a copy follow the original after it is made?"))
```

So: copy, not move, to take work away on a stick; give a second copy of a
folder a new name; and save what you open from a stick into `/home`.
