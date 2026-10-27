# A computer on one chip

```lesson
(objective "Name the main parts of the Zeitlos SOC, and say which of them differ from board to board.")
(minutes 15)
(keywords "gateware" "board block" "PicoRV32" "BIOS" "block RAM" "line rasterizer")
```

You will learn what the Zeitlos computer is made of inside its FPGA.

A Zeitlos computer is a circuit board with an FPGA, memory chips, a
flash chip and sockets. When the power comes on, the FPGA loads a design
from the flash, and that design turns it into a computer: the **Zeitlos
SOC**. A design like this is described in a language and built like a
program, so it is called **gateware**. The Zeitlos gateware is Verilog,
in the `rtl` folder of the Zeitlos source; its top part is `sysctl`,
in `rtl/sysctl.v`.

```sources
(source "Zeitlos docs: hwmap.md (the map drawn from rtl/sysctl.v); tc.md, The Machine")
```

```check
(choice
  (ask "Where does the processor of a Zeitlos computer come from?")
  (right "It is part of the gateware: the FPGA's logic becomes the CPU")
  (wrong "It is a separate RISC-V chip on the board"
    "There is no processor chip on the board. The CPU is described in Verilog and built out of the FPGA's logic, with everything else.")
  (wrong "The kernel creates it when it starts"
    "The kernel is a program, and a program needs a CPU already there to run on. The CPU exists as soon as the FPGA has loaded its design.")
  (hint "What does the FPGA load from flash when the power comes on?"))
```

---

## The parts

The SOC has the parts every computer has, all on the one chip:

| Part | What it is |
|---|---|
| CPU | PicoRV32, a RISC-V core (RV32IM), at 48 MHz |
| boot RAM | 8 KB of block RAM holding the first program, the BIOS |
| memory controller | for the board's main memory chips |
| VRAM | the screen's picture: 640 x 480 pixels, one bit each |
| GPU | the blitter, the line rasterizer, the video output |
| peripherals | serial port, USB, SD card, Ethernet, audio, pins |

The CPU is **PicoRV32**, an open RISC-V design. The boot RAM is
**block RAM**, memory inside the FPGA itself, and the program in it is the
**BIOS**, the first thing the CPU runs. The **line rasterizer** is the
part of the GPU that draws lines.

They are all joined by buses that follow Wishbone, an open standard for
buses inside a chip. Each part has its own lesson later
in this course.

```sources
(source "Zeitlos docs: boot.md (the BIOS in block RAM, the 48 MHz clock); gpu_blitter.md, Screen Specifications; tc.md, The System on the Chip")
```

```check
(choice
  (ask "Which part holds the picture you see on the screen?")
  (right "VRAM")
  (wrong "The boot RAM"
    "The boot RAM holds the BIOS, the first program the CPU runs. The picture is in VRAM, video memory.")
  (wrong "The GPU"
    "The GPU draws into the picture, but it does not hold it. The pixels themselves are bits in VRAM.")
  (hint "The V in its name stands for video."))
```

---

## Boards differ

Machdyne makes several boards, and Zeitlos runs on a few others. They
do not all have the same chips, so the gateware is built differently
for each. Every board has a **board block** in `rtl/boards.vh`: a list
of switches (defines) that say what that board's SOC contains, such as
`MEM_SDRAM`, `ICACHE` or `USB_HOST`.

| Board | Main memory |
|---|---|
| Obst | 1 MB of SRAM |
| Lakritz, Mozart ML1, Sergei ML1, ULX3S | 32 MB of SDRAM |
| Noir | 256 MB of DDR3 |
| Mozart ML2, Sergei ML2, Klinge | 512 MB of DDR3 |

The CPU and the boot RAM are the same on every board, and so is the
layout of the address map (next lesson).

```sources
(source "Zeitlos source: rtl/boards.vh, each board's MEM and MEM_* defines")
(source "Zeitlos docs: csrs.md, Overview")
```

```check
(number
  (ask "How many times more main memory does a Mozart ML2 have than an Obst?")
  (answer 512) (from "512 / 1")
  (wrong-if 32 "32 MB is the SDRAM boards. The ML2 has 512 MB, and the Obst 1 MB.")
  (hint "512 MB against 1 MB."))
```

---

## One system for every board

Zeitlos does not have a different kernel for each board. One kernel
binary runs on all of them, and at start-up it asks the hardware what
this board has: how much memory, which devices. The next two lessons
show where it looks.

The `info` app shows the answer as a list of short tags, one for each
feature the gateware was built with: `VRAM`, `BLIT` for the blitter,
`SDRAM` or `SRAM` for the kind of main memory, and so on.

```sources
(source "Zeitlos docs: boot.md, How software finds it (one kernel binary runs on every board); info_app.md, Feature tags")
```

Open `info` from the dock (it is in the Tools group). Under the
Zeitlos name is the list of feature tags. Find the tag for the line
rasterizer, the hardware that draws lines, then come back here.

```check
(text
  (ask "What is the line rasterizer's tag in info?") (accept "RASTER")
  (wrong-if "GPU" "GPU is the tag for the graphics hardware as a whole. The rasterizer has a tag of its own.")
  (wrong-if "BLIT" "BLIT is the blitter, which fills and copies rectangles. The line drawer's tag is another word.")
  (hint "It is the second word of the name line rasterizer, in capitals."))
```

---

```check
(choice
  (ask "A friend's board has 32 MB of SDRAM and yours has 512 MB of DDR3. What is true?")
  (right "The same kernel runs on both; it reads how much memory there is when it starts")
  (wrong "Each needs a kernel built for its own memory size"
    "One kernel binary runs on every board. It asks the hardware at start-up instead of assuming.")
  (wrong "The 512 MB board has a faster CPU"
    "The CPU is the same PicoRV32 at 48 MHz on every board. Only what the board block switches on differs.")
  (hint "Think about what the board block changes, and what stays the same."))
```

---

```check
(choice
  (ask "What is gateware?")
  (right "A hardware design, written like a program, that an FPGA loads to become a circuit")
  (wrong "The program the CPU runs first when the power comes on"
    "That is the BIOS, which is software in the boot RAM. Gateware is the design that makes the CPU and the boot RAM in the first place.")
  (wrong "The wiring printed on the circuit board"
    "The board's wiring is fixed when it is made. Gateware is loaded into the FPGA and can be changed and loaded again."))
```

So: the Zeitlos SOC is gateware that turns an FPGA into a whole computer, the same design everywhere except what each board block switches on.
