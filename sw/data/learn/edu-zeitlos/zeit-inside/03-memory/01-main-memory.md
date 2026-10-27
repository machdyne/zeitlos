# Main memory

```lesson
(objective "Say how the kernel shares out main memory, and work out the block a program is given.")
(minutes 20)
(keywords "memory pool" "block (of memory)" "heap" "image (of a program)" "arena" "RAM disk")
```

You will learn how main memory is divided between the programs.

Main memory starts at `0x4000_0000`. The BIOS copies the kernel there,
so the kernel's first instruction is at that address. All of main memory
is one **memory pool** that the kernel hands out, sized at start-up from
the MEM_MB register of the CSRs.

Every running program gets one **block**: a single stretch of the pool,
holding its code and data, and above them its stack and its **heap**,
the memory it asks for as it runs. The two grow toward each other. The kernel finds a block first-fit: the first free
stretch big enough.

```sources
(source "Zeitlos docs: kernel.md, Memory; Processes (a process is a contiguous block of main memory); Memory for a process")
```

```check
(choice
  (ask "Where does a Zeitlos program's heap come from?")
  (right "From its own block, in the space it shares with its stack")
  (wrong "From a separate heap the kernel keeps for every program"
    "There is no separate heap. The heap grows up inside the program's block, toward its stack.")
  (wrong "From VRAM"
    "VRAM holds only the screen's pixels. Program memory is main memory.")
  (hint "A program has one block."))
```

---

## How big a block is

A block is the program's **image**, its code and data, plus the
stack-and-heap allowance its
file asks for (16 KB if it does not ask). Then the kernel rounds it:

- up to a multiple of **4,096** bytes, so every block starts on a 4 KB
  boundary;
- and to at least **32,768** bytes, the smallest block there is.

```sources
(source "Zeitlos docs: boot.md, Memory budget (max(align_up(image + stack, 4096), 32768)); kernel.md, Memory (Z_MEM_ALIGNMENT, Z_MEM_MIN_BLOCK_SIZE)")
```

```example
(example
  (problem "A program's image is 100,000 bytes, and it asks for 16 KB (16,384 bytes) of stack and heap. How big is its block?")
  (step "Add them: 100,000 + 16,384 = 116,384 bytes.")
  (step "116,384 / 4,096 is about 28.4, so round up to 29 x 4,096 = 118,784.")
  (step "118,784 is more than 32,768, so that is the block.")
  (answer "118,784 bytes."))
```

```check
(number
  (ask "A small program: image 10,000 bytes, stack and heap 8,192 bytes. How big is its block, in bytes?")
  (answer 32768) (true "10000 + 8192 < 32768")
  (wrong-if 18192 "That is the sum before rounding. Blocks are rounded up, and none is smaller than 32,768.")
  (wrong-if 20480 "That is rounded to 4,096, right so far, but no block is smaller than 32,768 bytes.")
  (hint "Is the sum, rounded up to 4,096, at least 32,768?"))
```

---

## Arenas

A program's block is fixed when it starts, and must fit the 1 MB board
as well as the 512 MB one. A program that is only sometimes big, such as
an editor with a long document, can also have one **arena**: an extra
block it can grow and shrink while it runs. Arenas are taken from the
top of the pool, program blocks from the bottom, so the two do not mix.
The kernel never lets arenas take the memory needed to open the largest
core app, so on a 1 MB board there are none.

```sources
(source "Zeitlos docs: kernel.md, Arenas")
```

```check
(choice
  (ask "Why can a program not simply ask for a huge block at start, just in case?")
  (right "The same program must also start on a board with 1 MB, where a huge block does not fit")
  (wrong "Because blocks cannot be bigger than 32,768 bytes"
    "32,768 is the smallest block, not the largest. The limit is what the board has.")
  (wrong "Because the MTU only covers 64 KB"
    "The app window is a whole region, 256 MB. The limit is the memory on the board."))
```

---

## The RAM disk

The kernel also keeps a share of the pool as the **RAM disk**, the
folders `/ram` and `/tmp`: fast, and lost at power off. Its size is an
eighth of main memory, but never more than 4 MB, and a 1 MB board has
none.

```sources
(source "Zeitlos docs: kernel.md, Arenas (Z_RAMDISK_DIVISOR); ddr3.md (4 MB at 256 MB and at 512 MB); fs_driver.md, What fs serves")
```

```check
(number
  (ask "How big is the RAM disk on a board with 32 MB of main memory, in MB?")
  (answer 4) (from "32 / 8")
  (wrong-if 32 "The RAM disk takes a share of memory, an eighth, not all of it.")
  (hint "An eighth of 32, and no more than 4."))
```

---

```check
(number
  (ask "And on a Mozart ML2, with 512 MB, how big is the RAM disk in MB?")
  (answer 4) (true "512 / 8 > 4")
  (wrong-if 64 "An eighth of 512 is 64, but the RAM disk is capped at 4 MB.")
  (hint "There is a cap."))
```

```check
(number
  (ask "Image 60,000 bytes, stack and heap 16,384 bytes. How big is the block, in bytes?")
  (answer 77824) (from "19 * 4096")
  (true "18 * 4096 < 60000 + 16384")
  (wrong-if 76384 "That is the sum. Round it up to a multiple of 4,096.")
  (hint "76,384 / 4,096 is about 18.6."))
```

So: main memory is one pool at `0x4000_0000`, handed out in blocks that are multiples of 4 KB, at least 32 KB each.
