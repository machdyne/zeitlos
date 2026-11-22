# The USB-C socket as a USB device

On a board built with `` `USB_DEV `` -- every Lakritz build and Klinge,
by default -- the USB-C socket that powers the board is a USB device
with two functions. Plug it into a PC and it shows up as

```
16d0:116d  Machdyne Zeitlos  (bcdDevice 2.01)
  interface 0+1  CDC-ACM serial port   "Zeitlos console"
  interface 2+3  CDC-ECM network       "Zeitlos network"
```

- **the console**, `/dev/ttyACM0` on Linux, from the kernel on;
- **a network interface** between the PC and the board: the PC as the
  board's gateway, or TFTP, SSH, NFS and the remote desktop over the
  cable;
- and, while you share it, **the SD card as a drive on the PC**
  ([below](#the-card-on-the-pc)).

The device itself is software, `sw/apps/usbd`, a driver process like
`usb` for the host controller. The hardware, `rtl/usbd_ep.v`, gives it
packet buffers, and handles the network's frames by itself.

## What it costs: the BIOS

`` `USB_CDC `` (docs/usb_cdc.md) is the other way to use the socket: a
fixed CDC-ACM device in hardware, which is the console from the first
instruction of the BIOS. With `` `USB_DEV `` the console is software and
starts with `usbd`, early in the boot, after `fs`. So:

- the BIOS's own console (and its menu) is not reachable over USB;
- what the kernel printed before `usbd` ran is not lost: opening the
  port sends everything the console log still holds (4KB, about the
  whole boot), then goes on live.

A board that needs the BIOS over USB is built with `-USB_DEV` in its
release spec, which gives back `` `USB_CDC `` (rtl/boards.vh) -- the
console only, in hardware. Most users never need the BIOS.

## The console

```
minicom -D /dev/ttyACM0          (any baud rate: it is not a line)
screen /dev/ttyACM0
```

What the console prints goes to a terminal that has the port open
(DTR). Nothing waits for one: with no terminal, the output goes on as
if the port were not there, and the next terminal to open it is sent
the log first. Typing on it is typing on the serial console, for every
purpose -- including `passwd reset`, which wants the physical console
(docs/security.md): `usbd` is a driver and passes its input with
`Z_CONSOLE_PHYSICAL`.

The console log is polled about a hundred times a second while a
terminal is open: the kernel has no wake for new output.

## The network

The PC sees a USB network adapter (CDC-ECM): Linux and macOS have the
driver built in. Windows has none for ECM; NCM, which it does have, is
for later.

`net` uses it on a board with no network interface of its own
(`lakritz_gpio`, `lakritz_ddmi_audio`, `lakritz_console`), unless a USB
ethernet adapter is plugged in already. On a board with one (Klinge's
RMII, Lakritz's ENC28J60, Katze or Kabellos PMODs) the built-in one
wins; `phy = usbdev` in `net.cfg` picks the USB-C socket instead
(docs/networking.md).

### The PC as gateway

`net` asks for an address by DHCP, as on any network. Let the PC share
its connection over the new interface and it will answer:

- **Linux** (NetworkManager): the new wired connection, IPv4 method
  "Shared to other computers". Or by hand: an address on the interface,
  `ip_forward`, a masquerade rule, and a DHCP server (dnsmasq).
- **macOS**: System Settings, Sharing, Internet Sharing, to the new
  "Zeitlos network" interface.
- **Windows**: Internet Connection Sharing -- once there is NCM.

Without sharing, static addresses on both ends work too: `dhcp = 0`,
`ip`, `mask` and `gw` in `net.cfg`, and the PC's interface by hand. That
is enough for `tget`/`tput` (TFTP), `ssh` and NFS.

The PC's side of the link has the address 02:5a:4c:00:00:01 (iMACAddress,
locally administered); `net` uses its own. Every board has the same two,
which is harmless: each board is its own link to the PC.

### Speed

Full speed carries about 1.2MB/s at most; `net` itself bounds what TFTP
gets -- about 150KB/s received, 88KB/s to the card (docs/net_throughput.md).
Nothing is lost on the USB side: while both receive slots are full the
frame endpoint NAKs, and the PC keeps the frame until there is room.

## The card on the PC

```
> share
```

shares the SD card with the PC: it appears there as a drive, as it
would in a card reader, with the console and the network still there
beside it. While it is shared the card belongs to the PC alone. Zeitlos
cannot read it, so programs on the card cannot be started, and the
screen says so in a box that takes every key:

```
 Card shared
   The SD card is shared with the PC.
   Eject it on the PC when you are done,
   or press Eject once the PC has written.
   The PC has the card.
               [ Eject ]
```

To give it back, either:

- **eject it on the PC** (Eject in the file manager, `eject` or
  `udisksctl power-off` on Linux, "Eject" on macOS and Windows). This is
  always safe. The box closes by itself.
- **press Eject** (or Enter), or run `share` again (from a shell, over
  SSH, from cron). Do this only once the PC has finished writing: a PC
  that has not been told may still hold writes it has not sent, as with
  a USB stick pulled out too soon.

