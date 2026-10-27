# The address map

```lesson
(objective "Find which part of the Zeitlos SOC answers at a given address.")
(minutes 20)
(keywords "region (of addresses)" "app window" "MTU")
```

You will learn to read any Zeitlos address and say what is there.

Every address the CPU sends out is 32 bits, eight hex digits. The
Zeitlos SOC decides who answers by looking first at the **first hex
digit**, the top nibble. That splits the addresses into 16 **regions**,
one for each digit from 0 to F, and gives each region to one part of
the SOC. (A few regions, such as 6, 7 and F, are shared by several
small parts, told apart by the next digits.)

Move the slider to walk through the 16 regions.

```figure
(define r (slider 'region 0 15 4 "Region" 1))
(zi-address-map r)
(caption (string-append "0x" (zi-hexd r) "000_0000 to 0x" (zi-hexd r) "FFF_FFFF"))
```

```sources
(source "Zeitlos source: rtl/sysctl.v, the chip-select decodes (wire cs_...)")
(source "Zeitlos docs: hwmap.md, Reading the map (one column per top address nibble)")
```

```check
(choice
  (ask "Which part answers at address 0xD000_0008?")
  (right "The blitter")
  (wrong "Main memory"
    "Main memory is region 4. Only the first hex digit decides the region, and here it is D.")
  (wrong "The SD card"
    "The SD card is region B. Look again at the first digit: D.")
  (hint "Read only the first hex digit, then find that region in the figure."))
```

---

## How big a region is

The 7 hex digits after the first one are the address inside the
region. So each region spans from `0xN000_0000` to `0xNFFF_FFFF`, which
is $2^28$ bytes.

```check
(number
  (ask "How many megabytes is one region? (A megabyte here is 2 to the power 20 bytes.)")
  (answer 256) (from "2^28 / 2^20")
  (wrong-if 16 "16 is how many regions there are. Each one spans 7 hex digits, 28 bits.")
  (wrong-if 28 "28 is the number of bits inside a region, not its size. 28 bits make 2 to the power 28 bytes.")
  (hint "7 hex digits are 28 bits.")
  (hint "2 to the power 28, divided by 2 to the power 20, is 2 to the power 8."))
```

---

## Memory, and the rest

Five regions hold memory:

- **0**: the boot RAM, 8 KB of block RAM with the BIOS in it;
- **1**: the flash chip, which the CPU can read like memory;
- **2**: VRAM, the screen's pixels;
- **3**: glyph memory, the font the blitter draws text from;
- **4**: main memory, the board's RAM chips.

On the 512 MB boards (Mozart ML2, Sergei ML2, Klinge) main memory is too big for
one region and takes **5** as well: `0x4000_0000` to `0x5FFF_FFFF`. On
the others, region 5 is not used.

Nearly every other region is a device, reached by memory-mapped I/O:
its registers are addresses in its region.

```sources
(source "Zeitlos docs: ddr3.md, Memory map (MAIN_512MB); mpu.md, The rules (BIOS RAM 0x0000_0000 to 0x0000_1FFF)")
```

```check
(choice
  (ask "On a Mozart ML2, what is at 0x5000_0000?")
  (right "Main memory")
  (wrong "Nothing: region 5 is unused"
    "That is true on boards with less memory. A 512 MB board needs two regions for its memory: 4 and 5.")
  (wrong "Ethernet"
    "Ethernet is region 6. Region 5 is the second half of the ML2's 512 MB.")
  (hint "512 MB is two regions of 256 MB."))
```

---

## A region the CPU sees differently

Region **8** is not a device. It is the **app window**: whatever program
is running sees its own memory there, starting at `0x8000_0000`. A part
of the SOC called the **MTU**, the memory translation unit, sends each
address in region 8 to that program's real place in main memory. The
lesson
[The MTU](lesson:memory/mtu) shows how.

Region **9** holds the MTU's own register, and the registers of the
MPU, the memory protection unit.

```check
(choice
  (ask "A program reads its first instruction from 0x8000_0000. Where is that instruction really?")
  (right "Somewhere in main memory, where that program was loaded")
  (wrong "In a separate memory chip for programs"
    "There is no such chip. Region 8 is a window onto the program's own part of main memory.")
  (wrong "In the boot RAM"
    "The boot RAM is region 0 and holds the BIOS. A program's code is loaded into main memory.")
  (hint "Region 8 is a window, not a device."))
```

---

## No answer

On this bus a read or a write must be answered by the part it is
addressed to. If no part of the SOC answers an address, the CPU can wait
for the answer for ever. One part answers every address in its region for that
reason: on boards with a USB host, the host answers anything in region
C, and reads 0 where it has no register.

So the map matters. A wrong address is not just a wrong value: it can
stop the machine.

```sources
(source "Zeitlos source: rtl/sysctl.v, the USB_HOST decode (an unacked address hangs picorv32_wb forever)")
```

```check
(choice
  (ask "Why does the USB host answer every address in its region, even ones that are not its registers?")
  (right "So that a stray address there reads 0 instead of leaving the CPU waiting for ever")
  (wrong "Because USB devices can be at any address"
    "USB devices are reached through the host's few registers, not by addresses of their own on the CPU's bus.")
  (wrong "To make USB faster"
    "Answering unused addresses does not speed anything up. It is there so that a mistake cannot hang the machine."))
```

---

## Practice

```check
(text
  (ask "What is the first hex digit of every address in VRAM, the screen?")
  (accept "2") (accept "0x2")
  (wrong-if "4" "4 is main memory. The screen has a region of its own.")
  (hint "VRAM is one of the memory regions."))
```

```check
(choice
  (ask "The serial port's registers start at 0xF000_0000. A program writes a byte to that address. What happens?")
  (right "The byte is sent out of the serial port")
  (wrong "The byte is stored in main memory"
    "Region F is not memory. A store there goes to the serial port's register, which sends it.")
  (wrong "Nothing: a program cannot write to F"
    "Memory-mapped I/O means a device is written with an ordinary store to its address."))
```

```check
(choice
  (ask "Which region would you look in for the line rasterizer's registers?")
  (right "A")
  (wrong "D" "D is the blitter, which fills and copies rectangles. Lines have their own engine.")
  (wrong "2" "2 is VRAM, where the rasterizer draws. Its own registers are in another region.")
  (hint "Move the slider in the first step to find it."))
```

So: the first hex digit of an address picks one of 16 regions, and each region belongs to one part of the SOC.
