# Logging in with SSH

```lesson
(objective "Log in to another machine with SSH from term, check its host key, and leave.")
(minutes 20)
(keywords "host key" "user name")
```

This lesson is about using another computer from yours, through `term`.

Many computers, servers above all, are used from afar: you log in to
them and get a shell, as if you were sitting at them. SSH does this
encrypted. In `term`, type on the panel (press F12 first if the window
is connected):

```
ssh me@192.168.1.10
```

`me` is your **user name** on that machine, the name you log in as;
after the `@` comes its IP address or host name. Leave out `me@` and
SSH asks `login as:` instead.

```sources
(source "Zeitlos docs: ssh.md; connections.md")
```

```check
(text
  (ask "Write what you type in term's Open bar to log in as anna to the machine 10.0.0.5.")
  (accept "ssh anna@10.0.0.5")
  (wrong-if "ssh 10.0.0.5@anna" "The user name comes first, then @, then the machine.")
  (wrong-if "telnet anna@10.0.0.5" "telnet takes no user name and is not encrypted. The word for this is ssh.")
  (hint "ssh, a space, the user name, @, the address."))
```

---

## Is it the right machine?

Before anything secret is sent, the machine shows its **host key**: its
public key, the one that proves who it is. You see its fingerprint:

```
Host key fingerprint:
  SHA256:4ADgAqIUtDClAPaRQO8ax85ZPlUk7o/vfGoisvYx7JQ
Compare this against the server before accepting.
Accept this host key (yes/no)?
```

Type `yes` or `no` in full; a single letter is not taken, so that
nobody says yes to a question they did not read. Zeitlos keeps no list
of machines it has met yet, so it asks every time.

```check
(choice
  (ask "SSH shows a host key fingerprint and asks yes/no. What should you do before typing yes?")
  (right "Compare the fingerprint with the one the machine's owner says it has")
  (wrong "Nothing: SSH has already checked it"
    "SSH can check that the machine holds that key's private key, but not that the key is the right machine's. Only you can, by comparing.")
  (wrong "Type y, to save time"
    "A single letter is not accepted, on purpose: the answer is yes or no, in full.")
  (hint "The screen tells you: compare this against the server."))
```

---

## Why compare?

A machine between you and the server could answer in its place, with a
key of its own, and pass everything on while reading it. Its
fingerprint would differ from the real server's. Comparing the
fingerprint is the one thing that catches it.

Where do you find the right one? From the machine's owner, or on the
machine itself. A Zeitlos computer serving SSH prints its own on its
console at every start, in a line with `ssh host key SHA256:...`.

```check
(choice
  (ask "The fingerprint shown is different from the one the server's owner gave you. What do you answer?")
  (right "no")
  (wrong "yes, since the address is right"
    "The right address does not prove the right machine: something on the way could be answering for it. A wrong fingerprint means stop.")
  (hint "A different key is a different machine."))
```

---

## In, and out

Then comes `Password:`. What you type is not shown. Once in, you are
at the other machine's shell: everything you type goes to it.

To leave, type the shell's own command (often `exit`), or press **F12**,
which disconnects from anywhere. Either way the panel comes back.

`term`'s `ssh` asks for a password each time: it cannot log in with
keys of your own yet.

```check
(choice
  (ask "You are logged in to another machine with SSH. You type ls and press Enter. Where does ls run?")
  (right "On the other machine")
  (wrong "On your Zeitlos computer"
    "Everything you type in this window goes to the other machine's shell. Your term only shows the answer.")
  (hint "Whose shell are you talking to?"))
```

---

```check
(choice
  (ask "SSH asks: Accept this host key (yes/no)? and you type y. What happens?")
  (right "It asks again, for exactly yes or no")
  (wrong "It accepts the key"
    "A one-letter answer is not taken: it asks you to type exactly yes or no.")
  (wrong "It disconnects"
    "It does not give up on you: it asks again.")
  (hint "Is y the whole word?"))
```

So: `ssh user@host` in term, compare the fingerprint before `yes`, type
the password, and F12 or `exit` to leave.
