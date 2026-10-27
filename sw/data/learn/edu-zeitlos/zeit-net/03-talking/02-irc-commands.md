# Channels, people and views

```lesson
(objective "Send private messages, move between irc's views, leave a channel, and choose irc's server and nick.")
(minutes 15)
(keywords "private message (IRC)" "view (irc)")
```

This lesson is about the rest of what you do in `irc`.

A **private message** goes to one person, not to a channel:

```
/msg alice Are you coming tonight?
```

Only alice sees it (and, since IRC is not encrypted, any machine on the
way). irc opens a conversation with her, where her answers appear.
`/query alice` opens that conversation without saying anything.

```check
(text
  (ask "Write the command that sends the private message hello to the nick bob.")
  (accept "/msg bob hello") (accept "/query bob hello")
  (wrong-if "/join bob" "/join is for channels. A message to one person is /msg, the nick, then the words.")
  (wrong-if "bob hello" "Without a / at the start, irc says the line in the channel. A private message is a command.")
  (wrong-if "/msg hello bob" "The nick comes first, then the words.")
  (hint "/msg, then the nick, then the text."))
```

---

## Views

Each channel you are in and each private conversation has its own
**view**: its own page of lines. The server has one more, for
everything that belongs to no channel: its welcome message, errors,
people leaving.

**Ctrl+N** goes to the next view and **Ctrl+P** to the previous one.
The status line tells you which view you are in, and which others have
something new.

```check
(choice
  (ask "You are in #fpga and have a private conversation with alice. How do you switch to alice's view?")
  (right "Ctrl+N or Ctrl+P, until the status line shows alice")
  (wrong "Type /join alice"
    "That would join a channel called #alice. To switch views, use Ctrl+N and Ctrl+P (or /query alice).")
  (wrong "Press Tab"
    "Tab does not move between views in irc. Ctrl+N and Ctrl+P do.")
  (hint "N for next, P for previous."))
```

---

## More commands

| Command | Does |
|---|---|
| /part | leaves the channel you are in |
| /me waves | says that you wave: * zeitlos waves |
| /nick newname | changes your nick |
| /topic | shows the channel's topic |
| /names | lists who is in the channel |
| /whois nick | asks the server about someone |
| /quit | disconnects from the server |
| /connect host port | connects to another server |

```check
(choice
  (ask "You want to leave the channel you are in, but stay on the server. Which command?")
  (right "/part")
  (wrong "/quit"
    "/quit disconnects you from the whole server, every channel at once. /part leaves just this channel.")
  (wrong "/names"
    "/names lists who is in the channel. Leaving is /part.")
  (hint "You take part in a channel, and then you ..."))
```

---

## Going elsewhere

`irc`'s settings are lines in `/sys/zeitlos.cfg`, the system's settings
file, which you can edit in `text`:

```
apps.irc.server: irc.libera.chat 6667
apps.irc.nick: zed
apps.irc.channels: #fpga #books
```

The server is a host name and a port. The nick is the one irc asks for
when it connects, and the channels are joined once it has. With the
server line empty, irc waits for you to type `/connect`.

```sources
(source "Zeitlos docs: irc_app.md, Configuration; config.md")
```

```check
(choice
  (ask "You want irc to join #books every time it connects. What do you change?")
  (right "The apps.irc.channels line in /sys/zeitlos.cfg")
  (wrong "The apps.irc.nick line"
    "That sets your nick. The channels to join have a line of their own.")
  (wrong "Nothing: irc remembers the channels you joined last time"
    "irc keeps no list of its own. Channels to join at every start go in its settings.")
  (hint "Look at the three settings lines."))
```

---

```example
(example
  (problem "Join #books, say hello there, then leave it.")
  (step "Type /join #books and press Enter. The view for #books opens.")
  (step "Type hello and press Enter: a line without a / is said in the channel.")
  (step (choice (ask "Now, to leave #books?")
    (right "/part")
    (wrong "/quit" "That would disconnect from the whole server.")))
  (answer "/join #books, hello, /part."))
```

```check
(text
  (ask "Which command changes your nick to zed?")
  (accept "/nick zed")
  (wrong-if "/me zed" "/me says what you are doing. Changing the nick is /nick.")
  (wrong-if "nick zed" "Without the / it would be said in the channel. Commands start with /.")
  (hint "The command has the same name as the thing it changes."))
```

So: `/msg` talks to one person, each channel and person has a view
(Ctrl+N, Ctrl+P), `/part` leaves, and the settings choose where irc
connects.
