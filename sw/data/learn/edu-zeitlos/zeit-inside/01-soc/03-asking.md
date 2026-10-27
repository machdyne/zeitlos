# Asking the hardware

```lesson
(objective "Read the CSRs to find out how much memory a board has and which features its gateware was built with.")
(minutes 20)
(keywords "CSRs" "feature bit")
```

You will learn how software finds out what a board has.

Software cannot simply try a device to see if it is there. On this bus,
reading a register that was never built does not cause an error. If no
part answers its address, the CPU waits for ever (the last lesson); if
another part answers the region, it returns a value that proves
nothing. So the SOC has one small
part that every board builds, whose only job is to answer questions:
the **CSRs**, the capability registers (`rtl/csrs.v`). They are
read-only words at the start of region 7:

| Address | Name | Holds |
|---|---|---|
| 0x7000_0000 | MAGIC | always 0x5A45_4954 |
| 0x7000_0004 | MEM_MB | main memory, in megabytes |
| 0x7000_0008 | FEATURES | one bit for each feature built |
| 0x7000_000C | FEATURES2 | more of them |

```sources
(source "Zeitlos docs: csrs.md, Overview; Why this needed a real, positive present signal; Register map")
```

```check
(choice
  (ask "Why do the CSRs exist, when software could read a device's own registers to see if it is there?")
  (right "Reading a device that was never built gives no error: it can hang the CPU, or give a value that proves nothing")
  (wrong "Reading a missing device would crash the kernel"
    "On this bus a read from a device that is not there raises no fault for the kernel to see. That is exactly the problem: the CPU just waits, or gets a value that looks like an answer.")
  (wrong "Device registers can only be written, not read"
    "Most device registers can be read. The trouble is telling a real value from whatever the bus returns when nothing is there.")
  (hint "What happens on a read from an unbuilt register?"))
```

---

## The magic word

MAGIC is checked first. Its value, `0x5A45_4954`, is the four letters
"ZEIT" in ASCII: `5A` is Z, `45` E, `49` I and `54` T. If MAGIC reads
anything else, the gateware is older than the CSRs, and the honest
answer to "does this board have X?" is **unknown**, not no.

MEM_MB holds a plain number. A Mozart ML2 reads 512 there.

```sources
(source "Zeitlos docs: csrs.md, Register map; Software access (z_soc_has_feature and z_soc_feature_confirmed_absent)")
```

```check
(text
  (ask "MEM_MB on a Mozart ML2 is 512. What is that in hex?")
  (accept "200") (accept "0x200") (accept "0x0200") (accept "0x00000200") (accept "00000200") (accept "0x0000_0200")
  (wrong-if "512" "That is the decimal value. In hex, 512 is 2 sixteen-squareds: 2 x 256.")
  (wrong-if "2000" "That is 8,192. 512 is 2 x 256, and 256 is 0x100.")
  (hint "256 is 0x100.")
  (hint "512 is 2 x 256."))
```

---

## Feature bits

Each **feature bit** of FEATURES says whether one optional part was
built into this gateware: 1 if it was, 0 if not. A few of them:

| Bit | Feature |
|---|---|
| 0 | SRAM main memory |
| 1 | SDRAM main memory |
| 2 | VRAM |
| 7 | the line rasterizer |
| 8 | the blitter |
| 12 | the serial port UART0 |

The `info` app reads FEATURES and FEATURES2 and shows a tag for each bit
that is set: these are the tags you saw in the last lesson.

```sources
(source "Zeitlos docs: csrs.md, Feature bits; info_app.md, Feature tags")
```

```check
(text
  (ask "A gateware has only bits 1 and 2 of FEATURES set. What is FEATURES, in hex?")
  (accept "6") (accept "0x6") (accept "0x00000006") (accept "00000006") (accept "0x0000_0006")
  (wrong-if "3" "That would be bits 0 and 1. Bit 1 is worth 2 and bit 2 is worth 4.")
  (wrong-if "12" "12 is the two bit numbers side by side. Add the bits' values: 2 + 4.")
  (hint "Bit 1 is worth 2, bit 2 is worth 4."))
```

---

## In memory, byte by byte

The CPU is little-endian: a word's lowest byte is at the lowest
address. So MAGIC, `0x5A45_4954`, is stored as the bytes `54 49 45 5A`
from `0x7000_0000` up.

You can look. The console in `term` is the kernel's own shell, and its
command `hd` (hex dump) shows 256 bytes of memory from an address.

```sources
(source "Zeitlos docs: console.md (CONSOLE in term); tc.md, Memory and Addresses (little-endian)")
(source "Zeitlos source: sw/os/sh.c, hd and hex_dump()")
```

```try
(try (app "term")
  (task "Open term from the dock and click CONSOLE. At the prompt, type hd 70000000 and press Enter. Look at the first line, then close the term window and come back here.")
  (check (text (ask "What is the first byte of the dump, in hex?") (accept "54") (accept "0x54")
    (wrong-if "5A" "5A is the first byte of the word as it is written, 0x5A45_4954. In memory the lowest byte comes first.")
    (hint "Little-endian: the lowest byte, the last two hex digits of 0x5A45_4954, is at the lowest address."))))
```

---

## Practice

```check
(choice
  (ask "On an old gateware, MAGIC reads 0. What should software conclude about the blitter?")
  (right "It does not know whether there is a blitter")
  (wrong "There is no blitter"
    "Only the CSRs can say no, and they are not there. Without them the honest answer is unknown.")
  (wrong "There is a blitter"
    "Nothing has said so. The feature bits cannot be trusted when MAGIC is wrong."))
```

```check
(text
  (ask "Only bit 8 of FEATURES is set. What is FEATURES, in hex?")
  (accept "100") (accept "0x100") (accept "0x00000100") (accept "00000100") (accept "0x0000_0100")
  (wrong-if "8" "8 is bit 3. Bit 8 is worth 2 to the power 8, which is 256.")
  (wrong-if "256" "256 is right in decimal. Write it in hex: 256 is 16 x 16.")
  (hint "Bit 8 is worth 256, which is 16 x 16."))
```

```check
(choice
  (ask "A 32 MB board's MEM_MB is 32, which is 0x20. Which bytes does hd show at 0x7000_0004?")
  (right "20 00 00 00")
  (wrong "00 00 00 20" "That is the word as it is written. In memory the lowest byte, 20, comes first.")
  (wrong "32 00 00 00" "32 is the decimal value. hd shows bytes in hex, and 32 is 0x20."))
```

So: the CSRs at `0x7000_0000` say what this gateware has, and MAGIC tells software whether it can trust them.
