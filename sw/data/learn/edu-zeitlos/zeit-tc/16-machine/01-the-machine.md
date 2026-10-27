# What the machine is

```lesson
(objective "Name the parts of a Zeitlos machine and its SOC, and say how each program sees its own memory.")
(minutes 20)
(keywords "dock" "configuration flash" "MMOD")
```

You will see the earlier units come together in one real computer: a
Machdyne FPGA computer running Zeitlos.

With a keyboard, mouse, monitor and USB power supply joined to it, the
machine starts to a graphical desktop. The screen is 640 by 480 pixels
in one colour, as on early workstations: white, amber or green on
black, or black on white like paper. A **dock**, a row of icons along the bottom, starts
programs, and windows can be moved, resized and stacked.

The core of the system, the kernel, the drivers, the window manager,
the terminal, the editor, the file browser and Scheme, is kept in the
computer's flash, so the machine works with no storage card at all. A
microSD card adds more programs and room for files.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: What It Is")
(source "Zeitlos documentation: flash_apps (docs/flash_apps.md)")
```

```check
(choice
  (ask "The machine is switched on with no microSD card. What happens?")
  (right "It starts to the desktop, with its core programs from flash")
  (wrong "It cannot start without a card"
    "The kernel, the drivers, the window manager and the core programs are in flash. The card only adds more.")
  (wrong "It starts, but with no way to edit text or find files"
    "The editor and the file browser are core programs, kept in flash with the rest of the core.")
  (hint "Where is the core of the system kept?"))
```

---

## The hardware

Machdyne makes several FPGA computers, and Zeitlos runs on some boards
from other makers too, so details vary. The common parts:

- an **FPGA**, usually a Lattice ECP5, which becomes the whole computer;
- **external RAM**, from 1 MB of SRAM to tens of megabytes;
- **configuration flash**, the flash that holds the FPGA's bitstream and
  the core of the system, often on a removable **MMOD**, a small SPI
  memory module;
- **USB host ports**, for a keyboard, a mouse, storage and networks;
- **video**: DVI through an HDMI connector, VGA, or composite for a TV;
- a **microSD** slot, and **Pmod** sockets for expansion;
- power from a USB supply: a few watts.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The Hardware")
(source "Zeitlos documentation: mmod (docs/mmod.md)")
```

```check
(choice
  (ask "Two of these machines have the same board. One has its MMOD swapped for another holding a different design. What differs?")
  (right "What computer the FPGA becomes at power-on")
  (wrong "Nothing, since the boards are the same"
    "The MMOD holds the FPGA's design. A different design makes the same board into a different computer.")
  (wrong "Only the files on the microSD card"
    "The MMOD is the configuration flash, not the card. It decides what circuit the FPGA becomes.")
  (hint "What does the configuration flash hold?"))
```

---

## The SOC

| From the unit | In the Zeitlos SOC |
|---|---|
| The Computer | RISC-V CPU (RV32IM), 48 MHz, PicoRV32 core |
| The Computer | Wishbone bus, address map, caches |
| Operating Systems | a window per program, memory protection |
| Talking to the World | framebuffer (the screen in memory), blitter, line drawing |
| Talking to the World | audio mixer, USB host, serial ports |
| Talking to the World | pins on the Pmod sockets, I2C and SPI |
| Communication | Ethernet |
| Security | random number generator |
| Systems on a Chip | multiplier and hash hardware for cryptography |

Wishbone is an open standard for an SOC's bus.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The System on the Chip")
```

```check
(number
  (ask "The framebuffer holds the 640 by 480 screen at one bit per pixel. How many bytes is that?")
  (answer 38400) (from "640 * 480 / 8")
  (wrong-if 307200 "That is the number of pixels, one bit each. 8 bits make a byte, so divide by 8.")
  (hint "First the pixels: 640 times 480.")
  (hint "One bit each, and 8 bits to a byte: divide by 8."))
```

---

## A window for each program

There is no memory management unit. Each program is loaded somewhere in
main memory, but always sees itself at the address `0x8000_0000`, its
**window**. At each switch between programs, the kernel sets where the
window points for the next one. So no program needs adjusting for where
it was loaded, and every byte of memory also keeps its one physical
address.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The System on the Chip")
(source "Zeitlos documentation: kernel, Processes (docs/kernel.md)")
```

```check
(choice
  (ask "Two programs are loaded at different places in memory. Where does each see its own memory start?")
  (right "Both at 0x8000_0000")
  (wrong "Each at the physical address where it was loaded"
    "That would mean adjusting each program for where it lands. Instead each sees itself in the same window, at 0x8000_0000.")
  (wrong "At 0x0000_0000"
    "The window is at 0x8000_0000. The kernel points it at the running program's block.")
  (hint "What does the kernel change at each switch between programs?"))
```

---

## Protection that contains

A memory protection unit lets a program write only its own memory and
the devices it may use, and run instructions only from its own memory;
it may read nearly any memory. A faulty program is stopped with a
report, and everything else goes on.

Zeitlos has one user. This protection is there to contain mistakes,
not to defend against a hostile program the owner chose to run.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: The System on the Chip")
(source "Zeitlos documentation: mpu (docs/mpu.md)")
```

```check
(choice
  (ask "A program with a bug writes to memory belonging to another program. What happens?")
  (right "The faulty program is stopped with a report; the others carry on")
  (wrong "The whole machine stops"
    "Protection is there to contain the mistake: only the program at fault is stopped.")
  (wrong "Nothing: the write goes through"
    "A program may write only its own memory and its devices. The write is refused and the program stopped.")
  (hint "What is the protection for?"))
```

So: a Zeitlos machine is an FPGA, its RAM and an MMOD, and the SOC on it
gives each program its own window and contains each program's mistakes.
