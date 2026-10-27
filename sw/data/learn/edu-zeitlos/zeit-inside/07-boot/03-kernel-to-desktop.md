# From kernel to desktop

```lesson
(objective "Name, in order, what the kernel starts between its first instruction and the desktop, and how to stop before the desktop.")
(minutes 20)
(keywords "init" "console log" "rescue console")
```

You will learn what happens after the kernel starts.

1. The **kernel** sets up the serial port, the keyboard registers and
   the memory pool, makes itself process 0, and starts the kernel
   shell.
2. The shell starts **`fs`** from flash and waits for it, then, on
   boards with a USB host, **`usb`** (from the card if a copy is
   there), then reads the settings file,
   `/sys/zeitlos.cfg`.
3. It runs **init**, which starts `wm`, `net` and `console`, and
   others if the settings ask for them. Each program is looked for on
   the card first, then in flash.
4. When init is done it registers the name `init0`. Until then the dock
   is disabled and the pointer is the busy Z.

```sources
(source "Zeitlos docs: boot.md, Sequence; userspace_drivers.md, Drivers (boot order)")
```

```check
(choice
  (ask "Why does the shell read /sys/zeitlos.cfg before running init?")
  (right "So every program init starts sees the settings from its first instruction")
  (wrong "Because the settings file says which drivers to start"
    "fs and usb are started before the settings are read. The settings are read first so every later program has them.")
  (wrong "Because init is written in the settings file"
    "init is part of the kernel shell. The order is so that apps find the settings already loaded."))
```

---

## Only one is essential

Init carries on past anything that fails, with one exception: `wm`.
Without the window manager there is no desktop to start anything else
in, so if `wm` cannot start, or crashes later, the **rescue console**
comes up instead: the kernel shell on the screen, typed at on the USB
keyboard. `run wm` brings the desktop back.

You can also reach it on purpose: hold Esc on the keyboard while the
machine starts, and init is skipped. On the desktop, Ctrl+Alt+Esc shows
it at any time, and Ctrl+Alt+Esc again goes back (built and tested on
the build machine, not yet on a board, as of October 2026).

```sources
(source "Zeitlos docs: boot.md, Sequence (wm is the only fatal one); rescue_console.md")
```

```check
(choice
  (ask "A broken wm is on the card and the desktop does not come up. What do you get?")
  (right "The rescue console, where you can type commands to the kernel")
  (wrong "A blank screen until the machine is flashed again"
    "The kernel brings up the rescue console when wm cannot start, so the machine can be put right from the keyboard.")
  (wrong "The DFU bootloader"
    "The bootloader is long finished by then. The kernel is running; only the desktop is missing."))
```

---

## The console log

Everything the kernel prints goes to the serial port, and also into a
4 KB **console log** the kernel keeps in memory. In `term`, the CONSOLE
button replays it, so you can read how this machine started: the
features of its gateware, its memory, its CPU, and a line for each
program init started, saying where it came from (`init: wm (flash)`).

```sources
(source "Zeitlos docs: console.md, What you get; boot.md, What the boot log tells you")
```

```try
(try (app "term")
  (task "Open term from the dock and click CONSOLE. Scroll to the top of what it shows and find the line that starts with - cpu:. Then close the window and come back here. (If the log begins with a line saying earlier output was overwritten, the boot lines are gone; answer from what you have learned.)")
  (check (number (ask "What clock speed does the cpu line state, in MHz?") (answer 48)
    (wrong-if 50 "50 MHz is a rounder number, but Zeitlos runs at 48 so that the serial port's speed divides evenly.")
    (hint "It is the same on every board.")
    (hint "The lesson The clock and the tick worked with it."))))
```

---

## Practice

```check
(choice
  (ask "In what order are these started at boot?")
  (keep-order)
  (right "fs, then usb, then wm")
  (wrong "wm, then fs, then usb" "wm needs files to start, so fs comes first. wm is started last of these, by init.")
  (wrong "usb, then fs, then wm" "usb can come from the card, and the card can only be read once fs runs. So fs is first.")
  (wrong "fs, then wm, then usb" "usb is started by the shell before init, and init starts wm."))
```

```check
(choice
  (ask "The dock does nothing and the pointer is a Z, a few seconds after power on. What is going on?")
  (right "init has not finished starting programs yet")
  (wrong "wm has crashed" "If wm had crashed you would see the rescue console. The busy Z with a disabled dock means init is still at work.")
  (wrong "The BIOS is still copying the kernel" "By the time the dock is drawn, wm is running, so the BIOS finished long before."))
```

So: the kernel starts `fs`, `usb`, the settings, then init, which starts `wm` and the rest; if `wm` fails, the rescue console takes its place.
