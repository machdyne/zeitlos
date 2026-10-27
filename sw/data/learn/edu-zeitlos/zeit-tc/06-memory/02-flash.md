# ROM, EEPROM and flash

```lesson
(objective "Say how flash memory is written, why it wears out, and where each kind is used.")
(minutes 20)
(keywords "read-only memory" "ROM" "EEPROM" "flash" "erase (flash memory)" "wear levelling" "NOR flash" "NAND flash")
```

This lesson is about the memory that keeps its contents with the power
off and can still be rewritten.

A computer needs some instructions it can run as soon as it is switched
on, before anything has been loaded. Early computers kept them in
**read-only memory** (**ROM**), written once at the factory. Then came
ROMs a user could write once, then ones that could be wiped with
ultraviolet light, and then **EEPROM**, which can be wiped and
rewritten electrically one byte at a time, though slowly and a limited
number of times.

EEPROM is still common for small amounts of data, such as settings, and
keeps them for a century or more.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Read-Only Memory and Its Descendants")
```

```check
(choice
  (ask "What could EEPROM do that the first ROMs could not?")
  (right "Be rewritten, electrically, in place")
  (wrong "Keep its data with the power off"
    "ROM keeps its data with the power off too: that is why computers start from it. EEPROM's gain was being rewritable.")
  (wrong "Be written as fast as RAM"
    "EEPROM writes slowly, and only so many times. Its gain was that it can be rewritten at all, electrically.")
  (hint "The first ROMs were written once, at the factory."))
```

---

## Flash memory

Flash memory comes from EEPROM, but it is wiped clean, an **erase**, in large
blocks rather than byte by byte. That makes it much denser and cheaper,
and **flash** is now the most common non-volatile storage. Each bit is a
charge trapped on an insulated piece of silicon inside a transistor.

An erased bit reads as 1, and writing can only change 1s to 0s. To turn
a 0 back into a 1, the whole block, often 4 KB or more, must be erased
and written again. So writing a byte without erasing first leaves a 0
wherever either the old or the new byte had a 0: an AND of the two. Take the figure's steps in order.

```figure
(define st (choice 'act '("1" "2" "3" "4") "Step"))
(text 160 34 (list-ref '("1: erase the block" "2: write 0x5A" "3: write 0xA5" "4: erase the block") st) 'centre 'body 'bold)
(define vals '(255 90 0 255))
(define v (list-ref vals st))
(define b (cp-bin v 8))
(for-range 0 8
  (lambda (k)
    (let ((x (+ 36 (* k 32))) (one (string=? (substring b k (+ k 1)) "1")))
      (frame x 60 28 28 'ink 1.5)
      (text (+ x 14) 80 (if one "1" "0") 'centre 'body 'bold))))
(text 160 130 (string-append "the byte reads 0x" (cp-hex v 2)) 'centre 'big 'bold)
(text 160 170 (list-ref '("Erased: every bit is 1." "0x5A written: some 1s became 0s."
                          "0xA5 written over it: 5A AND A5 = 00." "Erased again: all 1s.") st)
      'centre 'small)
(caption "Writing changes 1s to 0s; only erasing makes 1s")
```

```check
(text
  (ask "An erased flash byte was written with 0x0F. Now 0xF0 is written to it without erasing. What does it read? Write it in hex.")
  (accept "00") (accept "0x00") (accept "0") (accept "0x0")
  (wrong-if "F0" "That would need the low four bits to go from 1 to 0, which writing can do, and the high four from 0 to 1, which only erasing can.")
  (wrong-if "FF" "Writing never turns a 0 into a 1. Only an erase does that.")
  (hint "Each bit stays 1 only if it is 1 in both 0x0F and 0xF0."))
```

---

## Wear and fading

Two more facts set how flash is used:

- **It wears out.** Each block survives a limited number of erases,
  often around 100,000 for the kind that holds programs, and far fewer
  for the densest kinds.
- **It slowly forgets.** The trapped charge leaks. Data usually lasts
  ten to twenty years, less when hot or in a worn block.

```check
(number
  (ask "A program erases and rewrites the same flash block once a minute. The block survives 100,000 erases. About how many days does it last?")
  (answer 69.4) (within 1) (from "100000 / (60 * 24)")
  (wrong-if 100000 "That is the number of erases. At one a minute, there are 60 times 24 = 1,440 a day.")
  (wrong-if 1667 "That is hours: 100,000 / 60. Divide by 24 more for days.")
  (hint "One erase a minute is 60 times 24 = 1,440 erases a day."))
```

---

## NOR and NAND

**NOR flash** can be read one byte at a time at any address, so a
program can run straight from it. A small NOR flash chip, joined by
four wires, holds the configuration of most FPGA boards.

**NAND flash** is denser and is read in pages. It is in SD cards, USB
sticks and solid-state drives. Each of these has a small controller
inside that hides the erasing, spreads writes over the whole chip to
even out the wear (**wear levelling**), and replaces bad blocks.

That controller is also a risk: if it fails, everything behind it can
be lost at once, without warning. SD cards and USB sticks are good for
moving data, not for keeping it for decades.

```sources
(source "Timeless Computing (docs/tc.md), Memory and Storage: Flash Memory")
```

```check
(choice
  (ask "Why does an SD card spread its writes over the whole chip?")
  (right "So that no block wears out long before the others")
  (wrong "So that it can be read one byte at a time"
    "Reading a byte at a time is what NOR flash offers. Spreading writes is wear levelling: it shares the erases out.")
  (wrong "So that its data lasts for a century"
    "Wear levelling does not stop charge leaking. It keeps any one block from using up its erases early.")
  (hint "What happens to a block that is erased far more often than the rest?"))
```

---

```check
(choice
  (ask "You want to keep family photos safe for thirty years. Is one USB stick a good place?")
  (right "No: flash fades over years, and its controller can fail all at once")
  (wrong "Yes: flash is non-volatile, so it keeps data for ever"
    "Non-volatile means it keeps data without power, not for ever. Flash usually lasts ten to twenty years, and the controller can fail sooner.")
  (wrong "Yes, if it is never written again, since only writing wears it"
    "Not writing it saves wear, but the trapped charge still leaks over the years.")
  (hint "Think of both ways flash loses data: slowly, and all at once."))
```

So: flash must be erased before it is written, wears with each erase,
and fades over years, so it suits moving data better than keeping it.
