# The bus, devices and interrupts

```lesson
(objective "Say how the CPU reaches devices through addresses, and how a device gets the CPU's attention.")
(minutes 25)
(keywords "address map" "memory-mapped I/O" "device register" "polling" "interrupt" "interrupt handler" "direct memory access" "DMA" "bus master" "blitter" "pixel")
```

This lesson is about how the CPU talks to everything that is not
memory.

The bus has wires for an address, wires for data, and a few control
signals that say whether a transfer is a read or a write and when it is
done. Every device on the bus has a range of addresses. A decoder looks
at the top bits of each address and enables the one device it belongs
to. The list of which device answers where is the computer's **address
map**. Here is a simple form of Zeitlos's: move the slider.

```figure
(define d (slider 'top 0 15 4 "Top hex digit" 1))
(define hd (cp-hex d 1))
(text 160 26 (string-append "address 0x" hd "000_0000") 'centre 'big 'bold)
(text 160 46 (string-append "top 4 bits " (cp-bin d 4) ": the decoder enables") 'centre 'small)
(define rows '((0 "boot program, in on-chip memory") (1 "flash memory")
               (2 "video memory: the screen") (4 "main memory")
               (8 "the current program's memory") (14 "general-purpose pins")
               (15 "serial port")))
(define hit #f)
(for-range 0 7
  (lambda (k)
    (let ((r (list-ref rows k)) (y (+ 72 (* k 18))))
      (text 24 y (string-append "0x" (cp-hex (car r) 1)) 'small)
      (text 60 y (cadr r) 'small)
      (if (= d (car r)) (begin (set! hit #t) (frame 16 (- y 13) 288 18 'ink 2))))))
(if (not hit) (frame 16 200 288 18 'ink 2))
(text 24 213 "others: devices left out of this short map" 'small (if hit 'grey 'ink))
```

```sources
(source "Timeless Computing (docs/tc.md), The Computer: The Bus and the Address Map")
(source "Zeitlos source: rtl/sysctl.v, the address decoder (cs_ wires)")
```

```check
(choice
  (ask "A program stores a word at address 0x2000_0010. Where does it go?")
  (right "To video memory: it changes what the screen shows")
  (wrong "To main memory"
    "Main memory starts at 0x4000_0000. The top hex digit, 2, makes the decoder enable video memory.")
  (wrong "To the CPU's register x2"
    "Registers are inside the CPU and have no address on the bus. The address alone picks the device: top digit 2 is the screen.")
  (hint "Look at the top hex digit of the address, and find it in the map."))
```

---

## Memory-mapped I/O

So the screen, the pins and the serial port are not reached with
special instructions: they have addresses, like memory. A store to a
device's address sends a value to it; a load reads from it. This is
**memory-mapped I/O**, and nearly every processor today talks to its
hardware this way.

The addresses a device answers to are its **device registers**, which
are not the CPU's registers. A device often has a register for data, a
status register whose bits say things like "a byte has arrived", and a
control register whose bits turn features on and off.

```check
(choice
  (ask "How does a RISC-V program send a byte out of the serial port?")
  (right "It stores the byte at the serial port's address")
  (wrong "It uses a special serial-port instruction"
    "RISC-V has no instructions for devices. A device is reached with ordinary loads and stores to its addresses.")
  (wrong "It writes the byte into register x31"
    "x31 is an ordinary CPU register. Only a store puts the value on the bus, at the device's address.")
  (hint "Every device has addresses, like memory."))
```

---

## A store to the pins

On a simple system, storing a word at 0xE000_0000 sets a row of output
pins from its lowest bits. Two instructions put x6 on the pins:

```asm
        lui  x7, 0xE0000     # x7 = 0xE000_0000, the pins' address
        sw   x6, 0(x7)       # send x6 to the pins
```

`lui` (load upper immediate) puts its 20-bit constant in the top 20
bits of a register and clears the lower 12: five hex digits followed by
three zeros. So software moves things in the world with a store to the
right address.

```check
(text
  (ask "What does lui x7, 0xF0000 put in x7? Write it in hex.")
  (accept "F0000000") (accept "0xF0000000") (accept "0xF000_0000") (accept "F000_0000")
  (wrong-if "F0000" "lui puts the constant in the top 20 bits, so three hex zeros follow it.")
  (wrong-if "0xF0000" "lui puts the constant in the top 20 bits, so three hex zeros follow it.")
  (hint "The 20 bits are 5 hex digits. The lower 12 bits, 3 hex digits, are cleared."))
```

---

## Waiting, or being told

A program can watch a device by reading its status register again and
again until it changes. This is **polling**. It is simple, but while it
waits the CPU does nothing else.

An **interrupt** lets the device ask for attention instead. When a key
is pressed, the keyboard circuit raises an interrupt signal. The CPU
finishes its instruction, saves the program counter and jumps to a
routine called the **interrupt handler**. The handler reads the key,
keeps it and returns, and the program goes on as if nothing had
happened.

```check
(choice
  (ask "A program waits for a key by reading the keyboard's status register in a loop. What is the drawback?")
  (right "The CPU can do nothing else while it waits")
  (wrong "It cannot work: devices can only be reached by interrupts"
    "Polling works: the status register can be read like any address. It only wastes the CPU's time.")
  (wrong "The key is lost when it arrives"
    "The status bit stays set until the program reads the key. Nothing is lost; the time spent checking is.")
  (hint "What else can the CPU run while it goes around the loop?"))
```

---

## The timer

A **timer** is a counter of clock cycles that raises an interrupt at
regular times. It lets an operating system, the program that runs the other programs,
share one processor between
several programs, giving each a turn.

On Zeitlos, the timer interrupt comes from a 16-bit counter driven by
the 48 MHz clock (48 million cycles a second): it interrupts each time
the counter goes all the way around.

```sources
(source "Zeitlos docs: kernel.md, The scheduler; The tick rate")
```

```check
(number
  (ask "A 16-bit counter counts 48,000,000 cycles a second and interrupts each time it wraps around. About how many interrupts a second is that?")
  (answer 732) (within 1) (from "48000000 / 2^16")
  (wrong-if 3000000 "That is 48,000,000 / 16. The counter has 16 bits, so it wraps after 2 to the 16th counts, 65,536.")
  (hint "A 16-bit counter wraps every 65,536 counts. Divide 48,000,000 by that."))
```

---

## Direct memory access

Moving a lot of data, such as filling the screen, one word at a time
keeps the CPU busy. Some devices can read and write memory over the bus
themselves: a **bus master**. This is **direct memory access** (**DMA**).

A common one is a **blitter**, which copies rectangles of **pixels**, the dots
that a picture on the screen is made of. The
CPU sets it up with a few stores to its registers and goes on with
other work while it copies. Zeitlos has one: this lesson's figures are
put on the screen with it.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Direct Memory Access")
(source "Zeitlos docs: gpu_blitter.md; learn_figures.md, Speed")
```

```check
(choice
  (ask "Why does a blitter make drawing faster for the whole computer?")
  (right "It copies the pixels itself, so the CPU can do other work meanwhile")
  (wrong "It makes the CPU's clock faster"
    "The clock stays the same. The gain is that the copying is done by another bus master, not by the CPU's own loads and stores.")
  (wrong "It stores pixels in the CPU's registers"
    "Registers hold a few words; a screen is far more. The blitter reads and writes memory over the bus by itself.")
  (hint "Who does the copying, and what is the CPU doing meanwhile?"))
```

So: every device has addresses on the bus, the CPU reaches it with
loads and stores, and a device asks for attention with an interrupt or
does its own copying by DMA.
