# The flash layout

```lesson
(objective "Find the kernel, the core apps and the key/value store in the Zeitlos flash region, as offsets and as flash addresses.")
(minutes 15)
(keywords "Zeitlos region" "core apps" "key/value store")
```

You will learn what Zeitlos keeps in flash, and where.

Zeitlos uses exactly 1 MB of the flash chip on every board: the
**Zeitlos region**. Everything outside it belongs to the gateware, or to
whatever else the board keeps there. Inside, the layout is fixed:

| Offset | Size | Holds |
|---|---|---|
| +0x000000 | 160 KB | the kernel |
| +0x028000 | 848 KB | the core apps |
| +0x0FC000 | 4 KB | a block kept for testing the flash |
| +0x0FE000 | 8 KB | the key/value store |


The **core apps** are the programs the machine needs with no card: the
drivers, `wm`, `term` and a few more. The **key/value store** keeps the
few settings that must live without a card, such as the password.

```sources
(source "Zeitlos docs: boot.md, The flash layout; kernel.md, Writing the flash")
```

```check
(text
  (ask "160 KB is 163,840 bytes. Write it in hex.")
  (accept "28000") (accept "0x28000") (accept "0x028000") (accept "0x0002_8000") (accept "0x00028000")
  (wrong-if "0x160" "160 is the size in KB, in decimal. In bytes it is 163,840, which is 0x28000.")
  (wrong-if "0x40000" "That is 256 KB, what the kernel's space used to be. 160 KB is 0x28000.")
  (hint "1 KB is 0x400, and 160 is 0xA0.")
  (hint "0xA0 x 0x400 = 0x28000."))
```

---

## Where the region is

The region's offset in the chip is its base: `0x100000` on the ECP5
boards and `0x300000` on the Artix-7 boards. The gateware reports it in
the last word of the CSRs, `0x7000_00FC`; 0 there means the usual
`0x100000`. That is how **one kernel** finds its apps on every board:
nothing in it holds a flash address, only offsets into the region.

```figure
(define b (choice 'fpga '("ECP5" "Artix-7") "Board"))
(define base (if (= b 0) 1048576 3145728))
(define (y-of off) (+ 20 (* 180 (/ off 1048576))))
(rect 120 (y-of 0) 80 (- (y-of 163840) (y-of 0)) 'grey)
(frame 120 20 80 180 'ink 1)
(line 120 (y-of 163840) 200 (y-of 163840) 'ink 1)
(rect 120 (y-of 1032192) 80 (- (y-of 1048576) (y-of 1032192)) 'ink)
(text 210 (+ (y-of 0) 14) "kernel" 'small)
(text 210 (+ (y-of 163840) 40) "core apps" 'small)
(text 210 (y-of 1040000) "test, key/value" 'small)
(text 112 (+ (y-of 0) 5) (zi-addr base) 'small 'right)
(text 112 (+ (y-of 163840) 5) (zi-addr (+ base 163840)) 'small 'right)
(text 112 (+ (y-of 1040384) 5) (zi-addr (+ base 1040384)) 'small 'right)
(caption "flash addresses of the 1 MB Zeitlos region")
```
On a 2 MB flash chip with base `0x100000`, the region is the chip's top
megabyte, so the key/value store is its last 8 KB.

```sources
(source "Zeitlos docs: boot.md, The flash layout; How software finds it; csrs.md, FLASH_BASE (word 63)")
```

```check
(text
  (ask "On an ECP5 board, at which flash address does the key/value store begin?")
  (accept "0x1FE000") (accept "1FE000") (accept "0x01FE000") (accept "0x001FE000") (accept "0x001F_E000")
  (wrong-if "0xFE000" "That is the offset inside the region. Add the base, 0x100000.")
  (hint "0x100000 + 0x0FE000."))
```

---

## Protected

Zeitlos can write the flash, to update itself, but the flash controller
refuses in hardware any write below `0x040000` on the ECP5 boards,
where the DFU bootloader is. On the Artix-7 boards the lock reaches
`0x220000`, over the whole gateware. And no program may write the
key/value store except the kernel.

```sources
(source "Zeitlos docs: kernel.md, Writing the flash")
```

```check
(choice
  (ask "On an ECP5 board with the DFU bootloader, a program goes wrong while rewriting the core apps. What still starts at the next power on?")
  (right "The DFU bootloader, which no Zeitlos program can overwrite")
  (wrong "Nothing at all"
    "Writes below 0x040000 are refused in hardware, so the bootloader is always there to take a new system.")
  (wrong "The core apps, because the flash cannot really be written"
    "The flash can be written, which is why a bad write can damage the core apps. The bootloader is what is protected."))
```

---

```check
(number
  (ask "How many KB of the region are the core apps' space? (From +0x028000 up to +0x0FC000.)")
  (answer 848) (from "(1032192 - 163840) / 1024")
  (wrong-if 864 "That runs to the end of the region. The test sector and the key/value store, 12 KB at the top, are not the apps'.")
  (hint "1,024 KB in all, minus 160 KB for the kernel, minus 4 KB and 8 KB at the top."))
```

So: Zeitlos keeps its kernel, core apps and settings in one fixed 1 MB region of flash, found by its base, with the bootloader below it out of reach.
