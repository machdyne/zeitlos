# USB Ethernet (CDC-ECM)

A USB ethernet adapter as a `net` backend, alongside the three MACs in
[networking.md](networking.md). The class driver is in the `usb`
process (`sw/apps/usb/usbh_ecm.c`; the kernel's until Phase 3, see
[usb_driver.md](usb_driver.md)), on top of the USB host controller
described in [usb_host.md](usb_host.md). `net` reaches it through two
rings shared with `usb`.

**Status:** works in co-simulation against the real gateware
(`make test_usb_ecm`), including enumeration, every frame length that
matters in both directions, faults mid-frame, link notifications, and
unplug and replug. Tested on hardware.

## Contents

- [Why ECM](#why-ecm)
- [Choosing the configuration](#choosing-the-configuration)
- [Enumeration](#enumeration)
- [The MAC address](#the-mac-address)
- [Framing and the packet buffer](#framing-and-the-packet-buffer)
- [Polling, and what it costs](#polling-and-what-it-costs)
- [The rings](#the-rings)
- [net's side](#nets-side)
- [Configuration](#configuration)
- [Throughput](#throughput)
- [Testing](#testing)
- [What to check first on hardware](#what-to-check-first-on-hardware)
- [Deliberately not done](#deliberately-not-done)

---

## Why ECM

There are four standard USB networking classes, and one vendor protocol
per chip family behind them.

**CDC-ECM** (Ethernet Control Model) is the simplest: one raw Ethernet
frame per bulk transfer, ended by a short packet. No header, no length
field, no aggregation. It is what macOS drives most wired adapters
with, what a Linux USB gadget offers by default (a Pi Zero in gadget
mode, for example), and what the very common Realtek RTL8152/8153
adapters offer as an *alternate configuration* beside Realtek's own.

**CDC-NCM** (2010) is its successor and packs several frames into one
transfer to cut per-transfer overhead. That matters at 480 Mbps and
hardly at all at 12 Mbps, so it is a reasonable second driver and not a
first one. Newer Android tethering, Apple devices and most 2.5G USB-C
adapters use it.

**RNDIS** is Microsoft's, is being deprecated at both ends, and is not
worth the code. **EEM** is rare.

What ECM does not cover is the vendor-only chips: ASIX AX88772/AX88179,
and Realtek's own mode, which is what Linux's `r8152` speaks. That is
the same policy `usb_host.md` records for CDC-ACM against FTDI and
CH340: the standard class first, a vendor driver only if a specific
device turns out to matter.

## Choosing the configuration

A device may offer several configurations and the host picks one. Until
this work, `usbh.c` always fetched configuration index 0 and set it.
That is wrong for exactly the adapters worth supporting: an RTL8152
offers configuration 1 as Realtek's vendor protocol (interface class
0xff) and configuration 2 as CDC-ECM, in that order. On Linux the
generic chooser avoids a configuration whose first interface is
vendor-specific, then `r8152-cfgselector` deliberately switches back to
the vendor one, which is why plugging one in produces both a
`cdc_ether` and an `r8152` line in `dmesg`.

`usbh.c` now uses the same rule as Linux's chooser: **if the first
interface of the configuration just read is vendor-specific and the
device has another configuration, read the next one instead.** A device
with one configuration, or whose first is a real class, is unaffected.
`lsusb` shows which one was taken (`config value 2`).

This is deliberately a property of the *configuration*, not a check for
"can I drive it": a driver-aware search would have to fetch every
configuration and then go back for the winner, which costs code and
control transfers for a case that does not occur. If a device ever
turns up whose second configuration is also vendor-specific and whose
first is the useful one, this is the rule to revisit.

## Enumeration

After `SET_CONFIGURATION`, and after the HID and CDC-ACM probes have
declined, `usbh.c` asks `usbh_ecm.c` to probe. An ECM function is a
communications interface (class 2, subclass 6) and a data interface
whose *alternate setting* carries bulk IN and OUT. Three states follow,
the same shape as the CDC-ACM pair:

| State | Request | On failure |
|---|---|---|
| `ecm-mac` | `GET_DESCRIPTOR(STRING, iMACAddress)`, language 0x0409 | not fatal: the adapter is run promiscuous instead |
| `ecm-set-interface` | `SET_INTERFACE(data interface, alt with endpoints)` | **fatal** |
| `ecm-filter` | `SET_ETHERNET_PACKET_FILTER` | not fatal: a STALL does not stop the bind |

The `SET_INTERFACE` is the one that must work. An ECM data interface
has alternate setting 0 with **no endpoints at all** — that is how the
class turns the data path off — and only alternate setting 1 has the
bulk pair. Skip it and there is no data path; the symptom would be an
adapter that enumerates perfectly and then STALLs or ignores every
transfer.

The filter is set to directed plus broadcast, or promiscuous as
described below. Multicast is not requested: nothing in `net` uses it,
and on a LAN with no IGMP snooping it is a steady stream of frames to
drop.

One adapter at a time. A second one enumerates and sits idle, exactly
as a second CDC-ACM device does.

## The MAC address

An ECM adapter has a real, burned-in address, reported as the
`iMACAddress` string (twelve hex digits in UTF-16), and its receive
filter passes unicast frames **for that address only**. There is no
register to change it. So `net` adopts the adapter's address rather
than using the locally-administered `02:00:00:00:00:01` it uses on the
three MACs — which also fixes a real problem that address has: two
Zeitlos boxes on one LAN collide.

That leaves the case of an adapter swapped while `net` is running.
`net` has a DHCP lease and every peer on the LAN has an ARP entry, both
tied to the first adapter's address, and changing address means
throwing all of that away. Instead:

- `net` tells `usb` which address it sends from
  (`Z_USBNET_OPEN`, called once at startup);
- when an adapter binds whose own address differs from that one — or
  which reported no address at all — `usb` adds `PROMISCUOUS` to
  the packet filter.

So the replacement adapter passes frames addressed to the *first*
one, `net` keeps its address, its lease and its peers' ARP entries, and
nothing has to restart. `lsusb` and `ic` both show when an adapter is
promiscuous. The cost is the traffic a switch floods to it, which on a
switched LAN is broadcasts and not much else.

This is the one behaviour the co-simulation checks that cannot be
checked with a single adapter on the bench: it needs two.

## Framing and the packet buffer

Each transfer is one frame with no FCS, ended by a short packet. A
frame whose length is an exact multiple of the endpoint's 64 bytes is
therefore followed by a **zero-length packet**; without it the adapter
would take the next frame as a continuation of this one. Both
directions handle that, and the co-simulation covers 64, 128, 512, 1024
and 1472-byte frames for exactly this reason.

A frame is up to 1514 bytes and the packet buffer is 2 KB with no free
region that size (`usb_host.md`, [The packet
buffer](usb_host.md#the-packet-buffer)). So both directions move a
frame in **512-byte `auto_cont` chunks through the mass storage sector
area at 0x000**, copying each chunk out or in before the next.

That sharing is safe, and the reason is worth stating plainly because
it is the kind of thing that is only obvious until it is not:

- mass storage and this driver both run in the `usb` process, one
  request or one frame at a time, to its end;
- a poll of the stack never touches it.

So neither can start while the other has data half-copied. (Until
Phase 3 the same was true because both ran with the scheduler held.)
The transaction engine itself is held across a whole frame with
`z_usbh_bus_reserve()`, as a whole SCSI command is.

A NAK, a bad CRC or the end-of-frame guard all stop the engine without
advancing past the packets it acknowledged, so the driver takes what
landed and asks for the rest — the same resume `usbh_msc.c` does, and
the reason a frame that pauses mid-flight is not lost.

Copies use word accesses, a quarter of the bus cycles of the byte-wise
copy the storage driver uses.

Frames longer than 1536 bytes are drained to their short packet and
dropped, so that one oversize frame does not turn into two bogus ones.
A frame too long for a ring slot is dropped the same way: the slot is
never written past its length, and `rx_drop` counts it.

## Polling, and what it costs

There is no receive interrupt. Something has to ask.

Since Phase 3 the asking is `usb`'s, not `net`'s: `usb` asks the
adapter, puts frames in its ring, and wakes `net` when the ring stops
being empty, so `net` sleeps until a frame is there. The pace below is
the same; it is just a different process that wakes for it.

`usb` asks **every tick while traffic has moved in the last half
second, and every 16 ticks (about 22 ms) when it has not**. An idle link therefore costs
about 45 wakeups a second, against the 10 a second a wired MAC's
backstop costs and the 732 `docs/networking.md` records as expensive
enough to take a scheduler share from whatever is painting the screen.
The latency cost is bounded by the same 22 ms.

**"Traffic" means frames addressed to us** (and anything we send), not
broadcasts or multicasts. Counting every received frame kept the fast
rate on almost permanently on an ordinary LAN, where ARP for other
hosts and similar chatter arrives every few hundred milliseconds:
`ipcbench` [7] (docs/userspace_drivers.md) measured `net` taking most of
a timeslice ~230 times a second on an idle desktop. The destination is
compared with our own address rather than by the group bit, since an
adapter in promiscuous mode also passes other hosts' unicast.

**Nor does answering ARP.** Everything we send used to count, and a
host on the LAN asking "who has us?" every few hundred milliseconds
got an ARP reply each time, which restarted the half-second busy hold
before it could run out. CPU accounting (round 7d) measured `net` at
~28% of the CPU on an idle desktop, polling every tick. Tick billing
had shown it as nearly idle, because the polls run between ticks. An
ARP frame no longer counts as traffic. If a conversation follows, its
first unicast frame to us starts the fast rate then.

An idle poll is cheap on the bus as well as in `usb`: one IN
transaction that the adapter NAKs, with the hardware's NAK budget set
to 0 so it comes straight back.

Nothing is dropped while we are not asking. A USB device that has a
frame and no one to give it to does not discard it — it NAKs, and the
frame waits in its own buffer. That is a real advantage over a MAC with
a fixed ring, and it is why this backend's `rx_capacity` could rise
once there is a measurement to justify it.

The idle receive is also where the notification endpoint gets read,
about four times a second, for `NETWORK_CONNECTION` (link up or down).
Nothing acts on link state yet; `ic` and `lsusb` report it.

## The rings

`Z_SYS_USBNET` (an op field: info, receive, send, open) was the way in
until Phase 3; its slot remains and fails. Now `NET_OPEN` sets up two
rings ([sw/common/zusbnet.h](../sw/common/zusbnet.h) has the rules):

| | in | written by | holds |
|---|---|---|---|
| `z_usbnet_srv_t` | `usb` | `usb` | received frames (2 slots), how far it has sent from `net`'s ring, and the adapter's info: presence, generation, MAC, link, promiscuous flag, `wMaxSegmentSize`, counters |
| `z_usbnet_cli_t` | `net` | `net` | frames to send (2 slots), how far it has read `usb`'s ring |

- **Reading the other side's ring.** Each reads the other's directly:
  `NET_OPEN` hands `usb` net's ring as a blob, and its reply is `usb`'s
  ring.
- **Wakes.** Messages pass only when a ring goes from empty to not:
  `Z_USB_EVENT` to `net`, `Z_USB_KICK` to `usb`.
- **Replugs.** The generation in the info moves on every bind and
  unbind, which is how `net` notices a replug, by reading memory.
- **A send no longer reports failure.** It returns as soon as the
  frame is in the ring; a full ring drops it, as a MAC with a full
  transmit buffer would, and TCP sends it again. The adapter's own
  failures are in `tx_err`.

## net's side

`sw/apps/net/usb_ecm.c`, a fourth `net_phy_t` backend beside
`enc28j60`, `rmii` and `esp32link`. Two things about it are new to that
interface:

- **`get_mac`**, a new optional hook: a NIC with an address of its own
  says so, and `net` adopts it. `our_mac` in `net.c` is no longer
  `const`. The three MAC backends leave it alone.
- **`init` waits.** On a board whose only NIC is a USB adapter, `net`
  starting before anything is plugged in is normal, and there is
  nothing else that would restart it when an adapter appears. So it
  prints one line and asks twice a second until `usb` is running and
  has one.

  The consequence to know about: `net` registers its name before this,
  so a process that messages `net0` while it is waiting will wait for
  its own timeout rather than failing at once. Set `phy=builtin` in
  `net.cfg` to get the old behaviour of exiting cleanly instead.

Unplug mid-session needs no handling beyond this: sends fail, receives
return nothing, and `net` logs the event. A replug resumes. `ic` on the
serial console dumps `usb`'s counters next to `net`'s own, which
is what to look at if frames appear to be going missing between them.

## Configuration

`net.cfg` gains one key:

```
phy = auto | usb | builtin
```

- **`auto`** (the default): a MAC built into the bitstream if there is
  one, otherwise a USB adapter if this build has the USB host
  controller.
- **`usb`**: a USB adapter even on a board that has a MAC.
- **`builtin`**: never USB. On a board with no MAC, `net` exits
  cleanly, as it did before this existed.

## Throughput

Not yet measured. What is known:

- Full speed caps the wire at about 1 MB/s, and a 1514-byte frame is
  24 packets of 64 bytes.
- `networking.md`'s measurements show `net`'s own TCP — one
  connection, a small window, no reassembly — bounding bulk transfer
  well below what the ENC28J60's SPI link can carry, so the link is
  unlikely to be the limit here either.
- The expectation is ENC28J60 territory, far below RMII.

Measure with `netprof` (`make NET_PROFILE=1` in `sw/apps/net`) before
changing anything on the strength of an argument.

## Testing

`make test_usb_ecm` — the real driver, the real gateware, and a CDC-ECM
adapter model (`rtl/tb/tb_usb_device.v` with `ECM=1`) shaped like the
RTL8152: two configurations with the vendor one first, the MAC string,
alternate settings 0 and 1, an interrupt notification endpoint, and
zero-length packets. A low-speed mouse polls on the other port
throughout, so auto-poll traffic lands between the bulk transactions.

What it checks:

- configuration 2 chosen over the vendor one; MAC read; alternate
  setting 1 selected; filter directed plus broadcast;
- receive of 60, 64, 100, 128, 511, 512, 513, 1024, 1472 and 1514-byte
  frames, byte for byte, and a burst of five;
- NAKs mid-frame past the hardware budget; bad CRCs mid-frame;
- an oversize frame dropped with the next one intact; a frame too big
  for the caller dropped with nothing written past its buffer;
- send of the same lengths, checked by the model, including the
  zero-length packets, and an adapter NAKing while its buffer is full;
- link notifications up and down;
- unplug while idle and unplug during a burst with interrupts
  emulated; replug rebinds; a different address wanted by `net` makes
  the next bind promiscuous;
- no bus contention, no late SOFs, no duplicate OUT, and the mouse
  still bound at the end.

The co-simulation found one real bug while being written: the
descriptor walk's end test demanded nine bytes of remaining descriptor,
so it never saw the final seven-byte endpoint descriptor and no adapter
ever bound.

## What to check first on hardware

1. **Does the adapter work at full speed at all?** It is a high-speed
   device and our host is full speed. The RTL8152 is specified for
   both, but its full-speed ECM path has probably never been exercised
   by anything you own. `lsusb` will show whether it enumerated and
   which configuration was chosen.
2. **`lsusb`**: `class=ecm`, `config value 2`, and a MAC matching the
   serial number Linux prints.
3. **DHCP**, then ping, then a TCP transfer.
4. **`ic`** for the counters, and for the link state the notification
   endpoint reports.
5. **Unplug and replug** while `net` runs.

## Deliberately not done

| | Why |
|---|---|
| CDC-NCM | Its aggregation buys little at full speed. The enumeration is the same shape, so it is a small driver to add later if a device needs it |
| RNDIS | Deprecated at both ends |
| Vendor drivers (ASIX, Realtek's own) | A standard class first; a vendor driver only if a specific device turns out to matter |
| Zero-CPU idle polling via an auto-poll slot | Bulk and interrupt IN are identical on the wire at full speed, so a slot could poll the bulk endpoint and wake `net` only when a frame arrives. That is a real improvement over 45 wakeups a second, and it is worth doing only with a measurement showing the polling costs something |
| A bigger packet buffer | Growing to 4 KB would let a whole frame land in one transaction. One more DP16KD and a gateware change; measure first |
| Acting on link state | Reported, not used. A DHCP renewal on link-up is the obvious use |
| More than one adapter | One at a time, as for CDC-ACM and mass storage |
