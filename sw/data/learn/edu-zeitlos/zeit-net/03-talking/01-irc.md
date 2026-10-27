# Chatting with irc

```lesson
(objective "Say what IRC, a channel and a nick are, and talk in a channel with the irc app.")
(minutes 15)
(keywords "IRC" "channel (IRC)" "nick (IRC)")
```

This lesson is about talking with people, live, in writing.

**IRC** (Internet Relay Chat) is one of the oldest ways to chat on the
internet, and still in use. You connect to an IRC server. On it are
**channels**, each a room for one subject, with a name that starts with
`#`. Everything written in a channel is seen at once by everyone in it.

You are known by your **nick**, short for nickname: a short name you
choose, unique on that server while you use it.

```sources
(source "Zeitlos docs: irc_app.md")
(source "Timeless Computing (docs/tc.md), Communication: Applications")
```

```check
(choice
  (ask "You write a line in an IRC channel. Who sees it?")
  (right "Everyone who is in that channel")
  (wrong "Only the person you answered"
    "A channel is a shared room: every line in it goes to everyone there. Writing to just one person is done another way (next lesson).")
  (wrong "Everyone on the server"
    "Only those in the channel see it. A server has many channels.")
  (hint "A channel is like a room."))
```

---

## The irc app

Open `irc` from the dock. With the settings it comes with, it connects
at once to the Libera.Chat network, the server `irc.libera.chat`, with
the nick `zeitlos`. If someone already has that nick, it adds `_` and
tries again.

The window is one conversation: what people say, a status line, and a
line at the foot to type in. The keys go to that line all the time.

```check
(choice
  (ask "irc tries to connect with the nick zeitlos, but someone on the server already has it. What does irc do?")
  (right "Adds _ to the nick and tries again")
  (wrong "Gives up and shows an error"
    "It does not give up: it adds _ to the nick and tries again.")
  (wrong "Takes the nick from the other person"
    "A nick belongs to whoever has it while they are connected. irc picks another: zeitlos_.")
  (hint "Look for a character added at the end."))
```

---

## Saying something

A line that does not start with `/` is said in the channel you are in:
type it and press **Enter**. A line that starts with `/` is a
**command** to irc. The first one to learn is `/join`:

```
/join #fpga
```

joins the channel `#fpga` and shows it. `/join fpga` means the same:
irc adds the `#` for you. `/help` lists every command.

```check
(text
  (ask "What would you type to join the channel #books?")
  (accept "/join #books") (accept "/join books") (accept "/j #books") (accept "/j books")
  (wrong-if "join #books" "Without the / at the start, irc says the line in the channel instead of running it. Commands start with /.")
  (wrong-if "#books" "That would be said in the channel, as words. The command to join starts with /.")
  (hint "A command starts with /, then its name, then the channel."))
```

---

## Who is talking to you

The status line says which channel you are in, your nick and the
server. It also lists your other channels that have something new: a
`*` after one with new lines, a `!` after one where someone said your
nick. A line that mentions you is drawn in reverse.

**PageUp** and **PageDown**, or the wheel, scroll back through what
was said.

```check
(choice
  (ask "The status line shows #fpga! . What does the ! mean?")
  (right "Someone in #fpga said your nick")
  (wrong "#fpga has new lines, nothing more"
    "New lines are shown with a *. The ! means more: someone there said your nick.")
  (wrong "You were thrown out of #fpga"
    "The ! is good news, not bad: you were mentioned.")
  (hint "* is new lines; ! is stronger."))
```

---

```warn
(warn "IRC is not private" "irc talks to the server without encryption, on port 6667. Anything you type, a password included, crosses the network readable by any machine on the way. Never type a password or anything private in irc.")
```

That holds for every channel, and for writing to one person too. The server's
other users are strangers too: treat a channel like a public square.

```check
(choice
  (ask "A channel's helper asks you to type your password there to prove who you are. What do you do?")
  (right "Refuse: irc sends everything unencrypted, and a channel is public")
  (wrong "Type it, since the helper asked"
    "Everyone in the channel would see it, and so could any machine on the way. Never type a password in irc.")
  (wrong "Send it to the helper alone instead"
    "A line to one person is not encrypted either: it crosses the network readable, like everything in irc.")
  (hint "What does irc do to what you type before sending it? Nothing."))
```

---

```check
(text
  (ask "On IRC, what is the word for the name you are known by?")
  (accept "nick") (accept "nickname") (accept "a nick") (accept "a nickname")
  (hint "Short for nickname."))
```

So: IRC is live chat in channels on a server; irc joins with `/join`,
says what you type, and keeps nothing private.
