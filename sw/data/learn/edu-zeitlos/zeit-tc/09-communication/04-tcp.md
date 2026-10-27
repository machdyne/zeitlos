# TCP, UDP and applications

```lesson
(objective "Choose between UDP and TCP, say how ports tell programs apart, and read a simple text protocol.")
(minutes 20)
(keywords "UDP" "datagram" "TCP" "port number" "HTTP" "HTTPS" "Gopher" "Gemini" "IRC" "SMTP")
```

IP delivers packets on a best-effort basis: they may arrive out of
order, twice, or not at all. The transport layer decides what to do
about that.

**UDP** sends single messages, called **datagrams**, with no promises.
It is simple and quick, and it suits uses where a lost message does not
matter or will simply be asked for again: DNS lookups, setting the time,
live sound.

**TCP** builds a reliable, ordered stream of bytes on top of unreliable
packets. It numbers every byte it sends. The receiver acknowledges what
it has received, and the sender sends again anything not acknowledged
in time. The receiver puts everything back in order before handing it
to the program. To the program, a TCP connection is like a pair of
pipes, one each way: all the bytes come out, in order.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Transport: TCP and UDP")
```

```check
(choice
  (ask "You copy a file to another computer. Which transport suits it?")
  (right "TCP: every byte must arrive, in order")
  (wrong "UDP: it is quicker"
    "UDP is quicker, but makes no promises: a lost or reordered packet would spoil the copy. A file needs every byte, in order.")
  (hint "What happens to a file if a piece of it is lost?"))
```

```check
(choice
  (ask "A live voice call loses one small packet. Which transport would rather carry on than wait for it to be sent again?")
  (right "UDP")
  (wrong "TCP"
    "TCP waits for the lost bytes to be sent again before handing on anything after them. In a live call, a short gap is better than a delay.")
  (hint "Which one makes no promise to resend?"))
```

---

## Ports

Both TCP and UDP use **port numbers** to tell apart the programs on one
machine. A server for web pages listens on port 80 or 443, and an SSH server on
port 22. A connection is known by four numbers: the two IP addresses
and the two ports.

```check
(number
  (ask "Your computer opens two connections to the same server, port 443. Its own address is the same for both. How many of the four numbers must differ between the two connections?")
  (answer 1)
  (wrong-if 0 "Then the two connections would be the same connection. At least one number must differ.")
  (wrong-if 2 "Both addresses and the server's port are the same. Your computer's own port number is enough to tell them apart.")
  (hint "Three of the four numbers are the same. Which one is left?"))
```

---

## Plain-text protocols

The application layer gives the services that people and programs use.
Many of the oldest application protocols are plain text, which makes
them easy to understand and to test by hand. A web browser asking for a
page sends lines like these, ending with a blank line:

```text
GET /about.html HTTP/1.1
Host: example.com
```

The server answers with a status line, some headers, a blank line and
the page itself.

```sources
(source "Timeless Computing (docs/tc.md), Communication: Applications")
```

```check
(text
  (ask "In the request above, which file does the browser ask for? Type its path.")
  (accept "/about.html") (accept "about.html")
  (wrong-if "example.com" "That is the name of the machine, on the Host line. The path of the page is on the GET line.")
  (hint "Look at the first line, after GET."))
```

---

## Some protocols

- **Telnet** and **SSH** give a terminal on another machine. Telnet
  sends everything, passwords too, as plain text; SSH scrambles it so
  that only the two ends can read it.
- **HTTP** and **HTTPS** carry the web. Modern web pages are often too
  large for a small computer, but simple pages work well.
- **Gopher** and **Gemini** are much simpler than the web: text
  documents and menus, suited to small machines.
- **IRC** is a long-standing protocol for text chat in shared channels.
- Email is delivered by **SMTP**, and collected by POP or IMAP.

```check
(choice
  (ask "Why is Telnet a poor choice for logging in to a machine across the Internet?")
  (right "It sends everything, including the password, as plain text that any machine on the way can read")
  (wrong "It is too slow for a terminal"
    "Speed is not the trouble. Telnet sends the password as plain text, and packets pass through many machines.")
  (wrong "It only works over a serial cable"
    "Telnet works across networks. Its weakness is that it hides nothing.")
  (hint "What does Telnet do with what you type?"))
```

So: UDP sends quick datagrams, TCP a reliable stream, ports pick the
program, and many application protocols are plain text you can read.
