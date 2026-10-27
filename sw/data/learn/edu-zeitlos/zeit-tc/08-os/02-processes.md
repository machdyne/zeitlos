# Processes and scheduling

```lesson
(objective "Say what a process is, which state it is in, and how a scheduler shares one processor among many processes.")
(minutes 20)
(keywords "process" "process ID" "ready (process)" "blocked (process)" "scheduler" "bug" "cooperative multitasking" "preemptive multitasking" "context switch" "time slice" "throughput" "responsiveness")
```

A program on a disk is a file. A program that is running is a
**process**: its code and data in memory, its stack, the contents of
its registers, and what it is using, such as open files. The kernel
keeps a table of processes, each known by a number, its **process ID**.

At any moment, each process is in one of a few states:

- **running**: on the processor now (with one processor, only one
  process is running);
- **ready**: able to run, and waiting for its turn;
- **blocked**: waiting for something, such as a key press, data from a
  disk or a message, and not using the processor.

```figure
(wo-box 20 40 90 30 "ready" 'body 'bold)
(wo-box 210 40 90 30 "running" 'body 'bold)
(wo-box 115 160 90 30 "blocked" 'body 'bold)
(arrow 110 48 210 48 'ink 1)
(text 160 36 "chosen to run" 'centre 'tiny)
(arrow 210 62 110 62 'ink 1)
(text 160 80 "time slice used up" 'centre 'tiny)
(arrow 255 70 205 165 'ink 1)
(text 276 120 "waits for a key," 'centre 'tiny)
(text 276 132 "a file, a message" 'centre 'tiny)
(arrow 115 165 65 70 'ink 1)
(text 52 120 "what it waited" 'centre 'tiny)
(text 52 132 "for has happened" 'centre 'tiny)
(caption "The states of a process")
```

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Processes")
```

```check
(choice
  (ask "A text editor waits for you to press a key. Which state is its process in?")
  (right "Blocked")
  (wrong "Running"
    "Running means using the processor now. While it waits for a key, the editor has nothing to do and does not use the processor.")
  (wrong "Ready"
    "Ready means it could run if given the processor. It cannot do anything until the key comes: it is blocked.")
  (hint "Could it do anything with the processor before the key arrives?"))
```

---

## Mostly waiting

Most processes spend most of their time blocked: an editor does
nothing until a key is pressed. That is why one slow processor can
serve many programs; only a few need it at any moment.

When the key arrives, the editor's process becomes ready, and then
running when it gets its turn.

```check
(choice
  (ask "You press a key. The editor's process was blocked waiting for it. Where does it go next?")
  (right "To ready, until it is chosen to run")
  (wrong "Straight back to blocked"
    "What it waited for has happened, so it is no longer blocked. It becomes ready to run.")
  (wrong "It ends"
    "A process ends only when its program finishes. Getting a key makes it ready to handle the key.")
  (hint "Follow the arrow out of blocked in the figure."))
```

---

## Taking turns

The part of the kernel that decides which ready process runs next is
the **scheduler**.

In **cooperative multitasking**, each program runs until it gives up
the processor of its own accord. That is simple, but one program that
never gives it up, through a **bug** (a mistake in a program) or poor
design, stops everything.

In **preemptive multitasking**, a timer interrupt takes the processor
away from the running program at regular times. The kernel saves that
program's registers, chooses another ready process, loads its
registers and lets it run: a **context switch**. Each process runs for
a short **time slice**, typically a few milliseconds.

Here process B has a bug and never gives up the processor. Tick the box
to switch to preemptive.

```figure
(define pre (toggle 'pre "Preemptive"))
(define names '("A" "B" "C"))
(for-range 0 3
  (lambda (r)
    (text 30 (+ 66 (* 40 r)) (list-ref names r) 'body 'bold 'right)
    (line 40 (+ 76 (* 40 r)) 290 (+ 76 (* 40 r)) 'grey 0.5)))
(for-range 0 12
  (lambda (i)
    (let ((p (if pre (modulo i 3) (if (< i 2) 0 1))))
      (rect (+ 50 (* 20 i) 1) (+ 50 (* 40 p)) 18 24 'warm))))
(arrow 50 190 290 190 'ink 1)
(text 170 204 "time slices" 'centre 'tiny)
(caption (if pre "The timer takes the processor from B: all three run."
                 "B never gives the processor up: A and C wait for ever."))
```

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Scheduling")
```

```check
(choice
  (ask "Under cooperative multitasking, one program enters a loop that never ends. What happens?")
  (right "The other programs never run again")
  (wrong "The scheduler takes the processor away after its time slice"
    "That is preemptive multitasking, driven by a timer interrupt. Under cooperative multitasking only the program itself can give the processor up.")
  (wrong "Only that program stops"
    "It does not stop; it keeps the processor. Since nothing takes it away, every other program waits.")
  (hint "Under cooperative multitasking, who decides when a program gives up the processor?"))
```

---

## Fast enough to seem at once

Context switches happen hundreds of times a second, so every program
seems to run all the time.

```check
(number
  (ask "Each time slice is 5 milliseconds. How many time slices are there in one second?")
  (answer 200) (from "1000 / 5")
  (wrong-if 5 "5 is the length of one slice. A second is 1,000 milliseconds.")
  (hint "1 second is 1,000 milliseconds."))
```

---

## What to aim for

A scheduler can aim for **throughput**, the most work done in all, or
for **responsiveness**, a quick reaction when something happens. For a
computer a person uses, responsiveness matters most. A good scheduler
runs a process as soon as what it was waiting for arrives, so a key
press is handled at once, even on a slow processor.

```check
(choice
  (ask "A long calculation is running when you press a key in the editor. A scheduler built for responsiveness does what?")
  (right "Runs the editor straight away, then lets the calculation carry on")
  (wrong "Lets the calculation finish its whole job first"
    "That would get the most work done, but you would wait. Responsiveness means handling the key as soon as it arrives.")
  (hint "Responsiveness is about a quick reaction to events."))
```

So: a process is running, ready or blocked, and the scheduler shares
the processor among the ready ones, taking turns many times a second.
