# Loading the configuration

```lesson
(objective "Say where your machine's bitstream is kept, and what happens at power-on and at a reload.")
(minutes 15)
(keywords "DFU bootloader (FPGA)" "PROGRAMN (pin)" "boot address (FPGA)" "MMOD (module)")
```

An LFE5U-25F has 559,588 bytes of configuration memory: about 560 KB
of settings, one bit for every switch and every LUT bit on the chip. A
bitstream must say what every one of them is.

Most of those bits are left at their defaults, so bitstreams are
usually **compressed**. A blinking-light design for a 25F, which uses
a few tiles, packs to about 99 KB. Zeitlos's gateware, which fills
most of the chip, is around 530 KB.

```sources
(source "docs/zfpga.md, sections 2.1 and 2.3; zfpga info LFE5U-25F")
(source "docs/zfpga-test.md, section 2; docs/boards.md, Mozart ML0 and Sergei ML0")
```

```check
(choice
  (ask "Why is a blinking-light bitstream so much smaller than 560 KB?")
  (right "It is compressed, and nearly all its bits are at their defaults")
  (wrong "A small design only sets part of the configuration memory"
    "Every bit of the configuration memory is set by every bitstream. A small design leaves most at their defaults, and compression makes those cheap.")
  (wrong "It is for a smaller FPGA"
    "The figure is for the same 25F. The size comes from compression, not from the chip.")
  (hint "What does a bitstream do with the bits a design does not use?"))
```

---

## Power-on

At power-on the ECP5 always reads from address 0 of its configuration
flash. What it finds there depends on the board.

Many boards, such as the Lakritz, keep a **DFU bootloader** at address
0: a small design that waits a few seconds for an update over USB, then
hands over to Zeitlos's gateware at address 0x040000. Boards flashed
over a JTAG cable, such as the Mozart and Sergei modules, can have
Zeitlos's gateware at address 0 itself.

```figure
(text 16 22 "Flash of a board with a bootloader" 'small 'bold)
(fp-box 16 34 60 50 "boot")
(fp-box-on 76 34 120 50 "gateware")
(fp-box 196 34 54 50 "kernel")
(fp-box 250 34 54 50 "apps")
(text 16 100 "0" 'tiny)
(text 76 100 "0x040000" 'tiny)
(text 196 100 "0x100000" 'tiny)
(text 250 100 "0x128000" 'tiny)
(arrow 46 150 46 108 'ink 1.5)
(text 16 166 "power-on reads here" 'small)
(arrow 136 182 136 108 'ink 1.5)
(text 100 198 "then the bootloader loads this" 'small)
(caption "Not to scale. Kernel and apps: software.")
```

```sources
(source "docs/zboot.md, sections 3 and 5 (the layout); docs/dfu_upgrade.md")
```

```check
(choice
  (ask "On a Lakritz with its DFU bootloader, what does the FPGA load first at power-on?")
  (right "The DFU bootloader, at address 0")
  (wrong "Zeitlos's gateware, at address 0x040000"
    "Power-on always reads address 0. On this board that is the bootloader, which then hands over to the gateware at 0x040000.")
  (wrong "The kernel"
    "The kernel is software. It needs a processor, which only exists once a bitstream has been loaded.")
  (hint "Where does power-on always read?"))
```

---

## Reloading: PROGRAMN

An ECP5 also reloads when its **PROGRAMN** pin is pulled low. On
Zeitlos's boards that pin is wired back to the FPGA, so a design inside
can pull it and make the chip load a new configuration. It is how the
bootloader hands over to Zeitlos, and how `reboot` works.

Where the reload reads from is not a setting in the chip. Each
bitstream carries a **boot address**: the flash address the *next*
configuration is read from. The bootloader's points at Zeitlos's
gateware.

A power cycle ignores all that: it reads address 0 again.

```sources
(source "docs/zboot.md, sections 1, 2 and 6")
```

```check
(choice
  (ask "A design you built hangs the machine. How do you get back to Zeitlos?")
  (right "Switch off and on again: power-on reads address 0")
  (wrong "Wait for the boot address to time out"
    "A boot address is only used when PROGRAMN is pulled, and a hung design will not pull it. A power cycle always reads address 0.")
  (wrong "Press Esc"
    "While your design runs, Zeitlos is not there to see the key. The FPGA is your circuit until it is reloaded.")
  (hint "Which way of reloading does not depend on the design?"))
```

---

## The MMOD

On many Machdyne boards the configuration flash is an **MMOD**: a
small removable flash module in a socket. Power off, swap the module,
power on, and the board loads whatever the other module holds.

That makes a spare MMOD the safest place to try a new design: the one
holding Zeitlos is never written, and putting it back brings Zeitlos
back.

```sources
(source "Timeless Computing (docs/tc.md), The Machine: Getting Back to Safety; docs/zfpga.md, section 2.9")
```

```check
(choice
  (ask "You want to try a bitstream without any risk to Zeitlos. Which is safest?")
  (right "Write it to a spare MMOD and boot from that one")
  (wrong "Write it over the start of Zeitlos's own MMOD"
    "Then Zeitlos's gateware is gone until you write it back, which needs another computer or another MMOD.")
  (wrong "Write it at address 0 of the flash you boot from"
    "Address 0 is what every power-on reads. A bad design there means no way back with a power cycle.")
  (hint "Which choice never touches the module Zeitlos is on?"))
```

So: the bitstream lives in the configuration flash, power-on reads
address 0, and PROGRAMN reloads from the running bitstream's boot
address.
