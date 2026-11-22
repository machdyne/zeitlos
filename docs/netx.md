# netx: the full net

`net`, the network driver in flash, is kept small: it has to fit the
flash archive beside everything else that starts without a card. `netx`
is the same program without that limit, on the SD card. A board with
`netx` on its card runs it instead of `net`, and it does everything
`net` does (sockets, TCP, SSH, telnet, TFTP, NFS, DNS, mDNS, NTP, the
servers) and more:

- **every network interface the board has, at once:**
  - the built-in Ethernet port, both on Klinge;
  - the USB-C socket, as a network to a PC ([usb_dev.md](usb_dev.md));
  - a USB Ethernet adapter ([usb_ethernet.md](usb_ethernet.md));
- **routing** between them, with static routes;
- **a gateway:** NAT, a DHCP server and a DNS forwarder, so that a
  network behind the board reaches the one in front with nothing set
  up on its hosts;
- **a firewall,** for the board itself and for what passes through it;
- **larger tables:** 32 ARP entries instead of 8, and 4 sockets instead
  of 2.

Programs do not see a difference: `netx` answers as `net0`, as `net`
does.

`netsettings` sets all of this up in a window ([netsettings.md](netsettings.md));
this page says what each setting means.

## When it runs

At start, `net` looks for `/apps/netx` on the card. If it is there,
`net` starts it and steps aside once `netx` has an interface up. If
`netx` is missing, cannot start, or finds no interface, `net` carries
on as always. So a board always has its network, card or not.

To keep flash `net` even with `netx` on the card, put this in
`/data/net/net.cfg`:

```
full = 0
```

A crash of `netx` is handled like one of `net`: the system starts the
network again (`net`, which hands over to `netx` again), up to three
times a minute.

## Interfaces

With nothing about interfaces in `net.cfg`, `netx` uses every interface
the board has:
- **The first** (the built-in port, or what `phy =` names) takes the
  usual keys (`dhcp`, `ip`, `mask`, `gw`), exactly as in `net`.
- **Each other one** (the USB-C device, a USB adapter that is plugged in
  at start) asks a DHCP server for its address.

To set them up yourself, give each one a name, `wan`, `lan` or `lan2`,
and its own keys:

```
wan.phy  = builtin            # the board's Ethernet port
wan.dhcp = 1                  # its address from the network's DHCP server

lan.phy  = usbdev             # the USB-C socket, to a PC
lan.ip   = 192.168.7.1/24     # address/prefix length, or with lan.mask
```

| Key | Means |
|---|---|
| `<if>.phy` | `builtin` (the board's MAC; Klinge's Ethernet A), `builtin2` (Klinge's Ethernet B), `usbdev` (the USB-C socket), `usb` (a USB Ethernet adapter), `xlink` (Sergei and Mozart: the carrier's USB port, through its RP2040; docs/xlink.md). Without it: `wan` is the built-in port, `lan` and `lan2` the second built-in port, then the USB-C socket, then the RP2040's link, then an adapter. |
| `<if>.dhcp` | `1`: ask a DHCP server; `0`: don't. The default is `1` unless `<if>.ip` is given. |
| `<if>.ip` | the address, `a.b.c.d/len` or `a.b.c.d` with `<if>.mask` |
| `<if>.mask` | the netmask, when `<if>.ip` has no `/len` |
| `<if>.gw` | the gateway: the default route, from the first interface that has one |

Each interface gets its own MAC address, except a USB adapter, which has
its own.

`netx` prints what it set up on the console:

```
netx: wan (rmii) up mac 02:00:00:00:00:01 192.168.178.40/255.255.255.0 gw 192.168.178.1
netx: lan (usbd) up mac 02:00:00:00:01:01 192.168.7.1/255.255.255.0
netx: forwarding on, 1 static route(s)
```

**The board's own services** (SSH, telnet, the servers) answer on every
interface. A connection to one of the board's addresses has to arrive on
that address's own interface; a ping is answered for any of them.

## Routing

