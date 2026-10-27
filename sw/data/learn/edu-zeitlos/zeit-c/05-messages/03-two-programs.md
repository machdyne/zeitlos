# Two programs together

```lesson
(objective "Write a program that registers a name, starts a second program, and adds up the numbers it sends.")
(minutes 25)
```

The last lesson's send.c needs a tally0 to send to. Here it is,
tally.c. It registers its name, then starts send itself: a
program can start another with

```c
unsigned z_proc_run(const char *path);
```

which gives back the new process's pid, or 0 if it could not start it.
Starting a program this way is what the shell does when you type a
program's name.

```sources
(source "sw/common/zeitlos.h, z_proc_run(): the new pid, or 0 on failure")
(source "sw/apps/posix/sh.c, bi_run(): the shell starts programs with z_proc_run")
```

```check
(choice
  (ask "Why must tally register its name before it starts send?")
  (right "send looks up tally0 as soon as it runs; if the name is not there yet, it finds nobody")
  (wrong "z_proc_run needs a name to start a program"
    "z_proc_run takes the path of the program file. The name is for send, which looks tally0 up.")
  (wrong "The order does not matter"
    "send may run before tally gets its next turn. If tally0 is not registered by then, send's lookup fails.")
  (hint "What is the first thing send.c does?"))
```

---

## tally.c

```c
#include "win.h"

int printf(const char *format, ...);
int z_pid_register(const char *base, char *out,
    unsigned size);
unsigned z_proc_run(const char *path);

#define NUMBER 1000
#define DONE   1001

int main(void)
{
    struct msg m;
    char name[16];
    unsigned child;
    int total = 0;

    if (!z_pid_register("tally", name, sizeof name))
        return 1;
    printf("I am %s\r\n", name);
    child = z_proc_run("/home/c/send");
    if (child == 0) {
        printf("cannot start send\r\n");
        return 1;
    }
    for (;;) {
        z_proc_wait(0);
        while (z_msg_read(&m) == 0) {
            if (m.from != child)
                continue;
            if (m.subject == NUMBER)
                total += m.obj.val;
            else if (m.subject == DONE) {
                printf("total %d\r\n", total);
                return 0;
            }
        }
    }
}
```

It uses win.h only for struct msg, z_msg_read and z_proc_wait.

```check
(number
  (ask "send.c sends 1, 2, 3, 4 and 5, then DONE. What total does tally print?")
  (answer 15) (from "1 + 2 + 3 + 4 + 5")
  (wrong-if 6 "That counts the messages. tally adds the numbers they carry.")
  (wrong-if 10 "Add all five numbers: the 5 as well.")
  (hint "Add the values of the NUMBER messages."))
```

---

## Only from the child

`if (m.from != child) continue;` skips any message that did not come
from the process tally started: `continue` goes straight on to the
next turn of the loop. Since the system fills in `from`, another
program cannot slip numbers into the total.

```check
(choice
  (ask "Another program sends tally a NUMBER message with the value 100 while send is running. What happens to it?")
  (right "It is read and skipped: its from is not child")
  (wrong "100 is added to the total"
    "tally checks from first. A message from any process but child goes no further.")
  (wrong "tally stops with an error"
    "continue just moves on to the next message. Nothing stops.")
  (hint "What does tally compare before it looks at the subject?"))
```

---

## Run them

In POSIX, compile both, then run tally:

```text
zcc /home/c/send.c -o /home/c/send
zcc /home/c/tally.c -o /home/c/tally && tally
```

tally starts send, and the CONSOLE window shows `I am tally0` and then
the total.

```try
(try (app "term")
  (task "Save send.c and tally.c in /home/c, compile both in term's POSIX window, and run tally. Read the CONSOLE window, then come back.")
  (check (number (ask "What total did tally print?")
    (answer 15) (from "1 + 2 + 3 + 4 + 5")
    (wrong-if 0 "If no numbers arrived, check that send.c looks up the name tally0 and that tally starts /home/c/send.")
    (hint "It is the sum of 1 to 5."))))
```

---

## A full mailbox

tally's mailbox holds 32 messages. If send.c sent 1 to 100, it could
fill it faster than tally reads, and then z_msg_send_u32 gives back 1:
that number would be lost. A careful sender checks, and waits a moment
before trying again:

```c
void z_proc_wait(unsigned ticks);

while (z_msg_send_u32(to, NUMBER, 0, i) != 0)
    z_proc_wait(1);
```

`z_proc_wait(1)` sleeps for one tick of the system's clock, or until a
message comes, whichever is first.

```sources
(source "docs/messaging.md, API; Z_MAILBOX_DEPTH")
(source "sw/common/zeitlos.h, z_proc_wait(): until a message or the timeout")
```

```check
(number
  (ask "With the careful loop, send.c sends 1 to 100. What total does tally print?")
  (answer 5050) (from "100 * 101 / 2")
  (wrong-if 100 "100 is how many numbers. Add their values: 1 + 2 + ... + 100.")
  (hint "Pair them: 1 + 100, 2 + 99, ... fifty pairs of 101."))
```

So: one program can start another, find it by pid and name, and the
two work together by messages, each checking what it sends and whom it
hears from.
