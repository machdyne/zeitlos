# The caches

```lesson
(objective "Say what the Zeitlos caches keep, which boards have them, and why no program ever has to flush them.")
(minutes 20)
(keywords "instruction cache" "data cache" "cache line" "write-through" "direct-mapped" "flush (a cache)")
```

You will learn how Zeitlos keeps its CPU fed on slow memory.

Main memory answers slowly on most boards: SDRAM takes about 5 to 11
clock cycles for each 32-bit word. SRAM, on the Obst, answers in about
one. So most SDRAM and DDR3 boards put caches between the CPU and the
bus:

- an **instruction cache**, which keeps recently fetched instructions;
- on most of them also a **data cache**, which keeps recently loaded
  data. The two are one module, `rtl/cache_id.v`.

On a `sergei_ml1` the 4 KB instruction cache alone took the CPU from
3.09 to 5.79 MIPS.

```sources
(source "Zeitlos docs: icache.md, Why this exists; Measured on hardware; dcache.md, Configuration")
```

```check
(choice
  (ask "Why does the Obst have no cache?")
  (right "Its SRAM answers in about one cycle, so a cache would have nothing to hide")
  (wrong "Its FPGA is too small for any cache"
    "Room was one concern, but the main reason is speed: with memory that answers in a cycle, a cache gains nothing. An early test even made it slower.")
  (wrong "Its programs are too small to need one"
    "Programs are the same on every board. The question is how slowly memory answers.")
  (hint "How many cycles does SRAM take for a word?"))
```

---

## What each board has

The board block chooses:

| Board | Instruction cache | Data cache |
|---|---|---|
| Obst | none | none |
| Lakritz | 4 KB | 4 KB |
| Mozart ML1, Sergei ML1 | 8 KB | 4 KB |
| Mozart ML2, Sergei ML2 | 8 KB | 4 KB |

Both caches are **direct-mapped**: each address has exactly one place in
the cache where it can be kept. They work in **cache lines** of 4 words,
16 bytes: a miss brings in the whole line around the word asked for.

```sources
(source "Zeitlos source: rtl/boards.vh, ICACHE_KB, DCACHE_KB and ICACHE_LINE_WORDS for each board")
(source "Zeitlos docs: icache.md, Design (direct-mapped)")
```

```check
(number
  (ask "How many cache lines does an 8 KB instruction cache with 16-byte lines hold?")
  (answer 512) (from "8 * 1024 / 16")
  (wrong-if 2048 "That is 8 KB in words of 4 bytes. A line is 4 words, 16 bytes.")
  (wrong-if 500 "A kilobyte here is 1,024 bytes, so 8 KB is 8,192 bytes.")
  (hint "8 KB is 8,192 bytes."))
```

---

## Only main memory

Only main memory, region 4 (and 5 on the 512 MB boards), is ever
cached. The boot RAM, VRAM and every device register go straight to the
bus: a device's register can change by itself, and a copy of it in a
cache would soon be wrong. (On some computers a program must instead
**flush** the cache, tell it to drop what it holds, before such a
read.)

The caches sit **after** the MTU, so they see real, physical addresses.
A context switch needs no work from them, and the kernel reaching an
app's memory by its physical address finds the same cache lines the
app uses.

```sources
(source "Zeitlos docs: dcache.md, Architecture (only main memory is cached; physically tagged, after the MTU)")
```

```check
(choice
  (ask "A program reads the blitter's status register in a loop until the blitter is done. Why does the cache not hide the change?")
  (right "Device registers are never cached: every read goes to the blitter")
  (wrong "The program must flush the cache before each read"
    "No program flushes the Zeitlos caches. Region D is simply never cached.")
  (wrong "The cache notices when the blitter finishes"
    "The cache never holds the register in the first place: only main memory is cached.")
  (hint "Which region is cached?"))
```

---

## Never stale

A cache that keeps old data is a classic source of bugs. On the boards
with both caches, it cannot happen:

- the data cache is **write-through**: every store goes on to memory,
  and updates the cached copy if there is one;
- the instruction side watches every store, and drops a line that the
  store changes. So loading a new program over an old one needs no
  flush;
- the only master that writes main memory is the CPU, through the cache.

So no program ever has to flush them. (The kernel still flushes the
instruction cache when it loads a program, because a board with only an
instruction cache, such as the Lebkuchen, needs it.)

```sources
(source "Zeitlos docs: dcache.md, Summary; Data side; Instruction side")
```

```check
(choice
  (ask "On a board with both caches, program B is loaded into the memory where program A ran. Why does the CPU not run A's old instructions from the cache?")
  (right "The stores that wrote B dropped A's lines from the instruction cache as they went")
  (wrong "The instruction cache is turned off while a program is loaded"
    "It stays on. The stores that write B pass the cache, and the instruction side drops every line they change.")
  (wrong "Instructions are never cached"
    "They are: the instruction cache is the one every cached board has."))
```

---

## Practice

```check
(choice
  (ask "A program stores a word to main memory. With a write-through data cache, where does the new value go?")
  (right "To main memory, and to the cache too if that line is in it")
  (wrong "Only to the cache, until the line is thrown out"
    "That would be a write-back cache. Write-through sends every store on to memory.")
  (wrong "Only to main memory, and the cached copy is left as it was"
    "Then the cache would be stale. A store that hits updates the line as well."))
```

```check
(number
  (ask "Lakritz has 4 KB of data cache in 16-byte lines. How many lines is that?")
  (answer 256) (from "4 * 1024 / 16")
  (wrong-if 1024 "That counts 4-byte words. A line is 16 bytes.")
  (hint "4,096 bytes divided by 16."))
```

So: on the slower memories, Zeitlos keeps instructions and data in small direct-mapped caches that cover main memory only and can never go stale.
