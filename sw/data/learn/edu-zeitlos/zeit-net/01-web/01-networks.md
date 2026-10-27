# Networks and addresses

```lesson
(objective "Say what a network, a server, a client, an IP address and a host name are, and how Zeitlos gets on a network.")
(minutes 15)
(keywords "network (of computers)" "server (network)" "client (network)" "protocol (network)" "IP address" "IP" "Internet Protocol" "host name" "DNS" "router")
```

This lesson is about how your computer talks to other computers.

A **network** is computers joined so that they can send each other
data: by cable, by radio (WiFi), or both. The internet is the largest
network of all, made of millions of smaller networks joined together.

Most of what you do on a network is one computer asking another for
something. The one that answers is a **server**: it waits for requests
and serves them. The one that asks is a **client**. When you read a
page with Zeitlos's `web`, your computer is the client.

```sources
(source "Timeless Computing (docs/tc.md), Communication")
```

```check
(choice
  (ask "You open a page in web. Which computer is the server?")
  (right "The computer that sends the page back")
  (wrong "Your Zeitlos computer, because it runs web"
    "Your computer asks for the page, so it is the client. The server is the one that waits for requests and answers them.")
  (wrong "Whichever computer is bigger"
    "Size has nothing to do with it. A small board can be a server: the server is simply the one that answers.")
  (hint "Who asks, and who answers?"))
```

---

## Addresses

To send a message, a computer must say where it goes. Every machine on
a network has an **IP address**: four numbers from 0 to 255, written
with dots between them, such as `192.168.1.20`.

**IP** is short for **Internet Protocol**. A **protocol** is a set of rules
that two computers agree on, so each understands what the other sends:
what to say first, how to ask, how to answer.

```check
(choice
  (ask "Which of these could be an IP address?")
  (right "10.0.0.5")
  (wrong "10.0.300.5"
    "Each of the four numbers is from 0 to 255, and 300 is too big.")
  (wrong "10.0.5"
    "An IP address of this kind has four numbers, and this has only three.")
  (wrong "ten.zero.zero.five"
    "The parts are numbers, written as digits.")
  (hint "Count the numbers, and check each is from 0 to 255."))
```

---

## Names

Numbers are hard to remember, so machines on the internet also have
names, such as `example.org`. A machine's name is its **host name**.

Before your computer can reach `example.org`, it must find the IP
address that goes with the name. It asks a name server, using **DNS**,
the Domain Name System: a worldwide system of servers that turn names
into IP addresses.

Change the step to follow a page being fetched.

```figure
(define s (choice 'step '("1" "2" "3" "4") "Step"))
(zn-machine 10 90 90 60 "You" "client")
(zn-machine 220 20 90 50 "DNS" "name server")
(zn-machine 220 160 90 60 "Server" "203.0.113.7")
(cond
  ((= s 0) (zn-message 100 100 220 50 "example.org?")
           (caption "1. Your computer asks DNS for the name's address."))
  ((= s 1) (zn-message 220 60 100 115 "203.0.113.7")
           (caption "2. DNS answers with the IP address."))
  ((= s 2) (zn-message 100 125 220 180 "send me the page")
           (caption "3. Your computer asks that address for the page."))
  (else    (zn-message 220 200 100 140 "the page")
           (caption "4. The server sends the page back.")))
```

```check
(choice
  (ask "What does DNS do?")
  (right "It turns a host name into an IP address")
  (wrong "It sends you the page"
    "The page comes from the server you asked for it. DNS only tells your computer where that server is.")
  (wrong "It gives your own computer its address"
    "Your computer gets its own address another way (next step). DNS looks up the addresses of other machines by name.")
  (hint "Look at steps 1 and 2 of the figure."))
```

---

## How Zeitlos gets on a network

On Zeitlos, one program does all of this: `net`. It starts when the
computer starts. It finds the network hardware the board has, and it
asks the local network for an IP address for this computer. At home,
the one that answers is usually your **router**, the box that joins
your home network to the internet. That needs nothing from you.

Its settings are in the file `/data/net/net.cfg`. Every line in it
comes commented out, so it changes nothing until you edit it: you need
it only for a network that wants fixed addresses, or for a WiFi
module: the name and password of your WiFi, and the fixed addresses
the module needs ([networking](docs:networking)).

```sources
(source "Zeitlos docs: networking.md (net.cfg); boot.md (init starts net)")
```

```check
(choice
  (ask "You plug a new Zeitlos computer into your home router with a cable. What must you set before it can use the network?")
  (right "Usually nothing: net asks the network for an address")
  (wrong "Its IP address, in /data/net/net.cfg"
    "You can set one there, but you need not: with no settings, net asks the local network for an address itself.")
  (wrong "The address of every server you will visit"
    "Those are looked up by name when you need them, with DNS.")
  (hint "What does net do when it starts?"))
```

---

```check
(choice
  (ask "A protocol is ...")
  (right "rules two computers agree on for talking to each other")
  (wrong "a cable between two computers"
    "A cable carries the data. A protocol is the agreement about what the data means and in what order it comes.")
  (wrong "a kind of computer that answers requests"
    "That is a server. A protocol is the set of rules client and server both follow.")
  (hint "IP is the Internet Protocol: the rules for addressing messages."))
```

---

```check
(text
  (ask "A computer that waits for requests and answers them is called a ...")
  (accept "server") (accept "a server")
  (wrong-if "client" "The client is the one that asks. The one that answers is the other kind.")
  (hint "It serves requests."))
```

So: clients ask, servers answer, every machine has an IP address, and
DNS finds the address that goes with a name.
