# Caches and speed

```lesson
(objective "Say why a cache makes a computer faster, and work out how many instructions a processor runs each second.")
(minutes 20)
(keywords "cache (memory)" "cache hit" "cache miss" "instructions per cycle" "pipelined")
```

This lesson is about what makes one computer faster than another.

Processors have grown faster much more quickly than memory. Reading a
word from main memory can take many clock cycles, and a simple CPU
waits all that time.

A **cache** is a small, fast memory inside the processor that keeps
copies of memory used recently. When the CPU needs a word, it looks in
the cache first. Finding it there is a **cache hit**, and it is fast.
Not finding it is a **cache miss**: the word is fetched from main
memory, usually with its neighbours, and kept in the cache.

```sources
(source "Timeless Computing (docs/tc.md), The Computer: Caches")
```

```check
(choice
  (ask "On a cache miss, why does the cache fetch the word's neighbours too?")
  (right "Programs often use the next words soon, as when instructions run in order")
  (wrong "Memory can only be read in whole screens"
    "Memory can be read a word at a time. The neighbours are fetched because they are likely to be needed next.")
  (wrong "To fill the cache up as fast as possible"
    "A full cache is no use in itself. Neighbours are fetched because programs tend to use them soon after.")
  (hint "Instructions run one after another. Where is the next one?"))
```

---

## Why a small cache works

Programs use memory in two ways that can be foreseen. A place used now
is likely to be used again soon, as in a loop. And its neighbours are
likely to be used soon, as when reading through a list or taking
instructions in order. So a small cache serves most reads.

The figure works out the average time of a read. Here a hit takes 1
cycle; a miss costs the slider's number of cycles (a made-up but
typical range).

```figure
(define h (slider 'hits 0 100 90 "Hits %" 5))
(define m (slider 'miss 5 50 20 "Miss cycles" 5))
(define (avg p) (+ (* (/ p 100) 1) (* (- 1 (/ p 100)) m)))
(graph 50 20 250 150 0 100 0 50)
(graph-axes 20 10)
(text 54 14 "average cycles a read" 'tiny)
(plot avg 'ink)
(plot-point h (avg h) 'ink)
(text 175 200 "hits (%)" 'centre 'small)
(text 160 218 (string-append "average: " (fmt (avg h) 1) " cycles a read") 'centre 'body 'bold)
```

```check
(number
  (ask "90% of reads hit, taking 1 cycle; the other 10% miss, taking 20 cycles. What is the average, in cycles?")
  (answer 2.9) (within 0.01) (from "0.9 * 1 + 0.1 * 20")
  (wrong-if 10.5 "That is halfway between 1 and 20, as if hits and misses were equally common. Most reads hit.")
  (wrong-if 1 "Misses are rare but each costs 20 cycles: they still add 0.1 times 20 = 2 cycles.")
  (hint "0.9 times 1, plus 0.1 times 20."))
```

---

## Never cache a device

A cache has costs. It makes timing harder to predict. And it must never
keep copies of device registers: their values change without the CPU
knowing, such as a status bit that says a key has arrived. Systems
mark device address ranges as uncached.

```check
(choice
  (ask "What would go wrong if the keyboard's status register were cached?")
  (right "The CPU could read an old copy and never see that a key had arrived")
  (wrong "Nothing: a cache only makes reads faster"
    "For memory that only the CPU changes, yes. A device changes its own registers, and the cached copy would not change with them.")
  (wrong "The keyboard would be damaged"
    "Nothing is harmed. The trouble is that the CPU would keep reading a stale copy.")
  (hint "Who changes a status register: the CPU, or the device?"))
```

---

## Speed

A processor's speed depends on two things: how many clock cycles it
runs each second, and how much work it does in each cycle.

Clock rates are counted in megahertz (MHz): millions of cycles a
second. Work per cycle is counted in **instructions per cycle**
(IPC), or its upside-down form, cycles per instruction.

A simple processor takes several cycles for each instruction: one to
fetch, one to decode, and so on. A **pipelined** processor overlaps
them, fetching the next instruction while decoding this one, as on an
assembly line, and can come close to one instruction a cycle.

```figure
(define f (slider 'mhz 8 96 48 "MHz" 8))
(define c (slider 'cpi 1 8 4 "Cycles each" 1))
(text 16 30 "24 clock cycles, a box each:" 'small)
(for-range 0 24
  (lambda (k)
    (let ((x (+ 16 (* k 12))))
      (if (= 0 (modulo (floor (/ k c)) 2)) (rect x 46 11 24 'grey) (frame x 46 11 24 'ink 1))
      (if (= 0 (modulo k c)) (line x 40 x 76 'ink 2)))))
(text 160 106 (string-append (fmt (floor (/ 24 c)) 0) " instructions finished in 24 cycles") 'centre 'small)
(text 160 150 (string-append (fmt f 0) " MHz / " (fmt c 0) " cycles each") 'centre 'body)
(text 160 180 (string-append "= " (fmt (/ f c) 1) " million a second") 'centre 'big 'bold)
(caption "instructions run each second")
```

```check
(number
  (ask "A processor runs at 48 MHz and takes 4 cycles for each instruction. How many million instructions does it run a second?")
  (answer 12) (from "48 / 4")
  (wrong-if 192 "That multiplies. Each instruction uses 4 of the cycles, so divide: 48 / 4.")
  (wrong-if 48 "That is one instruction a cycle. At 4 cycles each, it is a quarter of that.")
  (hint "48 million cycles a second, 4 cycles for each instruction."))
```

---

```check
(number
  (ask "The same 48 MHz processor is pipelined and comes close to 1 cycle an instruction. About how many million instructions a second now?")
  (answer 48) (from "48 / 1")
  (wrong-if 12 "That was at 4 cycles each. Pipelined, it finishes nearly one instruction every cycle.")
  (hint "One instruction a cycle, 48 million cycles a second."))
```

---

## How fast is enough

A simple 32-bit processor in an FPGA, at 48 MHz and four or five cycles
an instruction, runs roughly ten million instructions a second: a
thousand or more times slower than a modern desktop computer, which
runs at billions of cycles a second and does several instructions at
once. It is still enough to edit text, play music
and control machines, if the software is written with care.

```check
(choice
  (ask "Which of these makes a processor run more instructions each second, at the same clock rate?")
  (several)
  (right "Pipelining, so that instructions overlap")
  (right "A cache, so that it waits less for memory")
  (wrong "A larger memory"
    "More memory holds more, but each read is no quicker. Speed at a given clock comes from fewer cycles an instruction.")
  (hint "At a fixed clock rate, what matters is cycles per instruction."))
```

So: a cache hides slow memory because programs reuse places and their
neighbours, and speed is the clock rate divided by the cycles each
instruction takes.
