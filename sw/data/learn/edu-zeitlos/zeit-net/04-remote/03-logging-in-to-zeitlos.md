# Logging in to Zeitlos from afar

```lesson
(objective "Set up netserve so you can log in to your Zeitlos computer by SSH, and let callers reach your BBS.")
(minutes 20)
(keywords "listener (netserve)")
```

This lesson is about reaching your own computer's shells from another
machine.

A **listener** is one port netserve listens on, and where it sends the
people who connect there: which shell or program. This line makes an
SSH listener on port 22 that leads to the shell behind term's POSIX
button:

```
apps.netserve.ssh: 22 posix0
```

`posix0` is that shell's name, as in term's `port posix0`. From
another computer on your subnet you then type, in its own terminal,
`ssh` and your computer's address.

(As of this course, logging in to Zeitlos by SSH has been tested on the
build machine but not yet reported working on a board; telnet has.)

```sources
(source "Zeitlos docs: netserve.md, Configuration, Listeners, SSH")
```

```check
(choice
  (ask "What does the line apps.netserve.ssh: 22 posix0 do?")
  (right "Lets people log in by SSH on port 22, to the POSIX button's shell")
  (wrong "Connects your computer to another machine's port 22"
    "netserve's lines are for connections coming in. This one listens on port 22 and leads callers to posix0.")
  (wrong "Opens 22 term windows on POSIX"
    "22 is the port, the usual one for SSH.")
  (hint "A port, then where callers go."))
```

---

## The password

Zeitlos is a one-person computer with one password, the one that locks
the screen. Logins over the network ask for it, so it must be **10
characters or more**: telnet, and SSH with a password, stay off until
it is, and the console says why. Set it in `settings`, under Security,
which says *too short for network use* when it is shorter.

SSH also needs a board whose random number hardware is working: its
keys must be unguessable, so without it SSH stays off, saying so.

Three wrong passwords end the connection, and every attempt is logged
on the console with the caller's address.

```check
(number
  (ask "At least how many characters must your password have before netserve lets anyone log in with it?")
  (answer 10)
  (wrong-if 6 "6 is the least a BBS account takes. For logins to your computer itself, netserve wants at least 10.")
  (hint "settings warns about anything shorter: too short for network use."))
```

---

## Telnet too?

netserve can serve telnet as well (`apps.netserve.telnet: 23 repl0`).
But telnet sends your password as it is: anyone on the way can read it.
SSH sends it encrypted, and never as it is. Prefer SSH.

For SSH, netserve can also take **keys** instead of the password: lines
in `/data/netserve/authkeys`, copied from the other computer's public
key file. With `apps.netserve.ssh_auth: key`, it takes keys only.

```check
(choice
  (ask "You can switch on either telnet or SSH to reach your computer. Which is safer, and why?")
  (right "SSH, because it encrypts everything, the password included")
  (wrong "Telnet, because it is simpler"
    "Simpler, yes, but it sends your password readable by anyone on the way.")
  (wrong "They are the same, since both ask for the password"
    "Both ask, but telnet sends what you type as it is, and SSH encrypts it.")
  (hint "Which of the two sends the password readable?"))
```

---

## A BBS for callers

Your BBS logs its callers in itself, with their own handles and
passwords. A listener marked `noauth` asks for nothing and leads the
caller straight to it:

```
apps.netserve.telnet: 23 repl0; 2323 bbs0 noauth any
```

That is two listeners, separated by `;`: telnet on 23 to the REPL, with
your password, and on 2323 to the BBS, open to anyone (`any`). netserve
starts the BBS when the first caller comes. (Lists of listeners and
`noauth`, too, have so far been tested only on the build machine.)

`noauth` is safe only for a program that logs its own users in. A shell
refuses a `noauth` caller, so a slip such as `posix0 noauth` opens
nothing.

```sources
(source "Zeitlos docs: netserve.md, noauth; bbs.md, Running it")
```

```check
(choice
  (ask "Someone writes apps.netserve.ssh: 2222 posix0 noauth by mistake. What happens when a stranger connects to port 2222?")
  (right "The shell refuses them: shells do not take noauth callers")
  (wrong "They get your shell with no password"
    "That is exactly what the design prevents: posix refuses any caller who did not log in with the machine's password or key.")
  (wrong "They are asked for your password"
    "noauth means netserve asks nothing. It is the shell itself that refuses them.")
  (hint "Which programs may take noauth callers?"))
```

---

```check
(text
  (ask "In a netserve line, which character separates two listeners?")
  (accept ";")
  (wrong-if "," "Not a comma: commas can be part of a file name. Listeners are separated by a semicolon.")
  (hint "Look at the BBS example: between repl0 and 2323."))
```

So: a listener is a port and where it leads; logins need a password of
10 characters or more, SSH is the one to use, and `noauth` is only for
a program, such as the BBS, that logs its callers in itself.
