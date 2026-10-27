# Drivers are programs

```lesson
(objective "Say which Zeitlos drivers run as processes, what makes a program a driver, and what happens when one crashes.")
(minutes 15)
(keywords "driver flag" "grant window")
```

You will learn why the Zeitlos drivers are programs and not kernel code.

Three drivers run as ordinary processes, each found by its name:

| Driver | Name | Does |
|---|---|---|
| fs | fs0 | the filesystem: the SD card, the RAM disk, the flash's files |
| usb | usb0 | the USB host: hubs, keyboards, sticks, adapters |
| net | net0 | the network: Ethernet, IP, TCP and more |

They are preempted like any other program, so no disk or network work
ever holds up the scheduler. And they need not fit in the kernel's
share of flash: a newer `usb` or `net` can even come from the SD card.

```sources
(source "Zeitlos docs: userspace_drivers.md, the introduction; How it works now; Growing past the flash region")
```

```check
(choice
  (ask "Why are the Zeitlos filesystem and network drivers programs, not kernel code?")
  (right "So no input or output ever holds up the scheduler, and a crash can be survived")
  (wrong "Because the kernel cannot reach the SD card or Ethernet"
    "The kernel could; it used to contain both. They were moved out for responsiveness and to survive crashes.")
  (wrong "Because programs run faster than kernel code"
    "They run on the same CPU at the same speed. The gain is that they are scheduled and contained like any program."))
```

---

## The driver flag

What makes a program a driver is one bit in its own file's header: the
**driver flag**, bit 15 of the flags word. No list in the kernel names
the drivers, so a new one needs no kernel change.

A driver may open the **grant window**: permission, one client at a
time, to write straight into a client's memory, such as the buffer a
program is reading a file into. Only a client that has agreed to be
written by drivers can be opened; the file and stream libraries agree
for it on first use, and a program that does no input or output keeps
full protection.

```sources
(source "Zeitlos docs: userspace_drivers.md, Drivers (declaring is not granting); executables.md, Layout")
```

```check
(choice
  (ask "fs is a driver. Can it write into any program's memory it likes?")
  (right "No: only into a client that has agreed, through the grant window, one at a time")
  (wrong "Yes: drivers are privileged like the kernel"
    "Drivers are ordinary, unprivileged processes. The flag lets them ask for a grant, nothing more.")
  (wrong "No: drivers can never write into another program"
    "They can, through the grant window, so that a file can be read straight into the program's buffer."))
```

---

## When a driver crashes

The MPU ends a driver that breaks its rules, as it ends any program.
But a machine without its filesystem is not much use, so when `fs`,
`usb` or `net` **crashes**, the kernel starts it again a second later.
At most 3 times a minute: a fourth crash within the minute is left for
a person to look at. A driver that exits or is killed on purpose is not
restarted. Its clients look the name up again and carry on.

```sources
(source "Zeitlos docs: userspace_drivers.md, Restarting drivers")
```

```check
(number
  (ask "net crashes four times within one minute. How many times does the kernel restart it?")
  (answer 3)
  (wrong-if 4 "The fourth crash in the minute is not restarted: the kernel prints that it is not restarting it.")
  (wrong-if 0 "The kernel restarts a crashed driver, up to 3 times a minute.")
  (hint "At most 3 times a minute."))
```

---

```check
(choice
  (ask "After fs is restarted it has a new pid. How do programs that were using it find it?")
  (right "They look up the name fs0 again")
  (wrong "The kernel gives it the old pid back"
    "It gets a new one. Clients find drivers by name, not by pid.")
  (wrong "They must be restarted too"
    "No: the client libraries look the pid up again when a call fails or times out."))
```

So: in Zeitlos the filesystem, USB and the network are driver programs, marked by a flag in their header, contained like any program and restarted if they crash.
