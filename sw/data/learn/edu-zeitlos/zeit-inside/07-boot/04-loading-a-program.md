# Loading a program

```lesson
(objective "Read the header of a Zeitlos program file, and say how the kernel loads and starts it.")
(minutes 20)
(keywords "ZEXE" "stack size code")
```

You will learn what is inside a Zeitlos program file and how it comes
to run.

Every Zeitlos program file built today begins with a 16-byte header,
the **ZEXE** header:

| Offset | Bytes | Holds |
|---|---|---|
| 0 | 4 | the letters ZEXE |
| 4 | 2 | the version, now 1 |
| 6 | 2 | flags: stack size, compressed, driver |
| 8 | 4 | how many bytes to clear after the image (.bss) |
| 12 | 4 | 0; or, when compressed, the image's unpacked size |

The program's image follows from offset 16. Its zeros are not stored:
the header says how many there are, and the loader clears them, which
is much faster than reading them from the card.

```sources
(source "Zeitlos docs: executables.md, What it replaces; Layout")
```

```try
(try (app "hex")
  (task "Open hex from the dock (it is in the Tools group). Open a program from the apps folder, such as info, with the open button or Ctrl+O. Look at the first four bytes, then close hex and come back here.")
  (check (choice (ask "What are the first four bytes, in hex?")
    (right "5A 45 58 45")
    (wrong "45 58 45 5A" "The letters are stored in the order they are read, Z first. Only numbers are stored lowest byte first.")
    (wrong "7F 45 4C 46" "That is the start of an ELF file, the format programs are built in on the build machine. Zeitlos files are ZEXE.")
    (hint "Z is 0x5A, E is 0x45, X is 0x58."))))
```

---

## The flags

Bits 3 to 0 of the flags word are the **stack size code**: how much
stack and heap the program wants on top of its image. Code 0 means the
default, 16 KB. Otherwise the size is

$$ 8 * 2^(c - 1) $$

KB for code c, up to code 11, 8 MB, the most the kernel grants. Bit 8
says the image is compressed (LZ4); bit 15 is the driver flag.

The flags are a 16-bit number stored little-endian, so the low byte
comes first in the file.

```sources
(source "Zeitlos docs: executables.md, The stack size; Layout")
```

```check
(number
  (ask "A program's stack size code is 3. How many KB of stack and heap does it ask for?")
  (answer 32) (from "8 * 2^(3 - 1)")
  (wrong-if 24 "That is 8 x 3. Each code is twice the one before: 8, 16, 32...")
  (wrong-if 64 "That is code 4. Code 1 is 8 KB, code 2 16 KB, code 3 32 KB.")
  (hint "Code 1 is 8 KB, and each code doubles it."))
```

---

```check
(choice
  (ask "The driver flag, bit 15, is set and nothing else in the flags. Which two bytes are at offsets 6 and 7?")
  (right "00 80")
  (wrong "80 00" "That is the number 0x8000 as written. Little-endian puts the low byte, 00, first.")
  (wrong "00 0F" "Bit 15 is worth 0x8000, not 15.")
  (hint "Bit 15 alone is 0x8000."))
```

---

## How the kernel loads it

When you open a program from the dock:

1. The kernel finds the file, on the card first, then in flash, and
   reads its header.
2. It allocates one block: the image, the cleared bytes and the
   stack and heap. If the pool cannot hold it, the program is refused
   with a line saying what it asked for and what there is.
3. The new process then **loads itself** in its own turns, so the
   desktop never waits: it reads (and unpacks) the image into its block,
   clears the rest, and jumps to `0x8000_0000`.

Every program is built to start at `0x8000_0000`, so loading is a plain
copy.

```sources
(source "Zeitlos docs: kernel.md, Launching in the background; executables.md, The stack size (refusals)")
```

```check
(choice
  (ask "A program asks for code 15. What does the kernel do?")
  (right "Refuses to start it, with a line saying the code is reserved")
  (wrong "Starts it with the default 16 KB"
    "The kernel never grants something other than what was asked for. A code it does not understand is refused.")
  (wrong "Starts it with 8 MB, the most it grants"
    "Granting less than was asked would only make the program fail later, somewhere less clear. It is refused at once."))
```

---

## Practice

```check
(number
  (ask "What is the largest stack and heap the kernel grants, in MB? (Code 11.)")
  (answer 8) (from "8 * 2^(11 - 1) / 1024")
  (wrong-if 16 "That would be code 12, which the kernel refuses: its cap is code 11.")
  (hint "8 KB x 2 to the power 10."))
```

```check
(choice
  (ask "Why does the ZEXE header store the number of zero bytes, rather than the zeros themselves?")
  (right "Clearing memory is far faster than reading zeros from the card")
  (wrong "Because the zeros would not fit in flash"
    "They would fit, but reading them was slow: for one program nearly half its load time was zeros.")
  (wrong "Because the kernel cannot read zeros"
    "It can. It is a matter of speed."))
```

So: a Zeitlos program starts with a 16-byte ZEXE header, and the kernel makes a block for it that the new process fills itself before jumping to `0x8000_0000`.
