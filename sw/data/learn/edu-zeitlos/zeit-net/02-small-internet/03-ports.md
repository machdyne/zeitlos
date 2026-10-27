# Protocols and ports

```lesson
(objective "Say which port each of web's protocols uses, and read a URL that names a port.")
(minutes 15)
(keywords "port (network)")
```

This lesson is about how one machine can run several servers at once.

One computer can serve web pages, Gopher menus and a capsule all at
the same time. Each server program **listens** on a numbered **port**:
a number from 1 to 65535 that a connection asks for along with the IP
address. The IP address finds the machine; the port finds the program
on it.

Choose a scheme to see which port `web` asks for.

```figure
(define s (choice 'scheme '("http" "https" "gopher" "gemini") "Scheme"))
(define ports '("80" "443" "70" "1965"))
(define names '("web" "secure web" "Gopher" "Gemini"))
(zn-machine 6 85 70 50 "You" "web")
(frame 170 20 144 200 'ink 1.5)
(text 242 36 "203.0.113.7" 'centre 'small 'bold)
(for-range 0 4
  (lambda (k)
    (let ((y (+ 50 (* k 42))))
      (rect 170 y 46 32 (if (= k s) 'ink 'paper))
      (frame 170 y 46 32 'ink 1)
      (text 193 (+ y 21) (list-ref ports k) 'centre 'body (if (= k s) 'paper 'ink))
      (text 222 (+ y 21) (list-ref names k) 'small))))
(arrow 76 110 168 (+ 66 (* s 42)) 'ink 2)
(caption (string-append (list-ref names s) ": port " (list-ref ports s)))
```

```check
(number
  (ask "Which port does web connect to for a Gemini capsule?")
  (answer 1965)
  (wrong-if 443 "443 is https. Gemini has a port of its own.")
  (wrong-if 70 "70 is Gopher's port.")
  (hint "Set the figure's scheme to gemini."))
```

---

## The usual ports

Each protocol has a usual port, and a URL leaves it out when it is the
usual one:

| Scheme | Port |
|---|---|
| http | 80 |
| https | 443 |
| gopher | 70 |
| gemini | 1965 |

```sources
(source "Zeitlos docs: gopher_gemini.md (ports 70 and 1965); sw/apps/web/url.c (default ports)")
(source "Timeless Computing (docs/tc.md), Communication: Transport")
```

```check
(number
  (ask "A URL starts with https:// and names no port. Which port does web connect to?")
  (answer 443)
  (wrong-if 80 "80 is plain http. The secure web has its own usual port.")
  (hint "Look at the table."))
```

---

## A URL with a port

A server can listen on another port. Then the URL names it after the
host name, with a colon:

```
http://example.org:8080/news.html
```

That is the page `/news.html`, from the server listening on port 8080
of `example.org`, over http.

```example
(example
  (problem "Which port does web use for gopher://example.org:7070/1/phlog?")
  (step "The scheme is gopher, whose usual port is 70.")
  (step "But the URL names a port after the host name: :7070.")
  (step "A port named in the URL wins over the usual one.")
  (answer "Port 7070."))
```

```check
(number
  (ask "Which port does web use for http://example.org:8080/news.html?")
  (answer 8080)
  (wrong-if 80 "80 is the usual port for http, but this URL names another one after the colon.")
  (hint "Look after the host name's colon."))
```

---

## Two machines, two numbers each

A connection is fully named by four numbers: the two machines' IP
addresses, and a port at each end. Your end uses a port too, picked
for that one connection, so the answers find their way back to the
right program on your computer.

```check
(choice
  (ask "Why does a connection need a port as well as an IP address?")
  (right "The IP address finds the machine; the port finds the program on it")
  (wrong "The port is a second, backup address for the machine"
    "A machine has one IP address on its network. Its ports are numbered places on it where programs listen.")
  (wrong "The port says how fast to send"
    "A port is only a number that picks a program. Speed is another matter.")
  (hint "One machine can run several servers at once."))
```

---

```check
(choice
  (ask "Which URL fetches a Gopher menu from port 70?")
  (right "gopher://example.org/1/")
  (wrong "gopher://example.org:1965/1/"
    "1965 is Gemini's port, and it is written in this URL, so web would ask port 1965 for a Gopher menu.")
  (wrong "http://example.org:70/"
    "That asks port 70 for a web page, using http. The scheme must be gopher.")
  (hint "A Gopher URL with no port uses Gopher's usual one."))
```

So: the IP address finds the machine and the port finds the program;
each protocol has its usual port, and a URL can name another after a
colon.