Forwarding between the interfaces is off unless you turn it on:

```
forward = 1
route   = 10.9.0.0/16 via 192.168.7.20
```

| Key | Means |
|---|---|
| `forward` | `1`: packets for other hosts are passed between the interfaces |
| `route` | `net/len via gateway`: packets for that network go to that gateway, which must be on one of the interfaces' subnets. Up to 8; the longest prefix wins. |

Everything else goes to the default gateway. A forwarded packet leaves
with its TTL one less. Packets that cannot be forwarded get the ICMP
error the standard asks for, at most ten a second:
- TTL run out: "time exceeded";
- no route: "network unreachable";
- too large, with "don't fragment" set: "fragmentation needed".

Broadcasts and multicast are not forwarded.

With forwarding on, the hosts on each side need the board as their
gateway, or a route to the other side through it, unless the board is
their gateway (below).

## A gateway

A laptop on the USB-C cable gets onto the board's wired network, with
nothing to set up on the laptop:

```
wan.phy   = builtin          # the board's Ethernet port, to the router
wan.dhcp  = 1
wan.nat   = 1                # what leaves here leaves from the board's address

lan.phy   = usbdev           # the USB-C socket
lan.ip    = 192.168.7.1/24
lan.dhcpd = 1                # give the laptop an address
```

Swap `builtin` and `usbdev` for the other way round: the board gets the
PC's connection over USB-C and shares it on its Ethernet port.

| Key | Means |
|---|---|
| `<if>.nat` | `1`: masquerade. Connections from the other interfaces leave by this one from its address, and their answers come back to the hosts that made them. Turns forwarding on. |
| `<if>.dhcpd` | `1`: a DHCP server on this interface. It hands out addresses from its pool, with the board as the gateway and the nameserver. |
| `<if>.pool` | `a.b.c.d-a.b.c.e`: the addresses it hands out. Default: .100 to .199 of the interface's subnet. Never the board's own. |
| `<if>.lease` | seconds a lease lasts (default 3600) |
| `portfwd` | `tcp 2222 192.168.7.2:22`: a connection to that port of a NAT interface goes to that host and port (`udp` too). Up to 16. |
| `nat_size` | connections the NAT follows at once (default 256, 16 to 8192) |
| `dnsfwd` | `0`: no DNS forwarder; DHCP then hands out the board's own nameserver instead of the board |

**NAT** translates TCP, UDP and ping, with every checksum kept right.
- **Ports:** connections leave from ports 20000 to 29999, and anything
  else that arrives at the address is the board's own, as before.
- **Timeouts:** a connection is forgotten after a while with nothing
  sent:
  - TCP: 60 s until it is set up, then about two hours; 2 minutes after
    a FIN, 10 s after a reset;
  - UDP: 30 s, then 3 minutes once answered;
  - ping: a minute.
- **A full table** drops new connections until room comes free.
- **Not translated:** other protocols, and fragments after the first.
- **Not yet:** ICMP errors about a translated connection are not sent on
  to the host behind (traceroute through the board shows only the board).

**The DHCP server** keeps its leases in RAM. After a restart clients ask
again, and get their old address back if they ask for it and it is
free. It answers only on interfaces with `.dhcpd = 1`.

**The DNS forwarder** answers port 53 on those interfaces. It sends each
question on to the board's own nameserver (the one the wan side got, or
`dns =`) and returns the answer. It keeps no cache; the nameserver has
one. With no nameserver it answers SERVFAIL at once.

**Speed:** not yet measured. Every forwarded packet is copied once by
the CPU, so expect a few Mbit/s rather than the links' full speed.

## The firewall

As soon as an interface is a WAN (named `wan`, or with `.nat = 1`), what
comes in by it is filtered:

| From a WAN | Let in |
|---|---|
| to the board | ping, ICMP errors, and answers to the board's own connections; new connections only to the services in `<if>.allow` |
| through the board | only what NAT sends on: answers to connections the other side opened, and port forwards |

