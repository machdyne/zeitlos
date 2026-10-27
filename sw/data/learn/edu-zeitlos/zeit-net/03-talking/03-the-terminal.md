# The terminal

```lesson
(objective "Use term's panel and Open bar to connect to a shell or another machine, and get back with F12.")
(minutes 20)
(keywords "terminal (program)" "shell (program)" "telnet" "SSH")
```

This lesson is about `term`, the window that connects you to programs
you talk to by typing.

A **terminal** is a window that sends the keys you press to a program
and shows what the program writes back, as lines of text on a grid of
80 columns and 25 rows. The program can be on this computer or on
another one across the network. `term` is Zeitlos's terminal.

A new `term` window is connected to nothing. It shows a panel with four
buttons:

```
REPL     Scheme and system commands
POSIX    Unix-style shell, zcc and vi
CONSOLE  Boot log and kernel shell
OPEN     port, serial, telnet or ssh
```

REPL and POSIX are **shells**: programs you type commands into, one
line at a time. Press a button, or Tab to one and press Enter.

```sources
(source "Zeitlos docs: terminal.md, Starting out: the panel")
```

```check
(choice
  (ask "You open a new term window. What is it connected to?")
  (right "Nothing yet: it shows a panel to choose from")
  (wrong "The REPL, always"
    "It used to connect to the REPL by itself. Now a new window waits on its panel for you to choose.")
  (wrong "The last machine you connected to"
    "term remembers no connection. A new window starts on its panel.")
  (hint "What does the panel's title say?"))
```

---

## The Open bar

To go somewhere the buttons do not, just type on the panel: a bar opens
across the foot, `open>`, with what you typed. Enter connects and Esc
cancels. It takes a word and where to go:

| Typed | Connects to |
|---|---|
| port repl0 | a program on this computer, by its name |
| telnet 192.168.1.10 | a shell on another machine, by telnet |
| ssh me@192.168.1.10 | a shell on another machine, by SSH |

**Telnet** sends everything as it is, your password too. **SSH**
(Secure Shell) does the same job encrypted, and checks the machine is
the one you meant. The last unit of this course is about it.

```check
(choice
  (ask "You want a shell on another machine, and the network between you is one you do not trust. Which do you type?")
  (right "ssh me@192.168.1.10")
  (wrong "telnet 192.168.1.10"
    "Telnet sends everything unencrypted, your password included. On a network you do not trust, use SSH.")
  (wrong "port repl0"
    "port connects to a program on this computer, not another machine.")
  (hint "Which of the two encrypts?"))
```

---

## Waiting

A connection across the network can take a while: the bar says
`connecting to ... (Esc cancels)`. A machine that does not answer can
take up to 45 seconds to fail, so **Esc** gives up at once. When a
connection fails or is cancelled, the panel comes back and says why.

```check
(choice
  (ask "You typed telnet to a machine that is switched off, and term says connecting... What do you press to give up now?")
  (right "Esc")
  (wrong "F5"
    "In term, F5 goes to the far end, and nothing is connected yet. Esc cancels the connect.")
  (wrong "Nothing: you must wait for it to fail"
    "You need not wait the 45 seconds: Esc cancels.")
  (hint "The bar tells you."))
```

---

## F12: home

**F12** disconnects and brings back the panel, from anywhere, whatever
the program at the other end is doing. It is your way out of a program
that has stopped answering, or that has no command to leave.

From the panel, F12 and then typing is how you go somewhere else. The
window also comes back to the panel by itself when the other end
closes the connection.

```try
(try (app "term")
  (task "Open term from the dock. Press REPL. Then press F12 and see the panel come back. Type port repl0 and press Enter to connect again, then F12 once more. Then come back here.")
  (check (choice
    (ask "What did F12 do?")
    (right "Disconnected, and showed the panel")
    (wrong "Closed the term window" "The window stays: F12 disconnects it and shows its panel, ready to go elsewhere.")
    (wrong "Showed term's help" "term has no help box: its panel is the help. F12 disconnects."))))
```

```check
(choice
  (ask "The program on the other machine has stopped answering any key. How do you get out?")
  (right "F12")
  (wrong "Ctrl+C"
    "Ctrl+C goes to the program at the other end, which is not answering. F12 is caught by term itself.")
  (wrong "Esc"
    "While connected, Esc goes to the program at the other end. F12 is term's own.")
  (hint "term's home key."))
```

---

## The other keys

term is the exception to the function keys: **F1 to F10 go to the
program at the other end**, which may use them. **F11** is full screen,
as for any window. term has no help box: the panel is its help.

Text that has scrolled off the top is kept: **Shift+PgUp** and
**Shift+PgDn** scroll back and forward. The keys without Shift belong
to the program.

```check
(choice
  (ask "You are connected to a shell in term, and press F1. What happens?")
  (right "F1 goes to the program at the other end")
  (wrong "term's help box opens"
    "term has no help box, and while connected F1 is sent to the program at the other end.")
  (wrong "term disconnects"
    "That is F12. F1 to F10 go to the far end.")
  (hint "term is the exception to the usual function keys."))
```

---

```check
(text
  (ask "In term's Open bar, which word connects to a program on this computer by its name, as in ... repl0?")
  (accept "port")
  (wrong-if "telnet" "telnet reaches a shell on another machine. A program here is reached by another word.")
  (hint "The table's first row."))
```

So: `term` starts on its panel; typing opens the bar for `port`,
`telnet` or `ssh`; Esc cancels a connect, and F12 always brings you
home.
