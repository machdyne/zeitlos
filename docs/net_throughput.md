# Network throughput

How fast a file comes over the network, where the time goes, and the
plan to go well past the ~50KB/s TFTP gets ran at when the USB stack
was in the kernel -- keeping the drivers in userspace
([userspace_drivers.md](userspace_drivers.md)).

## Status (October 2026)

- **Round 1 measured: 91-98KB/s** receiving only (`tget ... -`),
  78-82KB/s writing to `/ram` -- about twice the monolithic kernel's
  50KB/s, no retries. 1468-byte blocks and a retry timeout that follows
  the round trip; but a block still takes ~10-12ms, so the round trip
  itself did not shrink as expected.
- **Round 2 measured:** of a 10.8ms block, reading the frame from the
  adapter took 5.2ms (the wire needs ~1.1ms) and `net`'s handling 4.5ms.
- **Round 3 built, awaiting hardware:** both fixed -- the reader no
  longer spins 1.2ms on a NAK in mid-frame, and received frames are
  word-aligned so `net`'s copies run a word at a time. Expected: ~5ms a
  block, ~250-300KB/s.
- **Before it:** 30-36KB/s over USB ethernet -- ~10ms a 512-byte block,
  the monolithic kernel's pace, plus one or two lost frames a transfer
  at 0.5s each.

## The ceiling

Lakritz reaches the network through a USB ethernet adapter on a
**full-speed USB port: 12Mbps, about 1.2MB/s of bulk data at most**,
whatever the adapter's own link speed. On boards with the RMII MAC
the link is 100Mbps and the CPU is the limit. Either way the limit is
far above 50KB/s: TFTP is stop-and-wait, so its speed is the block size
over the round trip, and the round trip was ~10ms.

## Where a round trip went (round 10, 512-byte blocks, ~10ms)

| | ms | note |
|---|---|---|
| block waits in the adapter for `usb` to ask | ~2 | asked once a tick or two |
| `usb` reads the frame | ~1.1 | spinning on the USB engine, 512 bytes at a time |
| the ACK waits for `usb` | 0.5-1.5 | `net`'s hand-off went to the shell first |
| processing, copies, switches | the rest | ~6 copies of each byte at ~7 MIPS; no D-cache write buffer on lakritz |
| a lost frame | 500 | one or two a transfer: a third of the time |

## Which interfaces take the big blocks

All of them: a 1468-byte TFTP block is a 1514-byte frame, an ordinary
full-size one.

| interface | largest frame received |
|---|---|
| RMII MAC (`rtl/ethmac_rmii.v`) | 2048-byte slots |
| ENC28J60 | 1518 (`MAMXFLL`), in its 6.6KB ring |
| USB ethernet | 1536-byte ring slots (`Z_USBNET_FRAME`) |
| ESP32 link | 1518 (`ZNIC_MAX_PAYLOAD`), credits sized for it in an 8KB FIFO |

The ~500-byte sizes elsewhere are not about frames: TCP's MSS of 536 is
sized so a window of segments fits the ENC28J60's ring
([networking.md](networking.md), "Throughput"), and 512 is TFTP's
default block. A TFTP transfer has one frame in flight, so the ring
never holds more than one. The ESP32's own TFTP server ignores options
and sends 512-byte blocks, which the client takes as they come. A path
with a smaller MTU than Ethernet's would drop the big blocks (they go
with DF and nothing reassembles), so a get that has agreed to them and
sees none after two retries asks again for 512.

## The proposals

| | change | where | expected | effort | state |
|---|---|---|---|---|---|
| **A** | **shorter round trips:** the ACK goes last so the hand-off is to `usb`; `usb` asks the adapter back to back for 2ms after a send instead of waiting for the next tick; word copies to and from the USB buffer | `net`, `usb` | 10 -> ~6ms a block | small | built |
| **B** | **retry timeout from the round trip** instead of a fixed 0.5s | `net` | a lost frame ~0.1s | small | built |
| **C** | **TFTP blksize (RFC 2348): 1468-byte blocks** | `net`, `tget` | 2.9x the data a round trip: **~150-200KB/s** with A | small | built |
| D | TFTP windowsize (RFC 7440): several blocks per ACK | `net` | ~300-450KB/s | medium | needs a server with it: tftpd-hpa 5.2 does not |
| E | HTTP over TCP: a `hget` to file from any web server; MSS 1460 and a bigger window for the USB adapter, whose buffer is far larger than the ENC28J60's | `net` settings, new client | ~200-400KB/s | medium | |
| F | bigger writes in `tget`: chunks of up to 8KB to `fs` | `net`, `zstream`, `tget` | receiver out of the way past ~150KB/s | small-medium | |
| G | frame receive in the USB host controller: ask the adapter in hardware, land a whole frame in its own BRAM, interrupt when complete | RTL, `usb` | toward the ~1MB/s bus limit with D or E | large; lakritz is ~85% full | |
| H | the D-cache write buffer on lakritz (`DCACHE_WBUF 2`) | RTL | every copy faster | ~540 LUT4 | |

