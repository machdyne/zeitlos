# System calls, drivers and names

```lesson
(objective "Say how a program asks the kernel for help, what a driver does, how files are named in one tree, and how processes pass data to each other.")
(minutes 20)
(keywords "system call" "device driver" "namespace" "path (of a file)" "root (of a tree)" "mount" "shared memory")
```

Ordinary programs are not allowed to do everything, so they ask the
kernel to act for them. A **system call** is such a request: open a
file, read some bytes, send a message, start a program, wait for
something to happen. Each operating system has its own set of system
calls, and each programming language's standard library offers them
ready to use.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: System Calls; Drivers")
```

```check
(choice
  (ask "A program wants to read a file from the SD card. What does it do?")
  (right "It makes a system call, and the kernel does the reading")
  (wrong "It writes to the SD card's registers itself"
    "An ordinary program is not allowed to reach the hardware directly; protection stops it. It asks the kernel with a system call.")
  (wrong "It switches the memory protection off first"
    "Only privileged code can change protection, so a program cannot. It asks the kernel instead.")
  (hint "Ordinary programs are not allowed to do everything. Who is?"))
```

---

## Drivers

A **device driver**, or driver, is the part of the system that operates one particular
device. It knows which registers to write, in what order, and what the
answers mean. The rest of the system uses the device through a simpler,
general interface. To a program writing a file, an SD card, a USB stick
and a disk held in RAM all look the same.

```check
(choice
  (ask "You add a new kind of storage card to a computer. What must be written so that every program can save files on it?")
  (right "One driver for the card")
  (wrong "A new version of every program"
    "Programs use storage through the general interface, so they need no change. Only the driver knows the new card.")
  (wrong "Nothing: the kernel works out new hardware by itself"
    "Someone must know which registers to write and what the answers mean. That knowledge is the driver.")
  (hint "Which part knows the details of one device?"))
```

---

## Files and names

A filesystem arranges one storage device as named files in folders. The
operating system joins all its devices into one **namespace**: the set
of names a program can use.

Unix-like systems, and many systems that learned from them, use a
single tree of names written as **paths**, such as `/home/notes.txt`.
The leading `/` is the **root** of the tree. Each device is attached,
or **mounted**, at some point in the tree, so a USB stick might appear
as `/usb`, with its files below. Other systems, such as MS-DOS and
Windows, give each device a letter instead.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Files and Names")
```

```check
(text
  (ask "A USB stick is mounted at /usb. On it, in its top folder, is the file notes.txt. What is the file's full path?")
  (accept "/usb/notes.txt")
  (wrong-if "notes.txt" "That is only the file's own name. The full path starts at the root and goes through /usb.")
  (wrong-if "/notes.txt" "That would be a file at the root itself. The stick's files are below the point where it is mounted.")
  (hint "Start with where the stick is mounted, then a /, then the name."))
```

---

## One tree for everything

The advantage of a single tree is that everything in it is reached the
same way. A program that can read files can read them from any device,
and some systems show devices, settings and network resources as files
in the tree as well.

```check
(choice
  (ask "A program reads files on a single-tree system. A new USB stick is mounted at /usb. What must change in the program to read the stick's files?")
  (right "Nothing: it gives paths that start with /usb")
  (wrong "It needs code for USB sticks"
    "The driver and the kernel deal with the stick. To the program, its files are names in the same tree.")
  (wrong "It must be told the stick's drive letter"
    "Drive letters are another system's way. In a single tree, the stick is reached by its place in the tree.")
  (hint "What does mounting do for a program that reads files?"))
```

---

## Talking between processes

Processes often work together: an editor asks the filesystem for a
file, and the program that looks after the windows tells another program
that a key was pressed.
Operating systems offer several ways:

- **messages**: small blocks of data sent from one process to another
  through the kernel;
- **pipes**: one-way streams of bytes from one process to another. Unix-like
  systems use them to chain commands, each feeding its output to the
  next;
- **shared memory**: a region both processes can read and write. It is
  fast, but they must take care that neither changes data the other is
  using.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Communication Between Processes")
```

```check
(choice
  (ask "Two processes must pass a lot of data back and forth as fast as possible. Which way is quickest, and what is its cost?")
  (right "Shared memory, but they must coordinate so neither changes data the other is using")
  (wrong "Messages, at no cost"
    "Messages go through the kernel, which copies them: safe, but slower than both processes reading the same memory.")
  (wrong "Pipes, which carry data both ways"
    "A pipe is one-way, and its bytes go through the kernel. Shared memory is quicker, at the cost of care.")
  (hint "Which way lets both processes use the same memory directly?"))
```

So: programs ask the kernel by system calls, drivers hide each device,
paths name everything in one tree, and processes talk by messages,
pipes or shared memory.
