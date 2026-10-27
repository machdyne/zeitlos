# Messages

```lesson
(objective "Describe how Zeitlos processes send each other messages, and why a message's data is not copied.")
(minutes 20)
(keywords "mailbox" "borrowed payload" "name registry")
```

You will learn how Zeitlos programs talk to each other and to the
drivers.

Messages are the one way Zeitlos programs ask each other, the window
manager or a driver to do something. Each process has a **mailbox** in
the kernel, holding up to 32 messages. A message has:

- `to` and `from` (the kernel fills in `from`, so it cannot be faked);
- a `subject`, a number saying what kind of message it is;
- a `tag`, to match an answer to its question;
- the payload: a number, a string, a list, a map or a block of bytes.

A process waiting for a message blocks in its mailbox and uses no time.
If a mailbox is full, the send is refused.

```sources
(source "Zeitlos docs: messaging.md, Overview; z_msg_t")
(source "Zeitlos source: sw/common/zmsg.h, Z_MAILBOX_DEPTH")
```

```check
(choice
  (ask "Why does the kernel, not the sender, fill in a message's from?")
  (right "So a program cannot pretend that its message came from another")
  (wrong "Because programs do not know their own pid"
    "A program can ask its pid. The kernel stamps from so that no program can forge it.")
  (wrong "To save the sender a few instructions"
    "The point is trust, not speed: the receiver can rely on from."))
```

---

## Borrowed, not copied

Copying every string from one process to another would be slow on a
48 MHz CPU. Zeitlos does not copy. Every byte has one physical address
that any process may read, so the kernel turns the sender's pointer
into a physical one:

physical = pointer - `0x8000_0000` + the sender's base

and the receiver reads the data where it lies. Numbers are copied, but
a string, list, map or block of bytes is a **borrowed payload**: the sender must leave
it alone until it has its answer, and the receiver must not change it.
If the receiver tries to, the MPU stops it and reports the receiver.

```sources
(source "Zeitlos docs: messaging.md, Why messages aren't serialized; The cost: borrowed data has a lifetime; mpu.md, What it catches")
```

```check
(text
  (ask "A process with base 0x4030_0000 sends a string at 0x8000_0100. At which physical address does the receiver read it?")
  (accept "0x40300100") (accept "40300100") (accept "0x4030_0100") (accept "4030_0100")
  (wrong-if "0x80000100" "That is the sender's own view. The receiver's window shows its own block, not the sender's.")
  (wrong-if "0xC0300100" "The window's 0x8000_0000 is taken off before the base is added.")
  (hint "Take off 0x8000_0000, then add the base."))
```

---

```check
(choice
  (ask "A program receives a string and wants to change it to capitals. What should it do?")
  (right "Copy it into its own memory first, then change the copy")
  (wrong "Change it where it lies; it is only a string"
    "It lies in the sender's memory. The MPU blocks the store, and the program is ended with a report.")
  (wrong "Free it and ask for it again"
    "A receiver must never free a borrowed payload: it is not its memory."))
```

---

## Finding each other by name

Pids change: a driver that is restarted gets a new one. So programs
find each other through the kernel's **name registry**. A program
registers a name and the kernel adds a number: the first `term` becomes
`term0`, the next `term1`. The window manager is `wm0`, the filesystem
`fs0`, USB `usb0` and the network `net0`. Others look the name up and
get the pid.

```sources
(source "Zeitlos docs: app_runtime.md, The syscall trampoline (the pid name registry); userspace_drivers.md, How it works now")
```

```check
(text
  (ask "Two term windows are open. Under which name did the second one register?")
  (accept "term1")
  (wrong-if "term2" "Numbering starts at 0: the first is term0, the second term1.")
  (wrong-if "term" "The kernel adds a number to the name each program asks for.")
  (hint "The first is term0."))
```

---

```check
(number
  (ask "How many messages can wait in one mailbox?")
  (answer 32)
  (wrong-if 1 "A mailbox is a queue, not a single slot.")
  (hint "The same number as the process table's slots."))
```

So: Zeitlos processes send messages through kernel mailboxes, find each other by registered name, and lend their data rather than copy it.
