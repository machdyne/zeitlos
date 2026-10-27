# Processes

```lesson
(objective "Say what the Zeitlos kernel keeps for itself, and read the kernel's list of processes.")
(minutes 20)
(keywords "microkernel" "kernel shell" "process table" "mailbox")
```

You will learn what the Zeitlos kernel is and what it keeps track of.

Zeitlos has a **microkernel** (`sw/os`): a small kernel that does only
what must be done in one privileged place. It schedules processes,
hands out memory, passes messages, routes interrupts and sets up the
MPU. It also keeps a few machine-level services: the serial console,
the configuration, the password, a small store of settings in flash and its own
command line, the **kernel shell**.

The filesystem, USB and the network are not in the kernel: they are
programs, called drivers, that the kernel starts and restarts like any
other. The whole kernel must fit in 160 KB of flash.

```sources
(source "Zeitlos docs: kernel.md, the introduction; The kernel image budget")
```

```check
(choice
  (ask "Which of these is a separate program, not part of the Zeitlos kernel?")
  (right "The filesystem")
  (wrong "The scheduler" "Scheduling is one of the few things the kernel keeps. The filesystem is the driver program fs.")
  (wrong "Message passing" "Messages pass through the kernel. The filesystem is a driver program.")
  (hint "A microkernel keeps only what must be privileged."))
```

---

## A process

To the kernel, a process is three things: a block of main memory, the
32 registers saved while it is not running, and a **mailbox** for its
messages. The kernel keeps them in its **process table**, with room
for 32 processes, numbered 0 to 31: the pid.

Process 0 is the kernel's own, which runs the kernel shell.

```sources
(source "Zeitlos docs: kernel.md, Processes; The process table")
(source "Zeitlos source: sw/os/kernel.h, Z_PROCS_MAX")
```

```check
(number
  (ask "How many processes can the Zeitlos kernel hold at once, its own included?")
  (answer 32)
  (wrong-if 31 "Pids run from 0 to 31, and 0 is a process too: the kernel's own.")
  (wrong-if 16 "The table was 16 once; it is 32 now.")
  (hint "Pids 0 to 31."))
```

---

## States

Each process shows a state; the usual ones are:

| State | Means |
|---|---|
| run | ready to use the CPU, or using it |
| blk | blocked: waiting for a message, a key, a timer |
| load | being loaded from flash or the card |
| die | ended, waiting for the kernel to clean up |

Most processes are blocked most of the time. A text editor does nothing
until a key comes.

The kernel shell's command `ps` lists every process: its pid, state,
the base of its block, its size and more.

```sources
(source "Zeitlos source: sw/os/kernel.c, k_proc_dump() (the ps command)")
```

```try
(try (app "term")
  (task "Open term from the dock and click CONSOLE. Type ps and press Enter. Each line starts with a pid, a state and base:, the start of that process's block. Look at the bases, then close the window and come back here.")
  (check (text (ask "What hex digit does every base start with?") (accept "4")
    (wrong-if "8" "8 is the app window, the address each program uses for itself. The base is where its block really is.")
    (wrong-if "0" "Region 0 is the boot RAM. Process blocks are in main memory.")
    (hint "Where is main memory in the address map?"))))
```

---

## Practice

```check
(choice
  (ask "Which state would you expect for a calendar app that is open but that nobody is using?")
  (right "blk")
  (wrong "run" "It has nothing to do until a key, a click or a timer comes, so it waits, blocked.")
  (wrong "die" "It is still open. die is for a process that has ended."))
```

```check
(choice
  (ask "Why is a Zeitlos process's base never 0x8000_0000?")
  (right "The base is the physical start of its block in main memory; 0x8000_0000 is only how the process sees it")
  (wrong "Because 0x8000_0000 is reserved for the kernel"
    "The kernel lives at the start of main memory, 0x4000_0000. 0x8000_0000 is the app window.")
  (wrong "It is, for the process that is running"
    "Every process sees itself at 0x8000_0000, but its block is in main memory. The MTU adds the base."))
```

So: the Zeitlos kernel is a microkernel that keeps up to 32 processes, each a block of memory, saved registers and a mailbox.
