# Sizes and meaning

```lesson
(objective "Convert between the units of size, bits and bytes, and explain why a byte means only what a program makes it mean.")
(minutes 15)
(keywords "kilobyte" "megabyte" "gigabyte" "kibibyte" "mebibyte" "kilobit")
```

Sizes are counted in bytes with the usual prefixes, but they have long
had two meanings. Memory comes in powers of two, so a **kilobyte**
(KB) of memory has long meant 1,024 bytes. Makers of storage, and
international standards, use 1,000. The binary sizes have names of
their own:

| Name | Decimal | Binary name | Binary |
|---|---|---|---|
| **kilobyte** (KB) | 1,000 bytes | **kibibyte** (KiB) | 1,024 bytes |
| **megabyte** (MB) | 1,000,000 bytes | **mebibyte** (MiB) | 1,048,576 bytes |
| **gigabyte** (GB) | $10^9$ bytes | gibibyte (GiB) | $2^30$ bytes |

Memory sizes, such as a computer's "32 MB" of memory, mean the binary sizes. The book
Timeless Computing uses the binary sizes for memory and storage: KB
and KiB both mean 1,024 bytes.

```sources
(source "Timeless Computing (docs/tc.md), Information: Units of Size")
```

```check
(number
  (ask "How many bytes are in 4 KiB?")
  (answer 4096) (from "4 * 1024")
  (wrong-if 4000 "That uses the decimal kilobyte. KiB always means 1,024 bytes.")
  (hint "4 x 1,024."))
```

---

## Bits and bytes

Memory chips and the speed of links (connections that carry data) are often given in bits, written
with a small b or "bit": Kb or Kbit means **kilobits**, while KB
means kilobytes. A byte is 8 bits, so a 64 Kbit memory chip
holds 8 KB, and a link of 12 Mbit/s (12 million bits) moves at most 1.5 million bytes a second.

```check
(number
  (ask "A link moves 80 Mbit a second. At most how many MB a second is that?")
  (answer 10) (unit "MB") (from "80 / 8")
  (wrong-if 80 "80 is in megabits. A byte is 8 bits, so divide by 8.")
  (wrong-if 640 "That multiplies by 8. There are fewer bytes than bits: divide.")
  (hint "8 bits make a byte."))
```

---

## Meaning comes from use

A byte holding 0x41 might be the number 65, the letter A, part of a
pixel, part of a sound, or part of an instruction for the processor.
Nothing in the byte says which. Its meaning comes only from how a
program uses it.

So the same memory can hold any kind of information, and the same
circuits can work on all of it. But a program that reads data the
wrong way, text as a number or data as instructions, does things
nobody meant. Much of the care in programming goes into keeping track
of what each piece of data means.

```sources
(source "Timeless Computing (docs/tc.md), Information: Meaning Comes From Use")
```

```check
(choice
  (ask "A byte holds 0100 0001. What is it?")
  (right "It depends on how a program uses it")
  (wrong "The letter A"
    "It is A only if a program reads it as ASCII text. The same byte can be the number 65 or part of a picture.")
  (wrong "The number 65"
    "It is 65 only if a program reads it as an unsigned number. Read as text, the same bits are the letter A.")
  (hint "Does the byte carry a label saying what it is?"))
```

---

```check
(number
  (ask "A signed byte holds 1111 1111. A program reads the same byte as unsigned. What number does it see?")
  (answer 255) (from "2^8 - 1")
  (wrong-if -1 "-1 is the signed reading. Unsigned, every place is positive: 128 + 64 + ... + 1.")
  (hint "Unsigned: add all eight places."))
```

---

```check
(number
  (ask "A memory chip holds 256 Kbit. How many KB is that?")
  (answer 32) (unit "KB") (from "256 / 8")
  (wrong-if 256 "That is in kilobits. Eight bits make a byte.")
  (wrong-if 2048 "That multiplies by 8. Bytes are fewer than bits: divide by 8.")
  (hint "Divide the bits by 8."))
```

So: KB can mean 1,000 or 1,024 bytes, a small b means bits, and a
byte's meaning comes from the program that uses it.
