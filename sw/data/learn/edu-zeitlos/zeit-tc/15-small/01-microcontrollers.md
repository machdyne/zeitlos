# Microcontrollers

```lesson
(objective "Say what a microcontroller has inside, how its program gets there, and why an interpreter on the chip helps a system last.")
(minutes 20)
(keywords "firmware" "bootloader")
```

You will meet the computers that most people never see.

Most computers in the world have no screen and no keyboard. They are
inside keyboards, chargers, washing machines and cars, and each runs
one program for its whole life. They are microcontrollers: a whole
computer on one cheap chip, with a processor, flash memory for its
program, a little RAM, and devices built in:

- general-purpose pins;
- timers, to measure time and make timed signals;
- converters that turn a voltage into a number;
- UART, SPI and I2C controllers.

A small RISC-V microcontroller might have 16 KB of flash and 2 KB of
RAM: enough to read sensors, switch outputs and decide what to do many
thousands of times a second.

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Microcontrollers")
```

```check
(number
  (ask "A microcontroller has 16 KB of flash and 2 KB of RAM. How many times more flash than RAM is that?")
  (answer 8) (from "16 / 2")
  (wrong-if 14 "14 KB is the difference. How many times more is a ratio: 16 divided by 2.")
  (hint "Divide 16 by 2."))
```

---

## Fixed, cheap and frugal

Unlike an FPGA computer, a microcontroller is fixed: the maker sets its
processor and devices, and only the program can change. In return it is
cheap, small, easy to use and sparing with power. Many can sleep on a
few microamps (millionths of an ampere) and wake on a timer or a pin
change, so they can run for years on a small battery.

```check
(choice
  (ask "A sensor must report once an hour for years from one small battery. Why does a microcontroller suit it?")
  (right "It can sleep on a few microamps between readings and wake on a timer")
  (wrong "It runs faster than an FPGA computer"
    "Its speed is modest, and here speed hardly matters. What matters is the tiny current it draws while it sleeps.")
  (wrong "Its devices can be redesigned for the sensor"
    "A microcontroller's devices are fixed by its maker. Its strength here is how little power it uses.")
  (hint "What does it do for most of each hour?"))
```

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: Microcontrollers")
```

---

## Firmware

A microcontroller's program is its **firmware**. It is usually written
in C, compiled on another computer, and written into the chip's flash
in one of two ways:

- through a debug connection of one or two wires, such as SWD, or the
  older and wider JTAG;
- through a **bootloader**: a small program kept in the chip that
  accepts new firmware over a serial port or USB.

```check
(choice
  (ask "A device has a bootloader. What does that let you do?")
  (right "Send it new firmware over a serial port or USB, with no debug connection")
  (wrong "Run programs without any firmware"
    "The bootloader is itself a small program in the chip. It is there to take in new firmware.")
  (wrong "Change the chip's built-in devices"
    "A microcontroller's devices are fixed. A bootloader only replaces the program.")
  (hint "What does the bootloader accept, and over what?"))
```

---

## An interpreter on the chip

Another way: an interpreter is written into the firmware once, and
programs are sent to it afterwards as text. Machdyne's modules work
this way, with Machdyne BASIC as their firmware. This helps a system
last:

- **No compiler is needed** to change a program: anything that can
  send text over a serial line or a bus can do it.
- **Programs are readable**: the program inside can be listed, read and
  changed years later, by someone who has never seen it.
- **Programs are portable**: a BASIC program does not depend on the
  chip, so it can move to a new module when the old chip is no longer
  made.

The cost is speed: an interpreted program runs many times more slowly
than compiled firmware.

```sources
(source "Timeless Computing (docs/tc.md), Small Computers: An Interpreter on the Chip")
```

```check
(choice
  (ask "A controller checks a greenhouse's temperature once a second. Does the interpreter's slowness matter?")
  (right "No: once a second is far within what it can do")
  (wrong "Yes: interpreted programs are always too slow for control"
    "They are many times slower than compiled ones, but a check once a second leaves the chip idle almost all the time.")
  (wrong "Yes, unless the program is compiled on another computer first"
    "That would give up the interpreter's advantages for speed that is not needed. Once a second is easy.")
  (hint "Compare how fast it must be with how fast a small chip is."))
```

---

```check
(choice (several)
  (ask "Years from now, someone must change the program in a module that runs BASIC. What helps them?")
  (right "The program inside is plain text they can list and read")
  (right "They can send the changed program from anything that sends text")
  (wrong "They need the exact compiler that built the first version"
    "With an interpreter on the chip, no compiler is needed at all. The program is sent as text.")
  (wrong "They need the original author's notes, since the program cannot be read back"
    "The program can be listed from the module itself, as text.")
  (hint "Two are true: think of what text gives."))
```

So: a microcontroller is a whole fixed computer on a cheap chip, and an
interpreter in its firmware keeps its programs readable and portable.
