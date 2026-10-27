# Your computer as a server

```lesson
(objective "Say what netserve does, how it is switched on, and which machines can reach it.")
(minutes 15)
(keywords "netserve" "subnet" "Ethernet")
```

This lesson is about turning the tables: other machines connecting to
yours.

So far your computer has been the client. With **netserve**, Zeitlos's
server app, it can be a server too: it can serve files over the web,
and let you log in from another machine with telnet or SSH.

Everything in netserve is **off** until you switch it on, in
`/sys/zeitlos.cfg`. A line for each service: the service, then its
port, then what it serves. For example, files:

```
apps.netserve.http: 80 /data/netserve/www
```

When a service is set, netserve starts by itself when the computer
starts ([netserve](docs:netserve)).

```sources
(source "Zeitlos docs: netserve.md, Configuration; config.md")
```

```check
(choice
  (ask "You have just installed Zeitlos. Can another machine log in to it over the network?")
  (right "No: every netserve service is off until you set it")
  (wrong "Yes, by telnet, with no password"
    "That would be dangerous, and it is not so: nothing is served until you switch it on, and telnet will not start without a password.")
  (wrong "Yes, by SSH, as on most servers"
    "Many servers come with SSH on. Zeitlos does not: every netserve service is off until set.")
  (hint "What do the settings say until you change them?"))
```

---

## Who may connect

Out of the box netserve accepts connections only from your **subnet**:
the machines on your own local network, the ones your computer can
reach directly without passing through a router. A machine elsewhere on
the internet is refused.

```
apps.netserve.allow: subnet
```

`any` instead of `subnet` lets in connections from anywhere. Do that
only for something meant for the public, and only once you understand
what it opens.

```check
(choice
  (ask "With the settings netserve comes with, which machine can reach your computer's services?")
  (right "A laptop on the same home network")
  (wrong "A friend's computer in another town, over the internet"
    "That machine is not on your subnet, and netserve accepts only your subnet unless you say any.")
  (wrong "No machine at all"
    "Once a service is on, machines on your own subnet can reach it.")
  (hint "subnet: your own local network."))
```

---

## Boards with Ethernet only

On a board whose network is a WiFi module through the ESP32 link (the
ULX3S, or Kabellos), the module stands between Zeitlos and your
network, as a small router does: Zeitlos can reach out to anything,
but nothing on your network can open a connection *to* it. netserve
starts there, and nobody reaches it. Serving needs a board with
**Ethernet**, the usual wired network: a cable, from a socket built in
or from an adapter.

```sources
(source "Zeitlos docs: netserve.md (Ethernet boards only); kabellos.md, Addresses")
```

```check
(choice
  (ask "Your board is on WiFi through a Kabellos module. You switch on netserve's file server. Can your laptop fetch the files?")
  (right "No: through the ESP32 link nothing can connect in")
  (wrong "Yes, from the same WiFi"
    "The module hides Zeitlos behind its own address, as a home router hides a PC: connections can go out, not in.")
  (hint "Which way can connections go through the module?"))
```

---

## Your computer's address

Other machines need your computer's IP address. `net` says it on the
console when it starts, in a line like this:

```
net: ip 192.168.1.50/255.255.255.0, listening (...)
```

In `term`, the **CONSOLE** button shows that log. The second number,
after the `/`, says how big your subnet is: here, every address that
starts `192.168.1.`.

```check
(text
  (ask "The console says: net: ip 10.0.0.23/255.255.255.0. What is this computer's IP address?")
  (accept "10.0.0.23")
  (wrong-if "255.255.255.0" "That is the second number: the size of the subnet. The address comes first.")
  (hint "The four numbers right after ip."))
```

---

```check
(text
  (ask "Which word in apps.netserve.allow lets in connections from anywhere?")
  (accept "any")
  (wrong-if "subnet" "subnet is the careful setting, the one it comes with: only your own network.")
  (hint "The other of the two words."))
```

So: netserve makes your computer a server; each service is off until
set in `/sys/zeitlos.cfg`, only your subnet may connect, and only a
board with Ethernet can be reached.
