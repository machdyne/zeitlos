# USB as a driver

`usb` (`sw/apps/usb`) is the USB host stack: the root ports, hubs,
HID setup, mass storage, USB ethernet (CDC-ECM) and USB serial (CDC-ACM,
CP210x). It was the kernel's until Phase 3 of
[userspace_drivers.md](userspace_drivers.md); now it is a driver
process, and the kernel keeps only its interrupt quiet. How the stack
works inside -- enumeration, the transaction engine, the class drivers
-- is still [usb_host.md](usb_host.md).

**Status (October 2026):** built and running on lakritz -- keyboard,
mouse, hub, USB sticks at `/usb` and USB ethernet. USB ethernet moves a
TFTP block in ~10ms, as the kernel did, but loses one or two frames a
transfer before they reach `usb`, so `tget` runs at 30-36KB/s against
~50KB/s before ([userspace_drivers.md](userspace_drivers.md), "Open
issues"). The rest is under "Known limits".

## What moved, and what did not

| | before | now |
|---|---|---|
| the stack (`usbh*.c`) | `sw/os/usb/`, stepped from IRQ 9 and the kernel tick | `sw/apps/usb/`, nearly unchanged, stepped by the `usb` process |
| IRQ 9 | `z_usbh_poll()` in the handler | bound by `usb` (`z_irq_bind()`): the kernel clears the controller's IRQEN and wakes `usb` |
| mass storage | the kernel's `Z_SYS_BLK` (USB) for `fs` | `fs` asks `usb` (`Z_USBOP_MSC_*`) |
| USB ethernet | `Z_SYS_USBNET`, polled by `net` with the scheduler held | two rings shared by `net` and `usb` ([zusbnet.h](../sw/common/zusbnet.h)) |
| USB serial | `Z_SYS_USBCDC_*`, with the scheduler held | requests to `usb` ([zusbcdc.h](../sw/common/zusbcdc.h)); same three calls |
| `lsusb` and the debug commands | in the kernel shell | the shell passes them to `usb`, which prints |
| keyboard and mouse reports | the controller feeds the kernel's HID registers | unchanged: no CPU in the path |
| Caps/Num/Scroll Lock LEDs | `hid.c` called the stack | `hid.c` pokes `usb` (`k_irq_poke()`), which asks for them |

The four retired syscalls keep their slots in `syscalls.def`, so no
later number shifts; they fail (`k_usb_retired()`, `sw/os/kusb.c`).

**Keyboards keep working if `usb` dies.** Once a keyboard or mouse is
set up, the controller polls it by itself and its reports go straight
to the kernel's HID registers (`usbh_hid.c`). What stops is setting up
new devices, storage, ethernet and serial.

**The UART is the rescue console.** Before `usb` has set a keyboard
up, and if it never does, the kernel console has no USB keyboard. The
kernel's own console input is the UART, as it always was during boot.

## Booting

The kernel shell starts `fs` from flash, then `usb`, then reads the
config and runs `init` (`sh()`, `sw/os/sh.c`):

- **`usb` second, not first,** so that it can come from the card. A
  copy in `/apps/usb` wins over the flash copy, as for any core app.
  `fs` cannot be overridden that way, because nothing can read the card
  before it runs.
- **Only with a USB host in the bitstream** (`Z_FEATURE2_USB_HOST`).
  On a board without one, `usb` is not started, and if it is started by
  hand it says so and exits.
- **A keyboard behind it works a few hundred ms after `fs` is up**:
  attach debounce, reset, enumeration.

`usb` registers as `usb0`. `net` waits for it, then for an adapter, as
it already waited for an adapter. `fs` asks about the drive at start
and then whenever `usb` tells it something changed.

## The process

`sw/apps/usb/usb.c`. One thread:

```
for (;;) {
    sleep until a request, IRQ 9, a poke, or the timeout
    serve the request, if there was one
    poll the stack, if IRQ 9 fired or a timed step is due
    move ethernet frames and serial bytes
    tell clients what the class drivers noted
    once a second: forget clients that died
}
```

- **IRQ 9.** The controller's interrupt is a level. When it fires, the
  kernel writes 0 to IRQEN (the silence write `usb` gave
  `z_irq_bind()`), which lowers it, and wakes `usb`, which runs next
  because it is a driver. `usb` polls, which clears IRQSTAT, and turns
  IRQEN back on. With nobody bound -- before `usb` starts, after it
  dies, on a reboot that left IRQEN set -- the kernel clears IRQEN
  itself (`k_usb_irq_off()`).
- **Telling an interrupt from a timeout.** IRQEN reading 0 means the
  kernel cleared it. A transfer `usb` makes outside a poll -- a sector,
  a frame, a serial packet -- turns IRQEN off around itself and clears
  its own XACT_DONE afterwards, so it does not wake `usb` for a poll
  with nothing to do.
- **The timeout.** Only what waits on time: every tick while the stack
  is mid-enumeration, every 16 ticks while a hub runs (its status
  endpoint is read from a poll slot that raises no interrupt), the
  ethernet adapter's receive timer, a serial client that is reading,
  and once a second while there are clients. Settled, with no hub and
  nobody using the adapter or serial, `usb` sleeps until the next
  interrupt or request.
- **Time** is the cycle counter over 65536, which is exactly the
  kernel's tick by construction (`usbh_ticks()`), with no syscall.
- **One request at a time, to its end.** A storage command spins on the
  engine until it is done, as it did in the kernel. The difference is
  who waits: only `usb`, and `fs` for the answer. The kernel ran it
  with the scheduler held, which stalled every process.
- **The engine.** Enumeration and storage are one thread now, so
  `z_usbh_bus_reserve()` steps a control transfer that is already
  under way to its end itself, rather than waiting for an interrupt
  handler to do it (`usbh.c`). Co-simulation keeps the interrupt model
  it was built for.

## The protocol

[sw/common/zusbproto.h](../sw/common/zusbproto.h), through one client,
[sw/common/zusbc.c](../sw/common/zusbc.c), which `fs`, `net`, `serial`
and the kernel (`-DZUSBC_KERNEL`, `zusb_cmd()` only) link.

- **A request** is a list of two blobs, the `z_usbreq_t` and the data,
  as in `fs`'s protocol. **A reply** is a blob in `usb`'s memory,
  borrowed until the client sends again. Each client gets its own reply
  slot (four, the least recently used reused), and sector data its own
  buffer, so one client's reply is not overwritten by another's
  request.
- **Events** (`Z_USB_EVENT`, a scalar of `Z_USBEV_*` bits) go to the
  client of a kind of device -- whoever asked about it last -- when one
  comes or goes, and to `net` and `serial` when data is waiting that
  was not before. They only say "look".
- **Storage:** `MSC_INFO`, `MSC_START`, `MSC_READ` and `MSC_WRITE` of up
  to 4 sectors. A sector is one SCSI command on the wire anyway, so a
  bigger request would only save round trips. `zusb_msc_read()` splits
  any count.
  A stick that NAKs while fetching is retried by the USB engine 15 times
  a round with a short spin between rounds, and given up on after 3
  seconds without progress ([usb_host.md](usb_host.md), "NAK pacing").
- **Bringing a drive up no longer resets it over and over**
  (October 2026, `usbh_msc.c`, "bringing a new drive up"). A stick that
  has just been plugged in can NAK a command's status for longer than
  the 3 s above while it starts. That used to count as a broken
  transport and cost a Reset Recovery on every step -- up to 400 of
  them -- and a reset restarts many sticks' own start-up, so some never
  became ready and some dropped off the bus: backup's "it makes the
  drive disconnect". During bring-up now:
  - ~250 ms of NAKs (`MSC_BRING_NAK_TICKS`) means "not ready yet": the
    step returns busy, and whatever the stick still owes for a command
    it took (data, then its status) is collected at the start of the
    next step (`drain_owed()`) instead of being reset away;
  - any other transport error still gets Reset Recovery, but at most
    once a bring-up (`MSC_BRING_RESETS`);
  - steps back off, 8, 16, 32 ... 256 ticks apart;
  - it gives up after ~15 s by the clock (`MSC_START_TICKS`), with 400
    steps as a backstop where no clock runs (co-simulation);
  - a drive that never became ready gets one more bring-up 5 s later,
    run by `usb` (fs asking would hold up all of fs's clients);
  - `usbmount` sleeps between its steps instead of firing them back to
    back.

  After bring-up nothing changed: every error is recovered as before.
  **To confirm on a board:** at most one `usb msc: reset recovery` line
  per plug-in, slow sticks coming up, and an exFAT stick shown as "not
  readable" by backup.
- **A new drive is brought up by `usb`, in steps.** A stick can take
  seconds to answer TEST UNIT READY. `usb` asks once every 8 ticks
  (`MSC_PACE`), between its other work, up to 400 times, then reads the
  capacity (`z_usbh_msc_start_step()`). When the drive is ready, or
  given up on, it tells `fs` (`Z_USB_EVENT`), which mounts it. So
  nobody waits: not `fs`'s other clients, not the keyboard or
  ethernet. `MSC_START` -- `usbmount` -- still runs the steps to the
  end for a caller that wants to wait.
- **Generations** in `MSC_INFO` start from the clock, so a restarted
  `usb` never repeats one for the same drive and `fs` remounts.
- **Serial:** `usb` reads the device for its client while the client
  is reading (for two seconds after its last read) and keeps what
  arrives, 512 bytes, so a read is answered from memory.
  `z_usbcdc_present()`, `_read()` and `_write()` are the calls they
  always were; they are requests now, so `serial` links `zusbc.o`.
- **Commands:** `lsusb`, `usbrescan` and `usbstat` always. `usbstat`
  prints the ethernet path's counters, then resets them: how often the
  adapter was asked and had nothing, frames in and out, ring-full
  stalls, wakes each way, and how long a frame waited in `usb`'s ring
  for `net` and a kick waited for `usb`, in microseconds; and IPv4
  frames whose length disagrees with their IP header (a USB packet
  lost or doubled inside a frame, which the class driver cannot see).
  Since round 2 of [net_throughput.md](net_throughput.md) also: how long
  reading a frame and sending one take, NAKs in the middle of a frame,
  and how soon after a send the answer was in the ring. Each ring slot
  carries its frame's arrival time (`rx_at`), so `net` measures its
  own wait.
  `lsusb` prints them too, without resetting. The bring-up tools
  (`usbcap`, `usbcapok`, `usbcapd`, `usbtune`, `usbnak`, `usbidle`,
  `usbbuf`) in a `make USBH_DEBUG=1` build of `sw/apps/usb`. That costs
  flash in the archive, or nothing at all as a card copy, instead of
  kernel space.

### Ethernet: rings, not messages

A message per frame would be a round trip per frame, about a
millisecond. So `NET_OPEN` sets up two single-producer,
single-consumer rings ([zusbnet.h](../sw/common/zusbnet.h)), and each
side writes only its own memory:

- **`usb`'s ring:** received frames (2 slots), and how far it has got
  with `net`'s.
- **`net`'s ring:** frames to send (2 slots), and how far it has got
  with `usb`'s.

Each reads the other's directly: any process may load from anywhere,
and a blob arrives as a physical address. `NET_OPEN` carries `net`'s
ring as its data, and its reply is `usb`'s ring itself. That is the one
reply that is not borrowed: it stays valid while `usb` runs, and `net`
checks once a second that the same `usb` is still running.

- **Wakes** happen only when a ring goes from empty to not empty:
  `usb` sends `net` a `Z_USB_EVENT`, and `net` sends `usb` a
  `Z_USB_KICK`. "Empty" is judged by the other side's position read
  AFTER the frame is in place: a receive spins on the bus for a
  millisecond and can be preempted, and judged by the position read
  before it, a frame could be put in a ring `net` had just emptied
  with nobody woken (round 10).
- **`net` no longer polls the adapter.** It sleeps until a frame is
  there.
- **After sending a frame in a conversation, `usb` asks the adapter
  back to back for up to 2ms** (`NET_BURST_US`): the answer -- a TFTP
  block after our ACK -- is usually a fraction of a millisecond away,
  and the next tick was 1.4-2.7ms. A NAK comes back in microseconds,
  and `usb` stays preemptible meanwhile. Not after an ARP reply.
  `usbstat` counts the bursts and the frames they found.
- **Otherwise `usb` keeps the pace `net` used to keep:** it asks the adapter
  every tick while a conversation is going on and every 16 ticks
  (~22 ms) when idle. A conversation is a unicast frame to us, or a
  frame sent that is not ARP (round 7e).
- **A full receive ring loses nothing:** `usb` stops asking, and the
  adapter holds the frame.

## Memory and flash

| | flash | RAM |
|---|---:|---:|
| `usb` | 41,936 | 52,604 image (13.7KB of it `.bss`), 16KB stack |
| kernel image, before and after | 223,852 -> 134,264 | 191,761 to `_end` |

- **`usb`'s `.bss`:** its ethernet ring (3KB), the sector reply buffer
  (2KB), four reply slots (1KB), the serial buffer, and the stack's
  device table.
- **No stdio in `usb`.** It prints through its own `printf`: zfmt's
  `vsnprintf`, then `write()` (`usbh.h`, `Z_USBH_OWN_PRINTF`; zfmt's
  `ZFMT_NO_STDIO`). The stdio behind ZFMT's `printf` would cost about
  12KB.
- **The kernel image stopped carrying `.bss`** in the same round:
  57KB of zeros. Its flash region went from 224KB to 160KB, and the
  archive grew to 848KB ([kernel.md](kernel.md), "The kernel image
  budget"; [flash_apps.md](flash_apps.md)).

## Drivers past the 1MB flash region

The Zeitlos flash region is 1MB, and the kernel and the core apps
share it. A driver in the kernel can only grow as far as the kernel's
share. A driver in the archive is no better off: the archive is full
too ([flash_apps.md](flash_apps.md), "Budgets").

**What changes with drivers as processes is that they need not be in
flash at all.** The card has no such limit, and a driver is a program
like any other. This is one of the main reasons for moving them out of
the kernel. Nothing here does it yet; these are the ways it is open:

1. **A bigger driver on the card.** A core app in `/apps` on the card
   wins over the flash copy, and `usb` and `net` are started the way
   any core app is. So the copy in flash can be the small one that
   brings the machine up and fetches files, and the card's the one with
   everything. `fs` is the exception, since nothing reads the card
   before it.
2. **Features as separate processes.** `web` already carries HTTP and
   TLS in its own process over `net`'s sockets. The same is open to
   `net`'s SSH, telnet and TFTP.
3. **Driver modules.** `usb` could hand a device it has no class driver
   for to a process that registered for it -- another serial chip, an
   audio interface, another ethernet adapter. The module would drive
   the transaction engine itself while holding a lease from `usb`: the
   arbitration `z_usbh_bus_reserve()` already does, extended to another
   process. Likewise `fs` could pass requests under a mount point to
   another filesystem process, in the same `zfsproto.h` messages.

What still limits a card driver is RAM. The RAM rule
([flash_apps.md](flash_apps.md), "Budgets") counts only what starts at
boot.

## Testing

```
make -C sw/apps/usb/tests        # the process and its client, on the host
make test_usb_cosim              # the stack against the gateware (also
make test_usb_msc test_usb_ecm test_usb_serial test_usb_hub)
```

- **`test_usb`** builds `usb.c` and `zusbc.c` on the host, with the
  stack faked: a drive, an adapter, a serial device.
  - **Storage:** reads split in fours, writes, the drive's event, and
    the interrupt kept quiet around a transfer.
  - **Ethernet:** the rings both ways, one wake per empty-to-not, a
    full ring not asking the adapter, the adapter going and coming,
    and a dead `net` forgotten.
  - **Serial:** buffering, the wake, writes in pieces, and the reading
    stopping when the client stops.
  - **The rest:** commands, and replies kept apart per client.
  - The drive brought up in steps, and `usbstat`.
  - 225 checks, under AddressSanitizer and UBSan.
- **The co-simulation** runs the stack itself, unchanged, against
  `rtl/usb`. It still passes.
- **`fs`'s test** plugs in a drive through the same client calls and
  checks that `/usb` follows it.

## Restarting

When `usb` crashes, the kernel starts it again a second later, from
the card if there is a copy, else flash, at most three times a minute
([userspace_drivers.md](userspace_drivers.md), "Restarting drivers").
Keyboards that were set up keep typing meanwhile (the kernel's HID
path). `usb` exiting -- no controller -- or being killed is left alone.

## Known limits

- **One client per kind of device:** the drive's (`fs`), the adapter's
  (`net`) and the serial device's (`serial`), as before.
- **A storage command holds up enumeration, ethernet and serial for its
  length:** milliseconds, bounded. It held up every process before.
- **The hub port errors behind a second hub** ([usb_host.md](usb_host.md),
  Known issues, item 5) are unchanged. `fs` retries the mount on each
  new enumeration, so a stick that keeps dropping is mounted once it
  stays.
- **USB ethernet loses a frame now and then** -- one or two in a 100KB
  TFTP transfer, before they reach `usb` -- and TFTP waits 0.5s for
  each ([userspace_drivers.md](userspace_drivers.md), "Open issues").
- **Once, the keyboard and mouse were not set up at boot** while the
  adapter was; the next boot was fine. Not understood yet: `lsusb` and
  `usbrescan` when it happens.
- **A restart of `usb`** -- by hand, or by the kernel after a crash
  (below) -- loses the open adapter until `net` notices (within a
  second; it checks that the ring it holds was opened, so a new `usb`
  with the old pid is still noticed), and the drive until `fs` looks
  again (each second; a new generation makes it remount).
