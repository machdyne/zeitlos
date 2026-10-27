# Secure links, and networks without the Internet

```lesson
(objective "Say what a secure connection protects against, and name ways to communicate when there is no Internet.")
(minutes 15)
(keywords "cryptography" "encrypt" "encryption" "TLS" "store and forward" "sneakernet")
```

Packets pass through many machines on their way, and any of them could
read or change the data. Secure protocols guard against this with
**cryptography**, the science of secret codes, which the Security unit
explains. To **encrypt** data is to scramble it with a secret key, so
that only someone who holds the key can read it; that scrambling is
**encryption**.

When a browser connects to an HTTPS site, or a terminal to an SSH
server, the client (the side that connects) first checks the server's
identity, and sometimes the server checks the client's. The two then
agree on a secret key over the open network, and encrypt the rest of
the session with it. The protocols are **TLS** and SSH: standard, open,
and on nearly every networked computer.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Secure Connections")
```

```check
(choice
  (ask "Why does the browser check the server's identity before agreeing on a key?")
  (right "Otherwise it might agree a key with an impostor, who could then read everything")
  (wrong "To make the connection faster"
    "Checking costs time. It is done so that the key is shared with the real server and nobody else.")
  (wrong "There is no need: encryption alone is enough"
    "Encryption with a key shared with the wrong machine protects nothing. The browser must know whom it is talking to.")
  (hint "What good is a secret shared with the wrong person?"))
```

---

## Without the Internet

The Internet is convenient, but it is not the only way to communicate,
and it may not always be there.

- A **local network** of a switch and a few cables, or a single serial
  link, needs nothing from outside.
- **Radio meshes** carry short messages across a town or a valley with
  nothing else built.
- **Store and forward** systems pass messages on whenever a link is
  there. Bulletin boards once swapped mail this way, calling each other
  at night when calls cost less.
- Data can be carried by hand on storage devices: **sneakernet**. It
  can still move large amounts faster than most networks.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Networks Without the Internet")
```

```check
(choice
  (ask "You must move 500 GB to a house across town, where the network is slow. What is likely fastest?")
  (right "Copy it to a drive and carry it over")
  (wrong "Send it over the slow network"
    "A slow link may take days for 500 GB. A drive carried across town takes an hour: sneakernet.")
  (wrong "Send it as email"
    "Email goes over the same slow network, and is not made for such large amounts.")
  (hint "How long does a drive take to cross town?"))
```

---

```check
(choice
  (ask "Messages must reach a farm in a valley with no Internet and no telephone. Which way fits best?")
  (right "A mesh of low-power radios, each passing messages on")
  (wrong "A server for web pages on the farm"
    "A server still needs a network to reach it. A radio mesh is the network, with nothing else built.")
  (wrong "An SSH connection"
    "SSH runs over a network. The question is how to have a network at all.")
  (hint "Which way needs nothing built but the radios themselves?"))
```

A timeless computer should be useful with no network at all, and able
to use whatever network there is: plain text, simple protocols and open
standards make both possible.
