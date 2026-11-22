# The XA-XD link: the network and the card on the carrier's USB

On **Sergei** and **Mozart** (Sechzig ML0, ML1 and ML2 modules), Zeitlos's
gateware has the link to the carrier's RP2040 that machboot uses
(machboot's `docs/xlink.md`). With machboot's RP2040 firmware
(`machboot-sv`, from the release after r12), two things reach the PC over
the carrier's USB port:

* **The network.** The PC sees a USB Ethernet adapter (CDC-ECM) whose
  frames reach netx. It is a third way to put the board on a network,
  beside its Ethernet port and a USB adapter, and a way to reach it with
  nothing but the cable that powers it.
* **The SD card.** `share` makes it a drive on the PC, through the
  RP2040's card reader, the one machboot shares its own card with
  ("The card on the PC", below).

## What you need

* A Sergei or Mozart, any module (`` `XLINK `` in `rtl/boards.vh`).
* machboot's supervisor firmware on the carrier's RP2040, with the menu
  for your module. The RP2040 watches the link only after it has started
  a system: machboot's Start, or a system from its list.
* netx on the card (`/apps/netx`) for the network. Flash net has no link
  driver, and without netx the RP2040 lets go of the link after 45 s
  (below). `share` needs the RP2040 on the link too, so it needs netx
  running as well.

## The console on ML0 and ML1

On ML0 and ML1 modules the link uses balls B3 and A2 (XC, XD), which
Zeitlos used to use for its console's UART. The console is now on the
module's own UART, B4 (TX) and C4 (RX). The carrier's RP2040 bridges
that UART to the PC as its USB serial port (machboot's docs/rp2040.md:
GP4, GP5). So the console is now on `/dev/ttyACM0`, where it was not
before. On ML2 the console's balls are unchanged.

machboot's RP2040 probes the link once a second for 40 s after every
Start, on every module. A Zeitlos image from before this change still
has its ML0/ML1 console input on XD, and sees those probes as noise
there. Rebuild it to fix that.

## Setting up the network

Without a config, netx adds the link as one more interface (`xlink`) and
asks for an address by DHCP. That suits a PC that shares its connection,
such as Linux with NetworkManager's "Shared to other computers". The
usual case is that Zeitlos serves the PC. For that, give the link the
LAN side, either in `/data/net/net.cfg` or as netsettings' "Carrier USB,
to a PC" port:

```
full     = 1
wan.phy  = builtin            # the Ethernet port, upstream
lan.phy  = xlink              # the PC on the carrier's USB
lan.ip   = 192.168.77.1/24
lan.dhcpd = 1
wan.nat  = 1
```

The PC then gets a 192.168.77.x address by DHCP and reaches Zeitlos at
192.168.77.1. With `wan.nat`, it also reaches the network beyond.

## The card on the PC

```
> share
```

Without a USB device on a USB-C socket (`USB_DEV`, docs/usb_dev.md), but
with the link, `share` serves the card over the link instead. The
dialog, the Eject button and the PC's eject work as on a USB-C socket.

* netx steps aside first (`Z_USBD_LINK_PAUSE`, zusbd.h). The PC's network
  on the carrier's USB pauses while the card is shared; Ethernet goes on.
  netx takes the link back afterwards, or by itself if `share` crashes.
* The RP2040 is the PC's card reader, as it is for machboot. Each READ,
  WRITE and EJECT is a request on the link, and `share`'s `xlcard.c`
  answers it as machboot's `fw/xlink.c` does.
* FLAGS bit 0 says the card is shared. Bits 7:4 count each change, so
  the RP2040 tells the PC that the medium changed.
* Speed: the link's own, as for machboot's card (not yet measured here).
  It is fine for copying files, and slower than a USB-C socket.

## How it works

The RP2040 is the link's master: it asks, and Zeitlos answers each
request before the next one. Frames go like this:

| Direction | How |
|---|---|
| PC to Zeitlos | `NET_PUT` (op 12): the request carries the frame. The reply is OK, or BUSY, in which case the frame is dropped, as a full NIC drops it |
| Zeitlos to PC | the frame waits in netx (up to 4), and FLAGS bit 1 says so. The RP2040 polls STATUS, asks `NET_GET` (op 13), and the reply carries the oldest frame |

Every request is `[op][tag][pc][0]` followed by its data, and every reply
is `[op|0x80][tag][status][n]` followed by its data (machboot's framing).
In the pc byte, bit 0 means a PC has configured the RP2040's USB, and
bit 1 means the PC's driver has the network interface up. netx drops
frames for a PC whose interface is not up, rather than queue them for
nobody.

`HELLO` (op 1) is how the RP2040 tells Zeitlos from machboot. Zeitlos's
reply data is `[shared][0x5A][caps][0][sectors 4]`, where caps bit 0
means the network. machboot's byte 1 is always 0. Until a HELLO says who
answered, the RP2040 acts on no FLAGS bit, and netx sets none: to
machboot's eyes the same bits mean other things (bit 1 would be
"starting the system").

`rtl/xlink.v` is machboot's `rtl/mb_xlink.v` with one more output, which
says its receive buffer is full. That level's rising edge is ORed into
`Z_IRQ_ETH`, so a request wakes net as a received frame does, and
`xlink_eth_recv()` serves it from `eth_poll()`.

| | |
|---|---|
| Base | `0x6400_0000`; FEATURES3 bit 6 (`z_xlink()`) |
| Registers | MAGIC "XLNK" +0x0, RX +0x4, TX +0x8, FLAGS +0xC, FRAMES +0x10 |
| Buffers | the request at +0x1000, the reply at +0x1800 (2 KB each) |
| Pins, ML2 | XA N1 (MOSI), XB P1 (SCK), XC R1 (MISO), XD R2 (CS) |
| Pins, ML0/ML1 | XA A4, XB A3, XC B3, XD A2; pull-ups on XA, XB, XD |
| Wire | SPI mode 0 at 8.93 MHz; STATUS 0x01, SEND 0x02, RECV 0x03; sync byte 0x5A |

## The RP2040's side

This is machboot's `rp2040/src/znet.c`, with `znet_tusb.c` and `msc.c`
(described in machboot's `docs/zeitlos-net.md`):

* After a Start or a system from the list, the RP2040 probes the link as
  it does for machboot, then once a second for 40 s.
* When STATUS answers, it sends HELLO once a second until something
  answers. Each unanswered HELLO costs its main loop 250 ms, so if
  nothing answers for 45 s (Zeitlos with flash net only), it lets go of
  the link.
* Once HELLO comes back with 0x5A, it polls as often as it can instead
  of every 20 ms. It sends the PC's frames and fetches Zeitlos's, while
  the PC's USB can take them, and it passes the card reader's requests
  through while `share` has the card.

The PC's network adapter is always present in the RP2040's USB
configuration. This TinyUSB's ECM cannot report "cable out", so while
machboot runs, or nothing answers, the PC's frames are simply dropped.

## Limits

* The network is CDC-ECM. Linux and macOS have drivers built in, but
  Windows does not. NCM, which Windows 11 has, would be the follow-up.
* Speed: tftp measured 88 KB/s on a Sergei ML2. TFTP waits for every
  block's reply before sending the next, so TCP should do better.
* netx only. Flash net, at a few bytes from its budget, has no room.
* While the card is shared, the network on the carrier's USB pauses.

## Tests

* `sim/xlink/tb_xlink.v`: the slave against a master at 8.93 MHz, 27
  checks (`iverilog -o tb tb_xlink.v ../../rtl/xlink.v && vvp tb`).
* `sw/apps/net/tests/test_xlink_eth.c`: netx's driver against a model of
  the registers, 49 checks (`make test` in `sw/apps/net`).
* machboot's `rp2040/tests/test_znet.c`: the RP2040's code over a
  byte-level model of the wire, 28 checks. It covers the network and
  `share`'s card (reads, writes, eject, netx stepping aside and back).
  It runs against machboot's model of Zeitlos's side, and also against
  this tree's `xlink_eth.c` and `share/xlcard.c`
  (`-DZEITLOS_XLINK_ETH=... -DZEITLOS_XLCARD=...`).
