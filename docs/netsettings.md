# netsettings: network settings

`netsettings` sets up `netx`, the full network program ([netx.md](netx.md)):
which ports the board uses, their addresses, whether it shares its
connection, and what it lets in. It edits `/data/net/net.cfg`.

```
 Network settings
 Upstream (wan)
   port                     Ethernet
   address                  from DHCP
   gateway                  -
   share it (NAT)           on
   hand out addresses       off
   let in                   ssh
 Local network (lan)
   port                     USB-C, to a PC
   address                  192.168.7.1/24
   ...
 The network
   network program          netx, if on the card
   forward between ports    off (NAT turns it on)
   answer DNS for the LAN   on
   nameserver               from DHCP
 [ Save ] [ Apply ] [ Firewall rules ] [ Close ]
```

**Up** and **Down** choose a setting; **Enter**, **Space** or a click
changes it:
- a switch flips;
- the port moves on to the next one: not used, Ethernet, Ethernet B
  (Klinge), USB-C to a PC, a USB Ethernet adapter;
- an address or a list asks for the new value.

Turning a port on gives it a start:
- **the upstream:** asks for its address by DHCP;
- **a local network:** gets 192.168.7.1/24 (the second one
  192.168.8.1/24) and hands out addresses.

| Button | Does |
|---|---|
| Save (Ctrl+S) | writes `net.cfg` |
| Apply | saves, and starts the network again so it uses the new settings. Open connections, SSH among them, close. |
| Firewall rules | opens `/data/net/firewall.cfg` in the text editor, from a commented template the first time. Rules take effect when the network starts again. |
| Close (Esc) | asks first if there is something not saved |

Only what the window shows is changed. Everything else in `net.cfg`
stays as you wrote it: comments, WiFi networks, routes, port forwards,
and keys `netsettings` does not know. A new setting goes at the end of
the file.

What each setting means, and the ones only `net.cfg` has (routes, port
forwards, pools, leases), are in [netx.md](netx.md).

**Tests:** `make -C sw/apps/netsettings test` edits a hand-written file
and checks that netx's own parser reads the result as intended.
