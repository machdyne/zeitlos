# A link between two machines

```lesson
(objective "Say what two machines joined by one cable must agree on, and how terminals, modems and bulletin boards grew from such links.")
(minutes 15)
(keywords "teleprinter" "escape sequence" "modem" "bulletin board system" "BBS" "Telnet" "SSH")
```

The simplest network is two computers joined by a serial cable, as in
the UART lesson: each byte one sends arrives at the other. Even this
needs some agreement between the two:

- the same speed;
- the same way of marking where one message ends and the next begins;
- some way to notice a byte that arrives damaged.

```sources
(source "Timeless Computing (docs/tc.md), Communication: A Link Between Two Machines")
```

```check
(choice
  (ask "Two computers on a serial cable agree on the speed, and every byte arrives. One sends two messages in a row. Why might the other still misread them?")
  (right "They have not agreed how to mark where one message ends")
  (wrong "Serial cables mix bytes up"
    "The bytes arrive in order. But a stream of bytes has no gaps of its own; both ends must agree on a way to mark the end of each message.")
  (wrong "A serial cable can carry only one message"
    "It carries as many bytes as you like. The receiver just needs to know where each message stops.")
  (hint "Speed is agreed. What else is on the list?"))
```

---

## Terminals

The first computer terminals were **teleprinters**: typewriters that
sent each key press down a wire and printed whatever came back. The
terminals with screens that followed added **escape sequences**: runs
of characters starting with the escape character (code 27) that move
the cursor, clear the screen or change colours instead of being
printed.

The sequences of the DEC VT100 terminal of 1978 became a standard that
nearly every terminal program still understands. A terminal program on
a modern computer is in effect a VT100 made of software.

```check
(choice
  (ask "A program sends a terminal the escape character followed by a few more characters. What happens?")
  (right "The terminal acts on them, for example moving the cursor, instead of printing them")
  (wrong "The terminal prints them as ordinary text"
    "The escape character tells the terminal that what follows is a command, such as moving the cursor or clearing the screen.")
  (wrong "The terminal ends the connection"
    "Escape sequences control the screen; they do not end anything.")
  (hint "What are escape sequences for?"))
```

---

## Modems and bulletin boards

For decades, people joined computers over the telephone network with
**modems**, which turn bits into sounds and back. A common use was the
**bulletin board system** (**BBS**): a computer others could dial into,
one or a few callers at a time, to read and post messages, swap files
and play games.

Many BBSes now run on the Internet and are reached with **Telnet** or
**SSH**: programs that open a terminal on another computer across a
network. They are still a text-based way for a community to meet.

```check
(choice
  (ask "Why did a modem turn bits into sounds?")
  (right "The telephone network was built to carry sound")
  (wrong "Sounds are faster than voltages"
    "They are not. A telephone line was made for voices, so bits had to become sounds to travel on it.")
  (wrong "So that people could listen to the data"
    "People could hear it, but that was not the point. The telephone network carries sound, so data had to be sound.")
  (hint "What was the telephone network made to carry?"))
```

So: even one cable needs agreed speed, message ends and error checks,
and terminals, modems and bulletin boards were built on such links.
