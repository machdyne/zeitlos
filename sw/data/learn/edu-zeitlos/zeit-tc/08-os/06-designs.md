# Kernels and interfaces

```lesson
(objective "Compare a monolithic kernel with a microkernel, read a command in a shell, and say why a small system is worth having.")
(minutes 20)
(keywords "monolithic kernel" "microkernel" "command shell" "terminal (everyday word)" "prompt (of a shell)" "argument (of a command)" "REPL" "interpreter" "graphical interface" "window manager")
```

The biggest choice in designing an operating system is how much goes
inside the kernel.

A **monolithic kernel** holds the scheduler, memory management,
filesystems, drivers and networking in one privileged program. Linux is
monolithic. Every part can call every other directly, which is fast,
but a bug in any driver can crash the whole system.

A **microkernel** holds only the essentials: scheduling, memory,
messages and interrupts. Filesystems, drivers and networking run as
ordinary processes, which talk to each other and to applications by
messages. A driver that crashes can be started again like any other
program, and each part is small enough to understand on its own. The
cost is the time spent passing messages, which careful design keeps
small. Zeitlos is a microkernel.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Kernel Designs")
```

```check
(choice
  (ask "The driver for a USB stick crashes. On which design is the rest of the system most likely to carry on?")
  (right "A microkernel: the driver is an ordinary process, and can be restarted")
  (wrong "A monolithic kernel: everything is in one program, so it can mend itself"
    "In a monolithic kernel the driver runs inside the privileged kernel, so its bug can bring the whole system down.")
  (hint "Where does the driver run in each design?"))
```

---

## The command shell

The part of the system you see is a program like any other.

A **command shell** reads commands you type, runs them and shows the
results. It works as well over a serial line or a network as on the
computer's own screen. A program that shows such a text screen, for a
shell nearby or far away, is a **terminal**.

The shell shows a **prompt** when it is ready. Each command is a line:
the name of a program, then its **arguments**, separated by spaces. In
a Unix-like shell:

```text
> ls
letters  notes.txt  programs
> cat notes.txt
Order fuses. Copy the card to the spare.
> cp notes.txt /usb/notes.txt
>
```

`ls` lists the files in the current folder, `cat` shows a file, and
`cp` copies one. A command that succeeds often prints nothing.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: User Interfaces")
```

```check
(number
  (ask "In the command  cp notes.txt /usb/notes.txt  how many arguments are there?")
  (answer 2)
  (wrong-if 3 "cp is the program's name, not an argument. The arguments are what follow it.")
  (hint "The first word is the program. Count the words after it."))
```

---

```check
(choice
  (ask "You type  cp notes.txt /usb/notes.txt  and the shell prints nothing but its prompt. What does that most likely mean?")
  (right "The copy worked")
  (wrong "The copy failed silently"
    "A command that fails usually says why. One that succeeds often prints nothing at all.")
  (wrong "The shell did not run the command"
    "The prompt comes back once the command has run. With no message, it most likely succeeded.")
  (hint "What does a command that succeeds often print?"))
```

---

## REPLs and windows

An **interpreter** runs a program written in a programming language
directly, one part at a time. An interpreter can serve as a shell: a
**REPL** (read-eval-print loop) reads an expression, works it out,
prints the result and waits for the next. Many home computers of the
1980s started with a BASIC prompt of this kind.

A **graphical interface** has windows, a pointer and icons. A
**window manager** decides where windows go, draws their borders, and
passes key presses and mouse movements to the right application.

```check
(choice
  (ask "You press a key while two windows are open. Which part decides which application gets it?")
  (right "The window manager")
  (wrong "The keyboard"
    "The keyboard only sends a scan code. It knows nothing of windows; the window manager passes the key to the right application.")
  (wrong "Every application gets every key"
    "Then typing in one window would type in all of them. The window manager sends each key to one application.")
  (hint "Which part knows where the windows are and which one you are using?"))
```

---

## Simplicity as a feature

A modern general-purpose operating system is large: it must support
thousands of devices, many users at once, and nearly every program ever
written. A system for timeless applications can be much smaller: one
person, a known family of hardware, and a modest set of carefully
written programs.

A system small enough to read can be understood, trusted, repaired and
changed by the person who uses it, and when something goes wrong there
are fewer places to look.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: Simplicity as a Feature")
```

```check
(choice
  (ask "What is the main gain from an operating system small enough to read?")
  (right "Its user can understand it, and so trust, repair and change it")
  (wrong "It runs every program ever written"
    "That is what a large general-purpose system aims for, and why it is large. A small system gives up that aim.")
  (wrong "It supports more devices"
    "A small system supports a known family of hardware, not thousands of devices.")
  (hint "Who can do what with a system they can read?"))
```

So: a microkernel keeps the kernel small and runs the rest as
processes, and the shell, the REPL and the window manager are programs
like any other.
