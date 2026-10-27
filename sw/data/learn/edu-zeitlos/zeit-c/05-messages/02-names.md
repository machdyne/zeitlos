# Names and pids

```lesson
(objective "Give a program a name others can find, look another program up by name, and send it a number.")
(minutes 20)
(keywords "process" "process ID" "pid")
```

A program that is running is a **process**. The system gives each
process a number, its **process ID** or **pid**, and a message's `to`
and `from` are pids. Run the same program twice and there are two
processes, with two pids.

A process can ask for its own: `unsigned z_getpid(void);`.

Pids are handed out as programs start, so you cannot know another
program's pid in advance. Names solve that.

```sources
(source "Timeless Computing (docs/tc.md), Operating Systems: processes")
(source "sw/common/zeitlos.h, PID name registry")
```

```check
(choice
  (ask "You start the same program twice. How many pids are there for it?")
  (right "Two: one for each process")
  (wrong "One: a program has one pid"
    "A pid belongs to a running process, not to the program file. Two runs are two processes.")
  (wrong "None until it opens a window"
    "Every process has a pid from the moment it starts, window or not.")
  (hint "Is a pid a number for a program file, or for one run of it?"))
```

---

## A name

```c
int z_pid_register(const char *base, char *out,
    unsigned size);
```

registers a name for the calling process. The system adds a number,
the first not in use: the first `tally` is `tally0`, a second one
running at the same time `tally1`. The full name is written into out,
an array of size bytes. It gives back 1 when it worked, 0 if not.

```c
int z_pid_lookup(const char *name, unsigned *pid);
```

finds the process with that full name. It gives back 1 and stores its
pid through the pointer, or 0 if no process has the name.

```sources
(source "sw/common/zeitlos.h, z_pid_register() and z_pid_lookup()")
(source "sw/apps/zcc/libz/syscall.c, z_pid_register(), z_pid_lookup()")
```

```check
(text
  (ask "Two copies of a program are running, each registered with the base name tally. What is the full name of the second?")
  (accept "tally1")
  (wrong-if "tally2" "Numbers start from 0: the first is tally0, the second tally1.")
  (wrong-if "tally" "The system always adds a number, even to the first: tally0.")
  (hint "The first one is tally0."))
```

---

## Sending a number

```c
int z_msg_send_u32(unsigned to, unsigned subject,
    unsigned tag, unsigned value);
```

sends a message carrying one number. It gives back 0 when the message
is in the receiver's mailbox, and 1 if not: no such process, or its
mailbox is full.

This program, send.c, sends 1 to 5 to tally0 with subject 1000, then
one message with subject 1001 to say it has finished:

```c
int printf(const char *format, ...);
int z_pid_lookup(const char *name, unsigned *pid);
int z_msg_send_u32(unsigned to, unsigned subject,
    unsigned tag, unsigned value);

#define NUMBER 1000
#define DONE   1001

int main(void)
{
    unsigned to;

    if (!z_pid_lookup("tally0", &to)) {
        printf("no tally0 running\r\n");
        return 1;
    }
    for (int i = 1; i <= 5; i++)
        z_msg_send_u32(to, NUMBER, 0, i);
    z_msg_send_u32(to, DONE, 0, 0);
    return 0;
}
```

```sources
(source "docs/libz.md, ABI 5: pointer-shaped wrappers (z_msg_send_u32)")
(source "docs/messaging.md, API: z_msg_send fails if `to` is not a live process or its mailbox is full")
```

```check
(number
  (ask "How many messages does send.c put in tally0's mailbox?")
  (answer 6) (from "5 + 1")
  (wrong-if 5 "Five numbers, and one more with subject DONE to say it has finished.")
  (hint "Count the loop's sends, then the one after it."))
```

---

## Practice

```check
(choice
  (ask "send.c runs while no tally0 is running. What happens?")
  (right "z_pid_lookup gives back 0, and send.c prints no tally0 running and ends")
  (wrong "The messages wait until a tally0 starts"
    "A message needs a pid to go to. With no process of that name, there is nobody to send to, and lookup says so.")
  (wrong "The system starts tally for it"
    "Looking up a name starts nothing. It only finds a process that is already running.")
  (hint "What does z_pid_lookup give back when no process has the name?"))
```

```check
(choice
  (ask "Why does send.c pass &to to z_pid_lookup?")
  (right "So that z_pid_lookup can store the pid it found in to")
  (wrong "Because to is a string"
    "to is an unsigned number. The & gives its address, so the function can store into it.")
  (wrong "So that z_pid_lookup can send the message"
    "z_pid_lookup only finds the pid. z_msg_send_u32 sends.")
  (hint "A function changes its caller's variable through the variable's address."))
```

So: each process has a pid, a registered name lets others find it, and
z_msg_send_u32 sends it a number.