Zeitlos then reads the card again, with whatever the PC left on it.

On Sergei and Mozart, which have no USB device of Zeitlos's own, `share`
serves the card through the carrier's RP2040 over the XA-XD link instead
(docs/xlink.md, "The card on the PC"); everything above is the same.

`share` refuses while any file or folder on the card is open ("files or
folders on it are open"): close them, then share again. Files in flash
(the core apps, their help) stay usable throughout.

**What the PC sees.** Each change is a new device to the PC: sharing,
and giving the card back, take the board off the bus for a moment and
bring it back with a different set of functions (bcdDevice 2.02 with the
card reader, 2.01 without). A terminal on `/dev/ttyACM0` has to be
reopened (minicom says the port went away), and starts where it left
off; the network interface comes back by itself.

**Speed.** About a slow USB stick's: full speed, 12 Mbit/s on the wire,
and the card is read and written 4 KB at a time.

Without the window manager, `share` runs on the console alone: it says
what it is doing there, and `share` run again gives the card back.

## How it is built

**rtl/usbd_ep.v.** ulixxe's SIE (`rtl/ext/usb_cdc/sie.v`, MIT) does the
wire. In front of it:

- endpoints 0-3 and 5-7, served by `usbd` (or `share`): a 64-byte OUT
  and IN buffer each, as machboot's `rtl/mb_usbd.v`;
- endpoint 4, the ECM data endpoint, served by the gateware: OUT
  packets gathered into one of two 1.5KB receive slots until a short
  packet ends the frame, and one 1.5KB transmit slot sent as 64-byte
  packets with a zero-length packet after an exact multiple of 64.
  `net` handles one frame per interrupt, as with the RMII MAC's slots.

What flows one way shares a block RAM: the receive slots with the CPU
endpoints' OUT buffers, the transmit slot with their IN buffers. Three
DP16KD; two with `USB_DEV_RX_SLOTS 1`, one receive slot, which loses
nothing either (the endpoint NAKs, the PC waits) and is what
`lakritz_katze` and `lakritz_kabellos` use, having no block RAM to
spare.

At 0x6200_0000, in nibble 6 with the MACs (an app region,
docs/mpu.md). The register map is in the file's header and in
`sw/common/zusbd.h`. Cost, synthesised alone: 1,287 LUT4, 640 FF and 3
DP16KD (seven CPU endpoints; 1,100 and 546 with three).

| Endpoint | Use | Served by |
|---|---|---|
| 0 | control | `usbd` |
| 1 IN | ACM notifications (never sent) | -- |
| 2 OUT, IN | the console | `usbd` |
| 3 IN | ECM notifications: NETWORK_CONNECTION, CONNECTION_SPEED_CHANGE | `usbd` |
| 4 OUT, IN | Ethernet frames | the gateware and `net` (`usbd_eth.c`) |
| 5 OUT, IN | the card reader (MSC bulk-only), while shared | `share` |
| 6, 7 | free | |

**The builds** (November 2026, synthesised -- yosys `synth_ecp5 -abc9`,
not yet placed and routed):

| Build | `USB_DEV` | DP16KD of 56 | LUT4 |
|---|---|---|---|
| `lakritz_gpio` | yes | 53 (`USB_CDC`: 50) | 16,352 (`USB_CDC`: 16,275) |
| `lakritz_langkatze`, the board build | yes | 53 | |
| `lakritz_ddmi_audio` | yes | 53 | |
| `lakritz_katze` | yes, one receive slot | 56 | 16,764 |
| `lakritz_kabellos` | yes, one receive slot | 56 | 16,560 |
| `lakritz_console` | no: no network in this profile, a PMOD UART console, about 95% full | 50 | |
| Klinge | yes | 43 | |

Against `` `USB_CDC `` the logic is about the same (in `lakritz_gpio`:
77 LUT4 more, 33 carry cells and 10 flip-flops fewer), so a build that
placed with the hardware console should place with this; the cost is
block RAM.

**Interrupts.** Two lines. Z_IRQ_USBD (11) is `usbd`'s: a level,
`(PEND & IRQEN) != 0`, non-latched, bound with IRQEN as its silence
register, as Z_IRQ_USB is for `usb`. The frame endpoint's "a frame is
ready" is edge-detected into Z_IRQ_ETH (8), the line `net` already
takes, as its own pulse beside the MAC's (rtl/sysctl.v).

**Who owns what.** `usbd` and `net` share the block and never touch
each other's registers. `usbd` turns the frame endpoint on when the
host selects the data interface's alternate setting 1 (EPTYPE bit 24);
`net` sets NCTL bit 0 when it is up, which `usbd` sees as an event and
reports to the host as NETWORK_CONNECTION.

**sw/apps/usbd** (about 7KB, 5,564 bytes compressed: a core app in flash
so that a board with no card still has its console; its budget is in the
top-level Makefile's notes). Chapter 9 and the CDC 1.2, PSTN 1.2 and
ECM 1.2 requests, written from the specifications, from machboot's
`fw/usbd.c`. The console is the kernel's console log read with
`Z_SYS_KLOG_READ` (as `console` does for term) and the kernel's receive
ring fed with `Z_SYS_CONSOLE_INPUT`. It starts from `sh.c` straight
after `fs`, and is restarted after a crash like the other drivers
(kernel.c, "restarting drivers"); a restart takes it off the bus for a
moment, so the host sees the device go and come back.

**sw/apps/share** (on the card, about 66KB) is the same device code,
`usbd.c` built with `-DUSBD_MSC`: a fifth interface, mass storage
(Bulk-Only Transport, the SCSI commands a PC sends a card reader, as
machboot's), over fs's own SD driver (`sw/apps/fs/fatfs/sdmm.c`), 8
sectors to a card transfer. Only one process runs the device at a time.
`share` first has `fs` let go of the card (`zfs_umount("/")`, refused
while anything on it is open). It then asks `usbd` to step aside
(`Z_USBD_PAUSE`, sw/common/zusbd.h): `usbd` leaves the bus and IRQ 11,
and answers with how far its terminal had read the console log. `share`
joins the bus as the device with the card reader. Giving the card back,
`share` leaves the bus, `Z_USBD_RESUME` hands the position back, and
`fs` mounts the card again. A `share` that dies is noticed by `usbd`
within a second; the card then stays unmounted until `mount`. A `usbd`
restarted while the card is shared waits for `share` to finish.

**Detection.** FEATURES3 bit 4 (`z_usb_dev()`, sw/common/zsoc.h), and
the block's MAGIC, "UDEV". FEATURES bit 12 (there is a console) is set
too.

## Tested

`make test_usbd` (rtl/tb/tb_usbd_cosim.v): the real `usbd` and
`usbd_eth.c` (rtl/tb/cosim/usbd_vpi.c) on the real `rtl/usbd_ep.v`,
against the real USB host gateware (`rtl/usb`) run by the real
`sw/apps/usb` driver -- two implementations, written separately, that
meet at D+ and D-. The device runs on its own clock (+300 ppm), and its
CPU is scheduled as the kernel would: on its interrupt, with the
silence write, or on `usbd`'s timeout.

- **The console** (the host binds CDC-ACM): enumeration from the PHY's
  attach; the boot log replayed when the port opens, byte for byte;
  live output, in exact multiples of 64 too; typing, also into a ring
  that takes 7 bytes at a time; `usbd` restarted -- off the bus, the
  host's port gone, back, enumerated again, the log replayed again.
- **The network** (the host, built with `Z_USBH_COSIM_NO_SER`, binds
  CDC-ECM): the MAC string; the link up and down as `net` sets it;
  frames of 60, 64, 100, 128, 511, 512, 513, 1024, 1472 and 1514 bytes
  each way, byte for byte; both receive slots full, the frames after
  waiting in the host; tx_room.
- **The card reader** (`share`'s device, five interfaces; the host,
  built with `NO_SER` and `NO_ECM` too, binds mass storage): bring-up
  through UNIT ATTENTION; sectors read and written by the host's own
  driver, checked on both sides of the wire; the medium taken away, and
  the device still configured.

`make test_usbd_msc` (sw/apps/usbd/tests/msc_test.c, run first) takes
the card reader's commands one by one, against a model of the endpoint
registers. It covers what the Zeitlos host never sends and a PC does:
reads and writes of many sectors (run through the card 8 at a time),
the whole card in one READ, a read or a write past the end, a length
that does not match, a command it does not know, a card that fails, a
reset in the middle of a read, and the eject.

It found three bugs before any board did: `usbd` enabled its interrupts
only after its first wait (the host's first request met an endpoint
nobody watched); Zeitlos's own USB host took a composite device's
first data interface as the ECM one (sw/apps/usb/usbh_ecm.c); and the
host did not stuff the zero bit due before an EOP when a packet's CRC
ended in six ones (rtl/usb/usb_sie.v), which a real device's receiver
rejects every time.

`make test_usbd` runs the network part twice, with two receive slots and
with one (199 checks in all, the 43 of `test_usbd_msc` among them). The USB host's own benches (`test_usb`,
`test_usb_cosim`, `_msc`, `_ecm`, `_serial`, `_hub`, `_margin`, `_hid`)
pass with the host fixes.

Not tested here: a PC's USB stack, the handover between `usbd` and
`share` (it is messages between processes, not this bench's), and
place-and-route. On hardware: `lsusb -v`, the console with minicom,
the interface coming up (`ip link`), DHCP from a shared connection,
`ping`, a TFTP transfer both ways, unplugging and replugging the cable
while a terminal is open; `share`, the card mounted on the PC, files
copied both ways, ejected from the PC and from the button, and Zeitlos
reading what the PC wrote.

## Later

- **CDC-NCM**, for Windows, which has the driver built in.
- **Sergei and Mozart**, whose USB-C is the RP2040's: the same functions
  from the RP2040, over the XA-XD link -- phase 3.
