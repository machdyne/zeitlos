# Memory and storage

```lesson
(objective "Say where your work is while you write, where it goes when you save, and what is lost when the power goes off.")
(minutes 15)
(keywords "memory (working)" "storage (lasting)" "card (SD card)" "flash (on the board)")
```

You will learn where your work is while you write, and where it goes
when you save it.

A computer keeps things in two very different places:

- **Memory** holds the programs that are running and the work they
  have open: the words you are typing, the picture you are drawing. It
  is fast, and it is emptied the moment the power goes off.
- **Storage** keeps what is written to it with the power off, for
  years. It is slower than memory.

The figure shows both. Switch the power off with the second control,
and see what is left.

```figure
(define saved (toggle 'saved "Saved"))
(define on (toggle 'power "Power on" #t))
(text 80 30 "Memory" 'centre 'body 'bold)
(text 240 30 "Storage" 'centre 'body 'bold)
(frame 10 40 140 120 'ink 1.5)
(frame 170 40 140 120 'ink 1.5)
(if on
    (begin (frame 30 70 100 50 'ink 1.5)
           (text 80 92 "book.txt" 'centre 'small 'bold)
           (text 80 108 "being written" 'centre 'small))
    (text 80 104 "(empty)" 'centre 'small))
(if saved
    (begin (frame 190 70 100 50 'ink 1.5)
           (text 240 92 "book.txt" 'centre 'small 'bold)
           (text 240 108 "as saved" 'centre 'small))
    (text 240 104 "(no copy yet)" 'centre 'small))
(text 160 190 (if on "The power is on." "The power is off.") 'centre 'small)
(caption (if on
             (if saved "Your work is in memory, and a copy is kept."
                 "Your work is only in memory.")
             (if saved "Memory is empty. The saved copy is still kept."
                 "Memory is empty. The unsaved work is lost.")))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: The Memory Hierarchy")
```

---

## Saving

While you type in **text**, your words are only in memory. **Saving**
writes them to storage, as a file. Until you save, a power cut loses
everything since the last save.

So save often, and always before you switch off.

```check
(choice
  (ask "You write for an hour without saving, and the power goes off. What is left of that hour's work?")
  (right "Nothing: it was only in memory")
  (wrong "All of it: the computer keeps what is on the screen"
    "What is on the screen is in memory, and memory is emptied when the power goes. Only what was saved is kept.")
  (wrong "The last few minutes are lost, the rest is kept"
    "Nothing is written to storage until you save. Without a save, the whole hour was only in memory.")
  (hint "Try it in the figure: leave Saved off, then switch the power off."))
```

---

## The card

On a Zeitlos computer, your storage is an **SD card**: a small card
that plugs into a slot on the board. The card holds your files and
settings, and most of the apps.

A few things are kept on the board itself, in a chip of **flash**
memory, which keeps its contents with the power off. It holds the core
of the system: the desktop, **files**, **text**, **term** and a few
others. They work even when no card is in the slot.

```sources
(source "docs/layout.md, The tree and /apps")
(source "docs/flash_apps.md, Booting with no card")
```

```check
(choice (several)
  (ask "You start the computer with no card in the slot. Which of these still work? Choose all that do.")
  (right "files")
  (right "text")
  (wrong "The files you saved last week"
    "Your files are on the card. Without the card, they are not there to open.")
  (hint "The apps in the board's own flash work without a card. Your files are not among them."))
```

---

## Which is which

| | Keeps it with the power off? | What is there |
|---|---|---|
| memory | no | running programs, unsaved work |
| the card | yes | your files, settings, most apps |
| flash on the board | yes | the core of the system |

```check
(choice
  (ask "Where is a letter you are writing in text before you save it for the first time?")
  (right "Only in memory")
  (wrong "On the card"
    "It reaches the card only when you save. Before that it is in memory alone.")
  (wrong "In the flash on the board"
    "The flash holds the core of the system, not your work. Your work goes to the card when you save.")
  (hint "Nothing is written to storage until you save."))
```

---

```check
(choice
  (ask "Why does the computer not simply work straight from the card, with no memory at all?")
  (right "Memory is much faster, and can be rewritten without limit")
  (wrong "The card cannot hold programs"
    "The card holds most of the apps. They are loaded into memory to run, because memory is fast.")
  (wrong "Memory keeps things longer than the card"
    "It is the other way round: memory is emptied at power off, and the card keeps what is on it.")
  (hint "Think about speed: which of the two is fast?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: The Memory Hierarchy")
```

So: work in progress is in memory, a save puts it on the card, and only
what is saved survives the power going off.