The other interfaces are not filtered, and a board with no WAN filters
nothing, unless the rules below say otherwise.

```
wan.allow = ssh, udp/5000, 8080
```

| `<if>.allow` | Lets in |
|---|---|
| `ssh`, `telnet`, `http`, `https` | that TCP port (22, 23, 80, 443) |
| `N`, `tcp/N` | TCP port N |
| `udp/N` | UDP port N |

**Rules,** in `/data/net/firewall.cfg`, come before all of that: the
first that matches a packet decides it.

```
# telnet only from the office
allow in=wan proto=tcp dport=23 src=198.51.100.0/24
# the second LAN may not reach the first
deny in=lan2 dst=192.168.7.0/24
# nobody on the LAN logs in to the board itself
deny in=lan to=board proto=tcp dport=22-23
```

| Part | Means |
|---|---|
| `allow`, `deny` | what a match does |
| `in=IF`, `out=IF` | the interface it arrives by, leaves by |
| `to=board`, `to=through` | only packets for the board, only packets passing through (neither: both) |
| `proto=` | `tcp`, `udp` or `icmp` |
| `src=`, `dst=` | `a.b.c.d` or `a.b.c.d/len` |
| `sport=`, `dport=` | `N` or `N-M` |

Up to 64 rules. A rule for an interface the board does not have never
matches. A file with a mistake is not used at all, and the console says
which line.

What is dropped goes to the console, five lines a second at most:

```
netx: fw: drop tcp 203.0.113.5:40000 -> 10.0.2.1:23 in wan (default)
```

**Limits.**
- Answers to the board's own UDP are let in for two minutes after it
  last sent.
- TCP that is not a new connection is left to the board's TCP, which
  refuses what it does not know.
- The rules do not see packets NAT sends on, so who may use a port
  forward cannot be limited yet.

## Klinge

Klinge has two Ethernet ports and a USB-C socket, and `netx` uses all
three: Ethernet A is `builtin`, Ethernet B `builtin2`, the USB-C socket
`usbdev`. As a router between two networks, with a laptop on USB-C
behind it as well:

```
wan.phy   = builtin          # Ethernet A, to the upstream router
wan.dhcp  = 1
wan.nat   = 1
wan.allow = ssh

lan.phy   = builtin2         # Ethernet B, a LAN of its own
lan.ip    = 192.168.10.1/24
lan.dhcpd = 1

lan2.phy  = usbdev           # a laptop on the USB-C cable
lan2.ip   = 192.168.7.1/24
lan2.dhcpd = 1
```

Flash `net` uses Ethernet A only. Ethernet B is a second MAC in the
gateware (`` `ETH_RMII_B `` at 0x6300_0000, FEATURES3 bit 5), on the same
50 MHz reference clock as A.

## Building

`sw/apps/netx` builds `sw/apps/net`'s sources with `-DNETX` (its Makefile
says how). The netx parts are under `#ifdef NETX`:
- **netif.c:** interfaces and routes;
- **netx.c:** start-up;
- **nat.c, dhcpd.c, dnsfwd.c:** the gateway;
- **fw.c:** the firewall;
- **rmii_eth_b.c:** Klinge's second MAC (rmii_eth.c again, at its own
  address);
- **forwarding in ip.c,** with ARP entries per interface in arp.c;
- **the new keys** in netcfg.c.

Flash `net` is unchanged apart from handing over.

Tests: `make -C sw/apps/net test` runs these with the rest:
- `tests/test_netx_fwd.c`: two interfaces on a wire, both hosts played
  frame by frame;
- `tests/test_netx_gw.c`: the gateway on the same wire. Every frame's
  addresses, ports and checksums are checked.
- `tests/test_netx_fw.c`: the firewall's defaults and rules, on that
  wire;
- `tests/test_netx_cfg.c`: the keys.

## Sergei and Mozart: the carrier's USB

On these boards (every module) netx has a fourth kind of port, `xlink`: the PC on the
carrier's USB port, through its RP2040 (docs/xlink.md).
