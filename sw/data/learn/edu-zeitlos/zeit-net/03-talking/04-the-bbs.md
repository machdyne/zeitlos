# The BBS

```lesson
(objective "Say what a BBS is, call Zeitlos's own BBS from term, make an account and find your way round its menu.")
(minutes 20)
(keywords "BBS" "bulletin board system" "modem" "sysop" "handle (BBS)" "forum (BBS)")
```

This lesson is about the bulletin board, the oldest of the online
places in this course.

A **BBS**, a **bulletin board system**, is a computer that others call to
read and post messages, send each other mail and play games, a few
callers at a time. For decades people called them over the telephone
with **modems**, devices that turn data into sound and back. Many now run on the internet, reached with telnet or SSH,
and they are still a text-based place for a community to meet. The
person who runs one is its **sysop** (system operator).

```sources
(source "Timeless Computing (docs/tc.md), Communication: A Link Between Two Machines")
(source "Zeitlos docs: bbs.md")
```

```check
(choice
  (ask "How do people reach a BBS today, from a Zeitlos computer?")
  (right "From term: telnet or SSH across the network, or port bbs0 on this computer")
  (wrong "From web, as a page"
    "A BBS is a terminal place, made of text screens and menus. You call it from term.")
  (wrong "Only over a telephone line"
    "That was how it was done for decades. Now most are on the internet, called with telnet or SSH.")
  (hint "A BBS shows screens of text, for a terminal."))
```

---

## Calling the BBS on your own computer

Zeitlos has a BBS of its own, the app `bbs`. It is not on the dock: you
start it from the REPL, then call it.

1. Open `term` and press **REPL**.
2. Type `(run "bbs")` and press Enter.
3. Press **F12** to go back to the panel.
4. Type `port bbs0` and press Enter.

`bbs0` is the name the BBS answers to. The Open bar does not start
programs, which is why the BBS is started first.

```check
(choice
  (ask "You type port bbs0 in term's Open bar, but the connection fails. You have not started anything. Why?")
  (right "The BBS is not running: the Open bar only connects, it does not start programs")
  (wrong "bbs0 is only for callers from the network"
    "Calls on your own computer are welcome too. But something must answer the name bbs0.")
  (wrong "You must use telnet for a BBS"
    "telnet is for a BBS on another machine. On this one, port bbs0 is right, once the BBS is running.")
  (hint "Which step did you skip?"))
```

---

## Joining

The BBS first asks your terminal a question, to learn what it can
show, then greets you and asks for your **handle**: your name on the
board. A new caller types `NEW`, then:

- a handle, 2 to 20 characters;
- a password of 6 or more characters, twice (it is not shown);
- where you are, if you want to say;
- `Y` to make the account.

**The first account made is the sysop's.** Make yours before anyone
else can call.

```check
(number
  (ask "At least how many characters must a BBS password have?")
  (answer 6)
  (wrong-if 10 "10 is what Zeitlos asks of the machine's own password for logins over the network. The BBS asks for at least 6.")
  (hint "The prompt says it: Password (... or more characters)."))
```

---

## The menu

```
   [N] New messages, everywhere
   [F] Forums
   [M] Mail
   [B] Bulletins
   [W] Who is online
   [U] Users
   [P] Profile and terminal
   [G] Goodbye
```

Press the letter in brackets. A **forum** is a place for messages on one
subject, which every caller can read. Mail is letters to one user.
**N** reads everything new since your last call, mail first. Bulletins
are notices from the sysop. The sysop also sees **S**, for looking
after users.

```check
(choice
  (ask "You want to read every message posted since you last called. Which key?")
  (right "N")
  (wrong "F"
    "F lists the forums, to choose one. N goes through everything new, everywhere, in order.")
  (wrong "B"
    "B shows the bulletins: the sysop's notices. N reads the new messages.")
  (hint "New messages, everywhere."))
```

---

## Reading and writing

A message shows who wrote it, its subject and its date, then its words a
page at a time. Then: **N** next (or Enter), **P** previous, **R**
reply, **Q** quit.

To write, press **W** in a forum, or **R** on a message. On `term` the
BBS gives you a full-screen editor: type, then **Ctrl+S** to post or
**Ctrl+X** to leave without posting. **G** on the menu says goodbye:
the BBS hangs up, and `term` shows its panel.

```check
(choice
  (ask "You have written a message in the BBS's editor. How do you post it?")
  (right "Ctrl+S")
  (wrong "Ctrl+X"
    "Ctrl+X leaves the editor without posting (it asks first). Ctrl+S sends.")
  (wrong "Enter"
    "Enter starts a new line in your message. Ctrl+S posts it.")
  (hint "S for send."))
```

---

Others can call your BBS over the network too, once you set it up to
be reached ([bbs](docs:bbs)); the next unit is about letting other
machines in.

```check
(text
  (ask "What is the person who runs a BBS called?")
  (accept "sysop") (accept "the sysop") (accept "a sysop") (accept "system operator")
  (wrong-if "admin" "That is a word for it elsewhere. On a BBS it has a name of its own.")
  (hint "Short for system operator."))
```

So: a BBS is a board of messages you call from a terminal; start `bbs`,
call `port bbs0`, join with `NEW`, and press the letters in brackets.
