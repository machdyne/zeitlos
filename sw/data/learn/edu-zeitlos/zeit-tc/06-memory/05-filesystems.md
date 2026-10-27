# Filesystems and power cuts

```lesson
(objective "Say what a filesystem records, and how a careful one survives a power cut in the middle of a write.")
(minutes 20)
(keywords "filesystem" "FAT" "journal (of a filesystem)")
```

This lesson is about how storage keeps named files, and keeps them safe
when the power fails.

A storage device is a long row of blocks. A **filesystem** is the set
of rules that turns those blocks into named files and folders. It
records which blocks belong to which file, and in what order; which
blocks are free; and each file's name, size and date.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Filesystems")
```

```check
(choice
  (ask "Which of these does a filesystem keep track of?")
  (several)
  (right "Which blocks belong to each file")
  (right "Which blocks are free")
  (right "Each file's name and size")
  (wrong "Which program the CPU is running"
    "That is the CPU's and the operating system's business. A filesystem organizes the blocks of storage.")
  (hint "A filesystem turns a row of blocks into named files."))
```

---

## FAT

**FAT**, first designed for floppy disks in the late 1970s, is the most
widely understood filesystem in the world: nearly every computer,
camera and operating system can read it. So it is the usual choice for
SD cards and USB sticks, and a sensible one for data that should
outlast the computer that wrote it.

Zeitlos keeps its files on an SD card in FAT, so the card can be read
by almost any other computer.

```sources
(source "Zeitlos docs: filesystem.md, Overview; fs_driver.md")
```

```check
(choice
  (ask "Why is FAT a good choice for a card you will read on other computers?")
  (right "Nearly every computer and operating system can read it")
  (wrong "It is the newest filesystem"
    "FAT is from the late 1970s. Its strength is that almost everything understands it.")
  (wrong "It cannot lose data"
    "No filesystem is safe from everything. FAT's strength is how widely it can be read.")
  (hint "What matters most when the card goes to another machine?"))
```

---

## The power fails

The hardest problem for a filesystem is a power cut in the middle of a
write. Changing a file usually means changing several blocks: the data,
the list of its blocks, and its entry in the folder. If the power fails
after some of those writes but not others, the file can point at the
wrong data.

The figure changes a file of three blocks from old (O) to new (N), then
writes one more small thing. Choose a way, in place or careful (a new copy, then
a commit), and when the power is cut.

```figure
(define way (choice 'way '("in place" "careful") "Way"))
(define k (slider 'cut 0 4 2 "Writes done" 1))
(define (blk x y label) (frame x y 40 30 'ink 1.5) (text (+ x 20) (+ y 21) label 'centre 'body 'bold))
(text 16 30 "the file, as read after the cut:" 'small)
(define cur (if (= way 0)
                (list (if (> k 0) "N" "O") (if (> k 1) "N" "O") (if (> k 2) "N" "O"))
                (if (= k 4) '("N" "N" "N") '("O" "O" "O"))))
(for-range 0 3 (lambda (j) (blk (+ 40 (* j 48)) 42 (list-ref cur j))))
(if (= way 1)
    (begin
      (text 16 104 "the new copy, in free blocks:" 'small)
      (for-range 0 3 (lambda (j) (blk (+ 40 (* j 48)) 116 (if (> k j) "N" "-"))))
      (text 200 140 (if (= k 4) "commit: done" "commit: not yet") 'small)))
(text 160 190
      (cond ((= k 0) "the old file, whole")
            ((= k 4) "the new file, whole")
            ((= way 1) "the old file, whole")
            ((= k 3) "data new, folder entry old")
            (else "half old, half new: broken"))
      'centre 'body 'bold)
(caption (if (= way 0) "Write 4 is the folder entry" "Write 4 is one small commit"))
```

```check
(choice
  (ask "The file is written over in place, and the power fails after 2 of its 3 data blocks. What is left?")
  (right "A file that is part new and part old")
  (wrong "The old file, whole"
    "Two of its blocks have already been written over. The old version is gone, and the new one is not finished.")
  (wrong "The new file, whole"
    "The third block was never written. The file mixes new and old.")
  (hint "Set the figure to in place with 2 writes done."))
```

---

## Careful rules

Careful filesystems follow a few rules:

- **Never write over in place.** Write the new version to an unused
  area.
- **Commit with one write.** Once the new version is whole and checked,
  make it the current one by changing one small value, best a single
  byte, that a power cut cannot leave half written.
- **Spread the writes.** On memory that wears, use free blocks in turn
  instead of always the same ones.

The small filesystem of Machdyne BASIC on its modules follows these
rules: wherever the power is cut, each file is as it was before or as
it is after; at worst, the end of a line being added is lost. Large
filesystems keep a **journal**: a log in which each change is written
before it is made, so that one cut short can be finished or undone at
the next start.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Surviving Power Loss")
```

```check
(choice
  (ask "With the careful way, the power fails just before the commit. What does the reader find?")
  (right "The old file, whole: the new copy was never made current")
  (wrong "Half of the new file"
    "The new blocks went to free space, and the file still points at the old ones until the commit.")
  (wrong "No file at all"
    "Nothing of the old file was written over, so it is all still there.")
  (hint "Until the commit, which version is the current one?"))
```

---

## Let it finish

You can help: let the computer finish writing before you pull out a
card or switch off. Systems have a command for this, often called sync,
eject or unmount, that writes everything out and then says the device
is safe to remove.

```check
(choice
  (ask "A computer is still saving to a USB stick when you pull it out. What can happen?")
  (right "The filesystem on the stick can be left half updated")
  (wrong "Nothing: a USB stick is non-volatile"
    "Non-volatile keeps what was written. What was not yet written, and the lists that point to it, can be left half done.")
  (wrong "Only the file being saved can suffer"
    "The block lists and folder entries are shared, so a cut write can damage more than one file.")
  (hint "Think of the several blocks a single save changes."))
```

So: a filesystem turns blocks into named files, and a careful one
writes the new version aside and commits it with one small write.