The order: A+B+C, measure; then D (if the server has it) or E; then F
when the receiver binds; G and H are hardware.

## Measuring

```
tget loom wm -            # the network alone: received and discarded
tget loom wm /ram/test    # with the file write
usbstat                   # before (to reset) and after
```

`net` prints the rate, then the time a block took on each side, the
retries, and duplicates. A retry line names the block, when, after how
long, and what `usb`'s ring held. `usbstat` counts the back-to-back
asks after a send and the frames they found. See
[networking.md](networking.md), "TFTP", and
[usb_driver.md](usb_driver.md), "Ethernet: rings, not messages".

## Round 1: A, B and C

- **`net`, `tftp.c`:**
  - the RRQ asks for `blksize` 1468; an OACK sets the size and is
    answered with ACK 0; error 8 means ask again without options;
  - the ACK goes after the chunk to the receiver;
  - the retry timeout is 4 x the smoothed round trip + 20ms, 41ms to
    0.5s, doubling per retry (~4s until the server first answers);
  - a duplicate of the newest block is not ACKed (tftpd-hpa would
    otherwise send every later block twice).
- **`usb`:** after sending a non-ARP frame, the adapter is asked back to
  back for up to 2ms (`NET_BURST_US`); aligned word copies in and out
  of the USB buffer (`usbh_ecm.c`).
- **kernel shell:** `tget <host> <file> -` discards, for measuring.
- **Tests:** `test_tftp_get` 86 checks (options, refused options, the
  adaptive timeout, ACK order, no Sorcerer's Apprentice); `test_usb`
  234 (the burst, and none after ARP).

**Measured** on lakritz against tftpd-hpa: 91.5 and 97.6KB/s received
and discarded, 77.7 and 81.9KB/s to `/ram`; no retries, no duplicates.
`ack->next block` 10.3-13.3ms. The back-to-back asks found 75 of 77
answers within 2ms of the ACK going out, so the server and the wire
are fast; the rest of the 10-12ms is somewhere in `usb` and `net`.

## Round 2: where the round trip goes

Measurement only, no behaviour change:

- **`usb`** times each frame read (from its first data to its end) and
  each send, and counts NAKs in the middle of a frame (each costs a
  spin, `pace()`), and the time from a send's end to the answer being in
  the ring, its reading included (`usbstat`, two new lines).
- **The ring** carries the cycle counter of each frame's arrival
  (`z_usbnet_srv_t.rx_at`), so `net` measures exactly how long a frame
  waited for it, and how long it then took to queue the ACK
  (`net: tftp per block over usb: ...`).

With `kick waited for usb` (ACK queued to sent), the round trip is then
accounted for, piece by piece:

| piece | from |
|---|---|
| ACK queued -> `usb` starts sending | `usbstat`: a kick waited for usb |
| sending the ACK | `usbstat`: sending |
| ACK sent -> next block in the ring | `usbstat`: after a send, the answer was in the ring |
| ...of which reading the frame | `usbstat`: reading a frame |
| in the ring -> `net` takes it | tftp line: waited in usb's ring |
| `net` takes it -> ACK queued | tftp line: net queued the ACK after |

**Measured** (`tget loom wm -`, 93.3KB/s; the round trip, averages):

| piece | ms |
|---|---|
| ACK queued -> `usb` sends it | 0.06 |
| sending the ACK | 0.13 |
| ACK sent -> next block at the adapter | ~2.7 |
| **reading the 1514-byte frame** | **5.2** (103 NAKs mid-frame over 95 frames) |
| in the ring -> `net` takes it | 0.7 |
| **`net`: frame taken -> ACK queued** | **4.5** |

## Round 3: the two big pieces

- **Reading a frame.** A NAK in the middle of a frame means the adapter
  has not got the rest off the wire yet, microseconds away at 100Mbps.
  The reader answered each with `pace()`, a spin of ~1.2ms on lakritz,
  and met about one per frame. Now the USB engine retries by itself, its
  most (15 per round, a few microseconds each), and between rounds the
  spin is ~50us (`usbh_ecm.c`, `ECM_HW_NAKS_MID`, `pace_short()`).
  Expected: 5.2 -> ~1.5ms.
- **`net`'s copies.** newlib's `memcpy` copies words only between
  buffers aligned alike. Behind a 14-byte Ethernet header the IP header,
  and the TFTP block in it, sit 2 off from any aligned buffer, so each
  1.5KB copy went a byte at a time, ~1ms. Received frames now start 2
  into their buffer, in `net` (`eth.c`, as the transmit buffer already
  did) and in `usb`'s ring (`Z_USBNET_RX_OFF`; `usb` writes them with
  shifted word stores), and TFTP's queue is word-aligned. Expected: 4.5
  -> ~1.5ms.
- **A fallback** for a path that drops full-size frames: big blocks
  agreed and none arriving after two retries -> ask again for 512.

Expected together: ~10.8 -> ~5ms a block, ~250-300KB/s received.
