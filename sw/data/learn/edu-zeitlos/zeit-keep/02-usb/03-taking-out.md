# Taking a stick out

```lesson
(objective "Take a USB stick out safely: let writing finish, then unmount it at the console.")
(minutes 15)
(keywords "console (in term)" "command (typed)" "unmount (a drive)" "mount (a drive)")
```

You will learn why a stick should not be pulled out while it is being
written, and how to tell Zeitlos you are about to take it out.

Changing a file on a drive means writing several things: the data
itself, the list of blocks that hold it, and the folder's entry for
it. If the stick is pulled out, or the power goes, after some of those
writes but not the others, the filesystem can be left inconsistent: a
file pointing at the wrong data, or space marked as used that belongs to
no file. FAT keeps no log of changes to repair this from. What was still
waiting to be written is lost as well.

```check
(choice
  (ask "You pull a stick out half way through saving a file to it. What can happen?")
  (right "The filesystem on the stick can be left inconsistent, and the file damaged")
  (wrong "Nothing: the save simply carries on when you plug it back in"
    "Nothing is kept waiting for the stick. What had not been written yet is lost.")
  (wrong "Only the card is affected, never the stick"
    "It is the stick that was being written, so it is the stick that can be left half changed.")
  (hint "A save is several writes. What if only some of them happen?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Surviving Power Loss")
(source "docs/usb_host.md, Removal while mounted")
```

---

## First, let it finish

Before you take a stick out, make sure nothing is writing to it:

- a copy or move in files has finished (the count above the list has
  gone);
- any file you opened from the stick is saved and its window closed.

The same holds for the power: let the computer finish writing before
you switch it off.

```check
(choice
  (ask "files is copying a folder to the stick and counting the files above the list. Is it safe to pull the stick out now?")
  (right "No: wait for the copy to finish, or stop it with Esc first")
  (wrong "Yes: files keeps a copy of what it was writing"
    "files writes straight to the stick. Pulling it out now can leave the stick half written.")
  (wrong "Yes, as long as the light on the stick is off"
    "The count above the list shows the copy is still running. Wait for it, or stop it.")
  (hint "What does the count above the list tell you?"))
```

```sources
(source "docs/file_browser.md, Rename, copy and move")
(source "Timeless Computing (docs/tc.md), Memory and Storage: Surviving Power Loss")
```

---

## The console

Then you tell the system to let go of the stick. One way is the
**console**: open **term** from the dock and press **CONSOLE**. It
shows the messages the system has printed lately, and a prompt, `>`,
where you can type a **command**: a line that tells the system to do
something, sent with Enter.

To **mount** a drive is to make it ready to use, as Zeitlos does by
itself when a stick is plugged in. To **unmount** it is to have the
system finish with it and let it go. The command for the stick is:

```
> usbunmount
```

It answers `/usb unmounted, safe to remove`. Then pull the stick out.

```sources
(source "docs/console.md")
(source "docs/filesystem.md, Volumes")
```

```check
(text
  (ask "Type the command, at the console, that lets go of the stick so it can be pulled out.")
  (accept "usbunmount")
  (wrong-if "usbmount" "That one mounts the stick again. The one that lets go of it has un in it.")
  (wrong-if "usb unmount" "Nearly: it is one word, with no space.")
  (hint "It is usb and unmount, as one word."))
```

---

## When it says no

If a program still has a file on the stick open, `usbunmount` answers
`files are open` instead, and the stick stays mounted. Save and close
that file, then type the command again.

After `usbunmount`, `/usb` stays away until you pull the stick out, so
it is not taken back by mistake. To use it again without pulling it
out, type `usbmount`.

```check
(choice
  (ask "You type usbunmount and the console answers that files are open. What do you do?")
  (right "Save and close the file you opened from the stick, then type usbunmount again")
  (wrong "Pull the stick out anyway"
    "A file on the stick is still open, and could still be written. Close it first.")
  (wrong "Type usbmount"
    "usbmount mounts the stick. It is still mounted: what is in the way is a file that is open.")
  (hint "Which program could still have a file on the stick?"))
```

```sources
(source "docs/filesystem.md, Volumes")
(source "sw/os/kfs.c, fs_usb_unmount")
```

---

```example
(example
  (problem "You edited /usb/letter.txt in text, and want to take the stick home.")
  (step "Save the letter and close text's window.")
  (step "Open term from the dock and press CONSOLE.")
  (step "Type usbunmount and press Enter.")
  (step (choice (ask "The console answers /usb unmounted, safe to remove. What now?")
          (right "Pull the stick out")
          (wrong "Wait for files to show usb again"
            "After usbunmount, /usb stays away until the stick is pulled out. It is safe to pull it now.")))
  (answer "The stick comes out with the letter whole."))
```

```check
(choice
  (ask "Why unmount before pulling a stick out, when nothing seems to be happening?")
  (right "So that the system has finished every write to it and lets it go")
  (wrong "To delete the files on it"
    "Unmounting deletes nothing. It only makes sure the system has finished with the stick.")
  (wrong "Because the stick only works on one computer otherwise"
    "A stick works on any computer that reads FAT. Unmounting is about finishing every write first.")
  (hint "What does to unmount a drive mean?"))
```

So: let every write finish, type `usbunmount` at the console, and pull
the stick out when it says it is safe.
