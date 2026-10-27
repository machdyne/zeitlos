# Protecting memory

```lesson
(objective "Say how an operating system keeps one process from damaging another: regions, address translation, pages and privilege levels.")
(minutes 20)
(keywords "memory protection unit" "MPU" "region (of memory)" "fault (of a process)" "virtual address" "physical address" "address translation" "memory management unit" "MMU" "page table" "virtual memory" "privilege level" "privileged code")
```

The kernel decides where in memory each process goes, and gives more
memory to a process that asks for it. It must also keep one process
from damaging another. A program with a bug might write through a bad
address into another program's memory, or into the kernel.

With **no protection**, every program can read and write all of
memory. Small and early systems worked this way. It is simple and
fast, but one faulty program can spoil everything.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Protecting Memory")
```

```check
(choice
  (ask "On a system with no memory protection, one program writes through a bad address into the kernel. What can happen?")
  (right "The whole machine can stop")
  (wrong "Only that program stops"
    "Nothing stops the bad write, so it changes the kernel itself. Every program depends on the kernel.")
  (wrong "The processor refuses the write"
    "Without protection hardware, there is nothing to refuse it: every program may write anywhere.")
  (hint "With no protection, what can a program write to?"))
```

---

## Regions

A **memory protection unit** (**MPU**) checks every access against a
few **regions**. A region is a range of addresses with permissions:
may the running process read it, write it, run instructions from it?
At each context switch, the kernel loads the next process's regions.

An access outside them is stopped before it takes effect, and the
processor reports it to the kernel as a **fault**. The kernel can then
stop the faulty process, say what it did, and let the others go on. An
MPU does not change addresses; it is small and fast, and common in
microcontrollers.

```check
(choice
  (ask "With an MPU, a process tries to write outside its regions. What happens?")
  (right "The write is stopped, and the kernel hears of it as a fault")
  (wrong "The write goes ahead, and the kernel is told afterwards"
    "The MPU stops the access before it takes effect. That is what protects the other processes.")
  (wrong "The whole machine stops"
    "That is what happens with no protection. With an MPU, the kernel can stop just the faulty process.")
  (hint "Is the bad access stopped before or after it changes memory?"))
```

---

## Address translation

**Address translation** changes the address a program uses, its
**virtual address**, into a different **physical address** in memory.
The simplest kind adds an offset: the hardware adds the running
process's own offset to every address in a window. Every program can
then be built to run at the same virtual addresses, wherever the
kernel puts it.

Here processes A and B both use virtual addresses 0 to 9,999. Move the
slider.

```figure
(define v (slider 'v 0 9500 2000 "Virtual address" 500))
(define (py a) (+ 30 (* a 0.0022)))
(frame 200 30 60 176 'ink 1)
(rect 201 (py 0) 58 (* 10000 0.0022) 'grey)
(wo-box 200 (py 30000) 60 (* 10000 0.0022) "A" 'small 'bold)
(wo-box 200 (py 60000) 60 (* 10000 0.0022) "B" 'small 'bold)
(text 230 24 "physical" 'centre 'tiny)
(text 266 (+ (py 0) 14) "kernel" 'tiny)
(text 266 (py 30000) "30000" 'tiny)
(text 266 (py 60000) "60000" 'tiny)
(text 266 (py 80000) "80000" 'tiny)
(wo-box 30 60 60 40 "A" 'small 'bold)
(wo-box 30 140 60 40 "B" 'small 'bold)
(define ya (+ 60 (* v 0.004)))
(define yb (+ 140 (* v 0.004)))
(dot 90 ya 'ink)
(dot 90 yb 'ink)
(arrow 90 ya 200 (py (+ v 30000)) 'ink 1)
(arrow 90 yb 200 (py (+ v 60000)) 'ink 1)
(text 26 (+ ya 4) (fmt v 0) 'right 'tiny)
(text 26 (+ yb 4) (fmt v 0) 'right 'tiny)
(text 60 114 (string-append "to " (fmt (+ v 30000) 0)) 'centre 'small)
(text 60 194 (string-append "to " (fmt (+ v 60000) 0)) 'centre 'small)
(caption "The same virtual address, two physical ones")
```

```check
(number
  (ask "Process A's offset is 30,000. It uses virtual address 2,500. Which physical address does it reach?")
  (answer 32500) (from "30000 + 2500")
  (wrong-if 2500 "2,500 is the virtual address, the one the program uses. The hardware adds A's offset to it.")
  (wrong-if 27500 "The offset is added, not taken away.")
  (hint "Add the offset to the virtual address."))
```

---

## Pages and virtual memory

A **memory management unit** (**MMU**) translates in finer detail. It
divides memory into **pages**, typically of 4 KiB (4,096 bytes), and looks up each
virtual page in a **page table** that the kernel keeps. The table says
where the page is in physical memory and what may be done with it.

Each process has its own table, and so its own complete view of
memory: **virtual memory**. Pages can be shared between processes,
made only when first used, or moved to disk when memory runs short.
Linux in its usual form needs an MMU.

```check
(choice
  (ask "Process A hands process B the address of some of its data. Why can B not simply read the data at that address?")
  (right "In B, the same virtual address means different memory")
  (wrong "B is not allowed to read numbers"
    "B can read its own memory freely. The trouble is translation: A's virtual address leads somewhere else in B.")
  (wrong "The address is too large for B"
    "Both use the same range of virtual addresses. Each process's translation sends them to different physical memory.")
  (hint "Look at the figure: one virtual address, two processes."))
```

---

## Privilege

Protection also needs **privilege levels**. The kernel runs at a higher
privilege than ordinary programs, and only **privileged code**, code running at that higher level,
can change the protection settings. Otherwise a program could simply switch the
protection off.

```check
(choice
  (ask "Why may only privileged code change the MPU's regions?")
  (right "Otherwise any program could give itself access to everything")
  (wrong "Because changing them is slow"
    "It is a matter of safety, not speed. A program that could set its own regions could reach any memory it liked.")
  (wrong "Because the regions are kept on disk"
    "The regions are loaded into the MPU at each context switch. Only privileged code may load them, so programs cannot widen their own.")
  (hint "What would a buggy or hostile program do if it could change them?"))
```

So: an MPU fences each process into its regions, translation gives
each its own addresses, and privilege keeps programs from undoing it.
