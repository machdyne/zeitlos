# From power to the BIOS

```lesson
(objective "Describe what happens from power on until the kernel's first instruction, and find the kernel in the CPU's address map.")
(minutes 20)
(keywords "DFU bootloader" "BIOS" "PROGRAMN")
```

You will learn the first steps of a Zeitlos boot.

Move the slider through the stages, from power on to the kernel.

```figure
(define s (slider 'stage 0 4 0 "Stage" 1))
(define names '("FPGA loads flash from 0" "DFU bootloader, 5 s"
                "FPGA loads the Zeitlos design" "BIOS copies the kernel"
                "kernel at 0x4000_0000"))
(for-range 0 5
  (lambda (k)
    (zi-box 40 (+ 10 (* 42 k)) 240 28 (list-ref names k) (= k s))
    (if (< k 4) (arrow 160 (+ 38 (* 42 k)) 160 (+ 51 (* 42 k)) 'ink 1.5))))
```

At power on an FPGA always reads its first design from the start of
its flash, address 0. On the ECP5 boards (ECP5 is the family of FPGA on
most Machdyne boards) that ship with it, that is the
**DFU bootloader**: a small design that waits about five seconds for a new
system over USB. Then it pulls the FPGA's **PROGRAMN** pin, which makes
the FPGA load a new design, this time from `0x040000`: the Zeitlos
gateware. A board flashed without the bootloader, and a board with an
Artix-7, another family of FPGA, has the Zeitlos gateware at 0.

```sources
(source "Zeitlos docs: zboot.md, 2. BOOTADDR; 3. How the DFU bootloader uses it; boot.md, Reboot")
```

```check
(choice
  (ask "On a board with the DFU bootloader, why can a bad experiment in flash never stop the bootloader from starting?")
  (right "At power on the FPGA always loads from address 0, where the bootloader is")
  (wrong "Because the bootloader is in the FPGA's own memory"
    "The bootloader is in the flash, at address 0. What protects it is that power on always starts there.")
  (wrong "Because Zeitlos checks the flash before starting"
    "Zeitlos is not running yet when the FPGA picks its first design. The FPGA itself always starts at 0."))
```

---

## The BIOS

The Zeitlos gateware includes the **BIOS**, a program in the 8 KB boot
RAM at `0x0000_0000`, so the CPU has something to run the moment the
design is loaded. The pointer appears at once, as the busy **Z** in the
middle of the screen: it comes out of reset that way.

The BIOS clears the screen and copies the kernel, 160 KB, from the
start of the Zeitlos part of the flash into main memory at
`0x4000_0000`, then jumps there. If the kernel's first word reads as all
ones or all zeros, it prints `no kernel` and stops instead.

On the DDR3 boards (Mozart ML2, Sergei ML2, Noir, Klinge) the BIOS
first trains the DDR3 memory, tuning how its data is read; that leaves
no room in 8 KB for anything else.

```sources
(source "Zeitlos docs: boot.md, Sequence; No boot logo; ddr3.md, the introduction")
(source "Zeitlos source: sw/bios/bios.c, BIOS_DDR3 and ROM_OS_SIZE")
```

```check
(choice
  (ask "Why must the BIOS be inside the gateware, not in main memory?")
  (right "Main memory holds nothing at power on; the boot RAM is filled with the design itself")
  (wrong "Main memory is too slow for the BIOS"
    "Speed is not the reason. At power on main memory is empty, and on DDR3 boards it does not even work until the BIOS has trained it.")
  (wrong "Because the BIOS is privileged"
    "It is privileged because of where it is fetched from. It is in the gateware because it must exist before anything is loaded."))
```

---

## The flash in the address map

The CPU can read the flash like memory: it is region 1, so flash offset
N is at address `0x1000_0000` + N. The Zeitlos part of the flash starts
at a **base** that depends on the board's FPGA: `0x100000` on the ECP5
boards, `0x300000` on the Artix-7 boards, whose gateware is bigger. The
kernel is at the start of it.

```sources
(source "Zeitlos docs: boot.md, The flash layout; csrs.md, FLASH_BASE (word 63)")
(source "Zeitlos source: sw/bios/bios.c, ROM_OS_ADDR")
```

```check
(text
  (ask "On an ECP5 board, at which CPU address does the BIOS find the kernel in flash?")
  (accept "0x10100000") (accept "10100000") (accept "0x1010_0000") (accept "1010_0000")
  (wrong-if "0x00100000" "That is the offset in the flash chip. The CPU sees the flash in region 1, from 0x1000_0000.")
  (wrong-if "0x40000000" "That is where the kernel is copied to, in main memory. It is read from region 1.")
  (hint "0x1000_0000 plus the base, 0x0010_0000."))
```

---

## Practice

```check
(text
  (ask "And on an Artix-7 board?")
  (accept "0x10300000") (accept "10300000") (accept "0x1030_0000") (accept "1030_0000")
  (wrong-if "0x10100000" "That is the ECP5 base. The Artix-7 boards' Zeitlos part starts at 0x300000.")
  (hint "0x1000_0000 plus 0x0030_0000."))
```

```check
(choice
  (ask "On a Mozart ML2, the kernel reboots the machine. What does it actually do?")
  (right "It makes the FPGA pull PROGRAMN, so the FPGA loads its design again")
  (wrong "It jumps back to the BIOS at address 0"
    "A reboot reconfigures the whole FPGA through PROGRAMN, so everything starts again from the flash. (A board whose PROGRAMN pin is not wired for it, such as the ULX3S, cannot reboot itself.)")
  (wrong "It turns the power off and on"
    "The board cannot switch its own power. It pulls PROGRAMN, as the DFU bootloader does."))
```

So: the FPGA loads its design from flash, the BIOS in the boot RAM copies the kernel from region 1 to `0x4000_0000`, and the kernel starts there.
