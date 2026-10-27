# The mailbox

```lesson
(objective "Name the parts of a message, say how messages wait in a mailbox, and choose a subject number for messages of your own.")
(minutes 20)
(keywords "tag (of a message)")
```

Your window program already reads messages from wm. Any two programs
can talk the same way. A message is a small record with five parts,
which struct msg in win.h names:

| field | is |
|---|---|
| to | the program it is for |
| from | the program that sent it, filled in by the system |
| subject | what kind of message it is |
| tag | a number to match a reply to its question; often 0 |
| obj | the value it carries: its type, and the value |

The **tag** lets a program that asked several questions tell which
reply is which. Because the system fills in `from`, a program cannot
pretend to be another.

```sources
(source "docs/messaging.md, z_msg_t; Subjects and tags")
```

```check
(choice
  (ask "A program receives a message and wants to answer whoever sent it. Which field says who that was?")
  (right "from")
  (wrong "to"
    "to is the receiver: the program reading it. The sender is in from.")
  (wrong "tag"
    "tag is a number the sender chose, often 0. The sender itself is in from, which the system fills in.")
  (hint "The field the system fills in, so that it cannot be wrong."))
```

---

## The mailbox

Messages sent to a program wait in its mailbox, in the order they
came, until it reads them. A mailbox holds 32. When it is full,
sending to it fails, so a program that never reads its messages
makes every message sent to it fail.

- `z_msg_read(&m)` takes the oldest message into m and gives back 0,
  or gives back 1 at once if there is none.
- `z_proc_wait(0)` puts the program to sleep until a message comes.

```sources
(source "docs/messaging.md, API; Z_MAILBOX_DEPTH 32")
(source "docs/app_runtime.md, Idling: why background processes must block")
```

```check
(choice
  (ask "Why does the message loop call z_proc_wait(0) rather than calling z_msg_read over and over?")
  (right "Waiting asleep takes no time from other programs; asking over and over keeps the processor busy for nothing")
  (wrong "z_msg_read cannot be called twice in a row"
    "It can, and it gives back 1 when the mailbox is empty. The point is not to spend the processor's time asking.")
  (wrong "z_proc_wait reads the message"
    "z_proc_wait only waits. The loop still reads with z_msg_read once it wakes.")
  (hint "What does a program that keeps asking do to every other program?"))
```

---

## Reading them all

When the program wakes, several messages may be waiting: mouse moves
come in bursts. So the loop reads with `while`, until z_msg_read gives
back 1, and only then sleeps again.

```c
for (;;) {
    z_proc_wait(0);
    while (z_msg_read(&m) == 0) {
        /* one message: act on it */
    }
}
```

```check
(choice
  (ask "A loop reads one message each time it wakes, then sleeps again. Five messages arrive together. What happens?")
  (right "z_proc_wait returns at once while the mailbox is not empty, so they are read, one turn at a time")
  (wrong "Four of them are lost"
    "Messages wait in the mailbox until they are read. Nothing is thrown away.")
  (wrong "The program sleeps for ever with four messages waiting"
    "z_proc_wait returns at once if a message is already there. It is only slower: a whole turn of the loop each time.")
  (hint "Does z_proc_wait sleep when there is already a message?"))
```

---

## Subjects of your own

Every kind of conversation in Zeitlos owns a block of subject numbers,
so that no two mean the same thing: wm's are 100 to 199, connections
200 to 215, the filesystem's 432 to 447, and so on.

Messages between your own programs need subjects outside every block.
Numbers from 1000 up are free; this course uses 1000 and 1001.

```sources
(source "sw/common/zsubjects.h, the blocks")
```

```check
(choice
  (ask "Your two programs use subject 106 for \"here is a number\", and one of them has a window. What goes wrong?")
  (right "106 is wm's WM_KEY: key presses and your numbers look the same")
  (wrong "Nothing: subjects only have to differ within one program"
    "A program with a window hears from wm too. One number with two meanings is read as whichever the program checks first.")
  (wrong "The system refuses subject numbers below 1000"
    "The system does not check. Choosing a number outside everyone's block is up to you.")
  (hint "Which block is 106 in?"))
```

---

## Numbers, and borrowed strings

A message carries one value. A number is copied into the message, so
it is safe whatever the sender does next.

A string is not copied: the receiver gets a pointer into the sender's
memory. It is only good while the sender leaves that memory alone, and
gone when the sender ends. So messages that go one way and get no
reply should carry numbers.

```sources
(source "docs/messaging.md, The cost: borrowed data has a lifetime")
```

```check
(choice
  (ask "A program sends a string in a message and then ends at once. What does the receiver find when it reads the message?")
  (right "A pointer into memory that no longer belongs to the sender: the text may be anything")
  (wrong "Its own copy of the string"
    "Strings are not copied into messages. The receiver reads the sender's own memory.")
  (wrong "Nothing: the message is removed when the sender ends"
    "The message stays in the mailbox. Only the string it points to is no longer safe.")
  (hint "Is a string copied into a message?"))
```

So: messages wait in a mailbox in order, a loop sleeps until one comes
and then reads them all, and your own subjects go outside the system's
blocks.
