# How the system runs

```lesson
(objective "Follow the machine from power-on to the desktop, and say how its kernel, drivers, files and languages are arranged.")
(minutes 20)
(keywords "zfed")
```

You will follow the machine from power-on to the desktop, then look at
how the running system is arranged.

1. The FPGA loads the Zeitlos design from its configuration flash and
   becomes the computer. Some boards first run a small bootloader that
   waits a few seconds for an update over USB.
2. A boot program, built into the design in block RAM, copies the
   kernel from flash into main memory and starts it.
3. The kernel starts the filesystem driver, then the USB driver, reads
   the configuration file, and starts the window manager, the network
   driver and the other services the configuration names.
4. The desktop appears.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Starting Up")
(source "Zeitlos documentation: boot (docs/boot.md)")
```

```check
(choice
  (ask "What does the boot program in block RAM do?")
  (right "Copies the kernel from flash into main memory and starts it")
  (wrong "Loads the FPGA's design from flash"
    "The FPGA loads its own design before any program can run. The boot program is part of that design, and runs next.")
  (wrong "Draws the desktop"
    "The desktop comes last, from the window manager. The boot program only brings in the kernel.")
  (hint "It runs as soon as the FPGA has become the computer."))
```

---

## Card before flash

A program is looked for first in the `/apps` folder of the microSD
card, then in flash. A newer version of a program can therefore be
tried by putting it on the card, and taking it off brings back the
flash copy. Only the kernel and the filesystem driver always come from
flash.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Starting Up")
(source "Zeitlos documentation: flash_apps, The search path (docs/flash_apps.md)")
```

```check
(choice
  (ask "The card's /apps folder has a new version of the text editor, text, which is also in flash. Which one starts?")
  (right "The one on the card")
  (wrong "The one in flash, since the core is kept there"
    "The card is looked at first. A program put in /apps on purpose wins over the flash copy.")
  (wrong "Whichever is newer"
    "The system compares no versions or dates. A copy on the card always wins.")
  (hint "Where is a program looked for first?"))
```

---

## A microkernel

Zeitlos has a **microkernel**: the kernel schedules programs, manages
memory, passes messages, routes interrupts and enforces protection. The
filesystem, USB and the network are driver programs that run like any
other. If one crashes, the kernel starts it again a moment later.

Scheduling is preemptive (the kernel takes the processor back from a
program when its turn is up), driven by a timer several hundred times a
second. When a program sends a message and waits for the answer, the
processor switches straight to the program that will answer it, and
programs waiting for an event use no time.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The Operating System")
(source "Zeitlos documentation: kernel (docs/kernel.md)")
```

```check
(choice
  (ask "The filesystem driver crashes. What happens?")
  (right "The kernel starts it again a moment later; open files may need opening again")
  (wrong "The whole machine stops, since the filesystem is part of the kernel"
    "In a microkernel the filesystem is a driver program outside the kernel. The kernel restarts it.")
  (wrong "Nothing more can be read until the machine is restarted"
    "The kernel restarts a crashed driver by itself, and most programs using it recover.")
  (hint "Is the filesystem inside the kernel or outside it?"))
```

---

## Files

Files are in one tree:

| Path | Holds |
|---|---|
| / | the microSD card, with the flash underneath |
| /apps | programs |
| /data | each program's own files, a folder each |
| /docs | the documentation, this book's too |
| /sys | the system's settings |
| /ram | a RAM disk: fast, and lost at power-off |
| /usb | a USB stick, when one is plugged in |
| /home | the owner's files |

Settings are lines in a plain text file, `/sys/zeitlos.cfg`, which can
be read and edited like any other.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The Operating System")
(source "Zeitlos documentation: layout (docs/layout.md), config (docs/config.md)")
```

```check
(text
  (ask "You write a letter you want to keep. Which folder does it belong in? (Type the path.)")
  (accept "/home") (accept "home") (accept "/home/")
  (wrong-if "/ram" "/ram is lost at power-off. Your own files go in /home.")
  (wrong-if "/data" "/data holds each program's own files. Your files go in /home.")
  (hint "Which folder holds the owner's files?"))
```

---

## The languages

- **Scheme** is always there, from flash, in a terminal window, with
  procedures for files, windows, drawing, programs, the network and the
  pins.
- **BASIC** runs as a separate small computer with a 320 by 240 screen
  and its own files, but no access to the rest of the system: a safe
  place to learn.
- **C** programs are compiled on another computer with the standard
  open tools, as Zeitlos itself is. An experimental compiler on the
  machine, `zcc`, builds simple programs, not the system's own.

```check
(choice
  (ask "A learner wants a language in which a mistake cannot touch the rest of the system. Which fits best?")
  (right "BASIC")
  (wrong "Scheme"
    "Scheme has procedures for files, programs and pins: it can reach the system. BASIC has no access to the rest of it.")
  (wrong "C"
    "A C program is a program like any other, with its devices and memory. BASIC is kept apart from the system.")
  (hint "Which runs as a separate small computer?"))
```

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The Languages")
```

---

## Messages between machines

Zeitlos has a bulletin board, a system where messages are posted for
others to read, and **zfed**, a way to pass messages
between machines that store and forward them, as bulletin boards did:

- no central server: each machine chooses the others it trades with;
- absence is normal: when two meet, each takes the messages it lacks,
  so a machine off for a week loses nothing;
- every message is signed by the machine that made it and named by its
  hash, so none can be changed on the way, and one that arrives twice
  is kept once.

```check
(choice
  (ask "A machine is switched off for a week. What happens to the zfed messages sent meanwhile?")
  (right "It takes them from the others the next time it connects")
  (wrong "They are lost"
    "Absence is normal in zfed. When the machine connects again, it takes every message it does not yet have.")
  (wrong "They wait on a central server"
    "zfed has no central server. Any machine it trades with can pass them on.")
  (hint "Store and forward: each takes what it lacks."))
```

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Messages Between Machines")
(source "Zeitlos documentation: fed (docs/fed.md)")
```

So: the FPGA becomes the computer, a boot program brings in the
microkernel, and drivers, files and languages run on top of it.
