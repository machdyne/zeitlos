# mesh

A Meshtastic client for Zeitlos. `sw/apps/mesh` talks to a LoRa node
running Meshtastic firmware -- first target a Heltec WiFi LoRa 32 V3 --
over USB, using Meshtastic's streaming client API, and gives you the
node list, the channels and a chat window.

**Status: phase 4 (the window) done off hardware, awaiting a hardware
test.** Phases 1 and 3 are confirmed on hardware: a Heltec V3 on
Meshtastic 2.7.3 binds as `cp210x`, and `run mesh` (then headless)
printed its node, both channels -- the unnamed primary as `#LongFast`,
the named `#LD` -- and its node list, correctly decoded. The window is
built, its logic passes the host tests, and every state of it has been
rendered and looked at off the board (`make render`). What is needed
from hardware next is in
[Hardware test: phase 4](#hardware-test-phase-4). Phase 5 is to be
reconsidered before it starts.

## Contents

- [What the radio does and what we do](#what-the-radio-does-and-what-we-do)
- [Clean-room and licensing](#clean-room-and-licensing)
- [Architecture](#architecture)
- [The USB side: CP210x](#the-usb-side-cp210x)
- [The wire: framing](#the-wire-framing)
- [The wire: protobuf](#the-wire-protobuf)
- [Session](#session)
- [Model](#model)
- [Phase 3, as built](#phase-3-as-built)
- [Phase 4, as built](#phase-4-as-built)
- [History](#history)
- [User interface](#user-interface)
- [Risks](#risks)
- [Phase plan](#phase-plan)
- [Hardware test: history](#hardware-test-history)
- [Hardware test: phase 4](#hardware-test-phase-4)
- [Hardware test: phase 3](#hardware-test-phase-3)
- [Hardware test: phases 0 and 1](#hardware-test-phases-0-and-1)
- [Deliberately not done](#deliberately-not-done)

## What the radio does and what we do

The single fact that makes this project small: **the node does all the
radio work and all the cryptography.** Channel encryption (AES on the
channel PSK), PKI direct messages, routing, retransmission, duty-cycle
and the NodeDB all live in the Meshtastic firmware. A client sees
packets that are already decrypted (`MeshPacket.decoded`) and hands the
node plaintext to send. The phone app, the Python CLI and the web
client are all this kind of client.

So Zeitlos needs no LoRa driver, no AES, no Curve25519 and no mesh
routing. It needs a USB serial driver, a framer, a protobuf codec small
enough to write by hand, and an app.

## Clean-room and licensing

Two things are GPL on the Meshtastic side, and neither is needed:

- **`meshtastic/protobufs` is GPL-3.0.** The `.proto` files are not
  copied into this tree, not used to generate code (no `protoc`, no
  nanopb output), and not required to build. What `mesh` needs from
  them is the wire interface -- which field number carries which value,
  and with which wire type -- and that is a set of interoperability
  facts, written down here and in `sw/apps/mesh/mesh_pb.h` in our own
  words as plain `#define`s. No comments or text from the schema are
  reproduced.
- **The Python, JavaScript and Android clients are GPL.** They are not
  consulted while authoring. The session flow is written from the
  published client-API page
  (<https://meshtastic.org/docs/development/device/client-api/>) and
  from observing a real node's byte stream.

The protobuf codec (`sw/common/zpb.c`) is written from the public
protobuf wire-format specification. nanopb is zlib-licensed and would
be acceptable, but it exists to consume generated descriptors, and
generating them is exactly the step we are avoiding; a hand-written
tag walker is also smaller than nanopb's runtime.

The CP210x driver is written from Silicon Labs' public application note
**AN571, "CP210x Virtual COM Port Interface"**. Linux's `cp210x.c`
(GPL-2.0) is not consulted.

Test vectors are captured bytes from a real node, which are data rather
than anyone's code. The capture tool is our own.

*Not legal advice; this is the engineering position the tree takes,
the same one `docs/usb_host.md`'s clean-room statement takes.*

## Architecture

```
  Heltec V3 (ESP32-S3 + SX1262, Meshtastic firmware)
      | UART 115200
  CP2102  USB-UART bridge
      | USB full speed, vendor class, bulk IN/OUT
  kernel: sw/os/usb/usbh_cp210x.c  --\
  kernel: sw/os/usb/usbh_cdc.c       --> one "USB serial" data path
                                          Z_SYS_USBCDC_* (unchanged)
  sw/apps/serial   port provider `serial0`, USB connection
      | z_port DATA (sw/common/zport.h)
  sw/apps/mesh     framer -> zpb -> session -> model -> window
```

The layering is the existing one, reused rather than extended:

- **The kernel binds the bridge.** A CP210x becomes one more way of
  being "the USB serial device". Its data path is plain bulk bytes --
  CP210x, unlike FTDI, puts no status header in front of received data
  -- so after a different setup sequence at enumeration it answers
  `z_usbh_cdc_read()` / `_write()` exactly as a CDC-ACM device does.
  The syscalls, `zusbcdc.h` and `serial` do not change.
- **`serial` stays the owner.** `mesh` connects to `serial0` with
  `Z_CONN_USBSERIAL_ARG`, the same CONNECT `term`'s `usbserial` sends.
  It never calls `Z_SYS_USBCDC_*` itself, so the one-owner convention
  in `zusbcdc.h` holds and `serial`'s round-32 flow control applies for
  free.
- **Free side effect:** with phase 1 alone, `usbserial` in a term
  window shows the node's debug console. That is the phase 1 test, and
  a permanent diagnostic.

`serial` writes a few in-band status lines onto the connection (its
connect banner, "[serial: the USB device went away]"). Meshtastic's
stream format already treats any byte outside a frame as console text,
so `mesh` simply sees them as log lines.

One thing did need changing: when the device went away, `serial`
printed that notice and dropped the connection on its own side only,
without telling the client. It now also sends `Z_PORT_CLOSE`, which is
what `mesh` reconnects on (and what makes `term` show its "closed by
the other end" panel).

The framer and codec take bytes, not a port, so a second transport --
Meshtastic's TCP API on port 4403, through `net`, for a node on WiFi --
is a new byte source and nothing else (phase 5).

## The USB side: CP210x

`sw/os/usb/usbh_cp210x.c`. **Implemented** as the first of a family:
FTDI and others come next, so USB serial is now one data path with a
per-kind probe, setup request list and receive filter --
`docs/usb_host.md`, "USB serial devices", has the design and the FTDI
plan. What follows is the CP210x part of it.

**Probe.** VID `10c4`, PID `ea60` (the CP2102/CP2102N/CP2104 family
ID), first interface class `0xff`, one bulk IN and one bulk OUT
endpoint. Match on VID:PID *and* shape; a vendor-class interface with
the right endpoints but the wrong ID is not ours. The VID/PID are
already stored in the device record (`d_vid_*`, `d_pid_*`). The
configuration chooser's "skip a vendor-class first configuration"
rule does not interfere: a CP2102 offers exactly one configuration,
so it is kept.

**Setup** -- vendor requests to the interface, `bmRequestType`
`0x41`, each its own enumeration state, each allowed to STALL without
stopping the bind except the first:

| state | request | value | data |
|---|---|---|---|
| `E_CP_ENABLE` | `IFC_ENABLE` (0x00) | 1 | -- (fatal on failure: a disabled UART passes nothing) |
| `E_CP_BAUD` | `SET_BAUDRATE` (0x1e) | 0 | 4 bytes, 115200 little-endian |
| `E_CP_LINE` | `SET_LINE_CTL` (0x03) | 0x0800 (8N1) | -- |
| `E_CP_MHS` | `SET_MHS` (0x07) | see below | -- |

All four run through one enumeration state, `E_SER_SETUP`, then
`z_usbh_cdc_bind()` and `Z_USBH_CLASS_CDC`, so unplug, `lsusb` (which
now says `class=cdc (cp210x)`) and the syscalls all behave as for CDC.

**DTR and RTS, carefully.** On ESP32 boards these two lines drive the
EN/IO0 auto-reset transistors that `esptool` uses. Asserting one
without the other resets the ESP32 (or drops it into its ROM
bootloader); asserting both, or neither, leaves it running. So they
are set in **one** `SET_MHS`, with both mask bits and both value bits
in the same request -- `0x0303` -- never as two writes. Both-asserted
is what Linux does on `open()`, which is the state the Meshtastic CLI
is known to work in. If the Heltec misbehaves, `0x0300` (both
deasserted, atomically) is the fallback; it is a one-constant change.

**Baud rate** is fixed at 115200 at bind, like CDC's line coding. That
is what Meshtastic's USB console runs at. A baud-rate syscall is not
needed for this project and is not added.

## The wire: framing

Every protobuf in either direction is preceded by four bytes:

| byte | value |
|---|---|
| 0 | `0x94` |
| 1 | `0xc3` |
| 2 | length, high byte |
| 3 | length, low byte |

A length over 512 means a false start: go back to hunting for `0x94`.
Bytes seen while hunting are the node's console text; `mesh` collects
them into lines for its log view. Towards the node there is only ever
framed data.

`sw/apps/mesh/mesh_frame.c`: a byte-at-a-time state machine
(HUNT, START2, LEN_HI, LEN_LO, BODY) with a 512-byte body buffer,
calling back with complete frames and complete text lines. It must
survive a lost byte anywhere -- the stream has no CRC -- and the
failure it must avoid is not "a packet is lost" but "a corrupt packet
is believed" (see [Risks](#risks)).

## The wire: protobuf

`sw/common/zpb.h`/`.c`: the protobuf wire format, nothing else. No
schema, no allocation, no reflection.

**Reading** is a cursor over `(ptr, len)`: `zpb_next()` returns the
next field's number and wire type and its value -- a varint, a
fixed32, a fixed64, or a (ptr, len) slice for length-delimited. A
nested message is read by opening a new cursor on the slice. Unknown
fields are skipped by wire type, which is what gives forward
compatibility with newer firmware for free. Every step is bounds-checked
and a malformed field ends the message with an error rather than
reading past it.

Two details that bite:

- **Negative `int32` is ten bytes.** `rx_rssi` is `int32` and is
  always negative, so the varint reader must accept 64-bit varints and
  truncate, not stop at five bytes.
- **Floats are fixed32**, reinterpreted. `rx_snr` and `NodeInfo.snr`
  are the only ones used; `mesh` keeps them as tenths of a dB in an
  `int16_t` rather than doing float arithmetic.

**Writing** is into a caller's fixed buffer: varint, tag, fixed32,
bytes/string. A nested message is encoded into a scratch buffer first
and then written as a bytes field; everything `mesh` sends is well
under 512 bytes, so the copy costs nothing worth avoiding.

The field numbers used, in `sw/apps/mesh/mesh_pb.h`:

| message | fields used |
|---|---|
| ToRadio | packet 1, want_config_id 3, disconnect 4, heartbeat 7 |
| FromRadio | id 1, packet 2, my_info 3, node_info 4, config 5, log_record 6, config_complete_id 7, rebooted 8, channel 10, queueStatus 11, metadata 13, clientNotification 16 |
| MeshPacket | from 1 (fixed32), to 2 (fixed32), channel 3, decoded 4, id 6 (fixed32), rx_time 7 (fixed32), rx_snr 8 (float), hop_limit 9, want_ack 10, rx_rssi 12 (int32), hop_start 15, pki_encrypted 17 |
| Data | portnum 1, payload 2, request_id 6, reply_id 7, emoji 8 |
| NodeInfo | num 1, user 2, position 3, snr 4 (float), last_heard 5 (fixed32), device_metrics 6, hops_away 9, is_favorite 10 |
| User | id 1, long_name 2, short_name 3, hw_model 5, role 7 |
| MyNodeInfo | my_node_num 1 |
| Position | latitude_i 1 (sfixed32), longitude_i 2 (sfixed32), altitude 3, time 4 |
| Channel | index 1, settings 2 (name 3 inside), role 3 |
| DeviceMetadata | firmware_version 1 |

Port numbers: `TEXT_MESSAGE_APP` 1, `POSITION_APP` 3, `NODEINFO_APP`
4, `ROUTING_APP` 5, `TELEMETRY_APP` 67. Broadcast destination is
`0xffffffff`.

The exact field list is checked against a captured stream in phase 0
before it is frozen; anything above that the capture contradicts is
corrected here first.

### As built (phase 2)

`sw/common/zpb.c` is 1.4 KB of RV32IM code and `mesh_frame.c` 0.9 KB,
neither allocating. Two framer decisions that are not obvious from the
spec:

- **A lone 0x94 is text.** It is also the last byte of a UTF-8 em dash
  (`E2 80 94`), which log lines do contain. So START1 not followed by
  START2 goes to the console line, and the following byte is examined
  afresh -- it may itself be a START1.
- **A bad length is re-examined, not dropped.** When a header's length
  is over 512, its two length bytes go back through the hunt, because
  when a header's own START bytes were lost they are exactly where the
  next real header starts.

What the framer cannot do, and the tests confirm it does not pretend
to: a byte lost *inside* a frame makes that frame end one byte late and
takes the next frame with it. The test drops a byte at every position
in a frame and checks that the frame after next always arrives intact;
the damaged one is phase 3's plausibility check to catch.

## Session

1. CONNECT to `serial0` with `Z_CONN_USBSERIAL_ARG`. Refused -- no USB
   serial device, or someone else already has it -- is shown as such.
2. Wake the node's API: a short run of `0xc3` bytes, so a node whose
   receiver is mid-hunt resynchronises before our first real frame.
3. Send `ToRadio{want_config_id = N}`, `N` a fresh random nonce from
   the TRNG (`zrng.h`).
4. Absorb the dump: `my_info`, `metadata`, `config`/`moduleConfig`
   (skipped for now), one `channel` per slot, one `node_info` per known
   node -- updating the model per message by type, not by position.
   The docs say the order is not to be relied on, and it is not.
5. `config_complete_id == N` ends the dump. An old nonce is from a
   previous session and is ignored.
6. Live: `packet` messages as they arrive. Send a `heartbeat` every
   minute so the node does not decide the client has gone.
7. `rebooted` means: forget the node's DB, channels and config, and ask
   again (step 2). A `serial` CLOSE (the device went away) means: drop
   the session, keep the message history, reconnect every 3 s. Silence
   alone is not treated as a failure -- a quiet mesh is silent.

Sending text: `ToRadio{packet{to, channel, want_ack, decoded{portnum
= 1, payload = utf8}, id = random}}`. The node answers with a
`ROUTING_APP` packet whose `request_id` is our `id` -- delivered, or an
error reason -- which is how a sent message gets its status.

## Model

Fixed-size, allocated once at start, sized by `zeitlos.cfg`
(`apps.mesh.*`, `docs/config.md`) with defaults that fit a 1MB board:

- nodes: 128 x ~80 bytes (num, short/long name, hw model, last heard,
  SNR, hops, position, battery). Least-recently-heard evicted, never our
  own node or a favourite.
- channels: 8, the firmware's maximum.
- messages: a ring of 96 x ~260 bytes (header and up to 233 bytes of
  text -- `Data.payload`'s limit in current firmware; it was 237).

As built these are compile-time constants (`mesh_model.h`), 46 KB of
static data in all; moving them to `zeitlos.cfg` waits for phase 4,
which is when the per-conversation views make the right numbers
visible.

Text is UTF-8 end to end. The system fonts cover ISO 8859-15 and the
Japanese font; anything else -- emoji, most notably, which Meshtastic
users send a lot of -- draws as the replacement glyph rather than as
garbage.

## Phase 3, as built

| file | what |
|---|---|
| `mesh_pb.h` | field numbers and the wire type each must arrive with, by hand |
| `mesh_model.c/.h` | node, channels, node list, message ring; lookups and names |
| `mesh_proto.c/.h` | FromRadio -> model plus an event; the ToRadio encoders |
| `mesh_session.c/.h` | handshake, nonces, timeouts, heartbeat, reboot, sending |
| `mesh.c` | the headless app: `serial0` transport, arguments, console output |
| `tests/test_session.c` | 86 checks against a simulated node |

The first four have no Zeitlos dependency; the host tests link them as
they are, and phase 4's window is one more caller.

**Checked against the real schema, once, outside the tree.** The
simulated node in `test_session.c` is built from `mesh_pb.h`, so on
its own it would only show the code agreeing with itself. So the
published schema was compiled locally with the reference protobuf
runtime (BSD) and used, in a scratch directory, both ways: fifteen
reference FromRadio messages covering every kind mesh reads -- ten-byte
negative RSSI, float SNR, an emoji in a long name, routing acks and
failures, telemetry, notifications, reboot -- all decoded correctly;
and every ToRadio mesh sends parsed back as exactly the intended
message. Nothing from that check is in the tree. A capture from a real
node (`tools/mesh_capture.py`) will add the missing third leg: real
firmware.

**Plausibility, measured.** `test_session.c` drops one byte at every
position of a whole config dump followed by six messages -- 730
positions -- and runs each through the session. 515 damaged frames were
rejected; **no damaged name or message text was believed**; the model
stayed within bounds every time (under ASan and UBSan too). A lost byte
still costs the frame it lands in and usually the one after it; that is
the floor for a stream with no CRC, and why the status line will show
the reject count.

The checks, all in `mesh_proto.c`: every known field must carry its
wire type; hop counts at most 7, channel below 8, RSSI between -200 and
0, coordinates within +-90/+-180, node number neither 0 nor broadcast,
payload at most 233 bytes. Messages are decoded into temporaries and
applied only if all of that holds. Strings are cleaned, not rejected:
invalid UTF-8 becomes `?`, control characters a space (a newline is
kept in message text).

**Acks.** A sent message starts `sending`. A ROUTING reply naming it
(`request_id`) moves it: success from the destination of a DM to
`delivered`; success from anyone else -- our own node's implicit ack,
or any ack of a broadcast -- to `sent`; an error to `failed` with the
reason ("no ack", "no route", "duty cycle limit"...). `delivered` is
never demoted by a late implicit ack.

One exception, found on hardware: **a broadcast whose error is
MAX_RETRANSMIT is `sent, no relay heard`, not failed.** For a
broadcast, want_ack means "listen for a neighbour rebroadcasting it";
the node transmits it regardless, retries, and reports MAX_RETRANSMIT
when no rebroadcast was heard. On a quiet mesh -- one other node, out of
range or not relaying -- that is every channel message, and showing
`[no ack]` said the message had not gone out when it had. A relay heard
later still upgrades it to `sent`. For a DM, MAX_RETRANSMIT stays
`failed: no ack`: the destination really did not answer.

**Details worth knowing.** The hop limit for our packets comes from the
node's own LoRa config (3 until it is known). An unnamed primary channel
is shown by its modem preset's name, as the phone apps do
("LongFast"). A node whose LoRa region is unset will not transmit;
`mesh` says so at connect. The node sometimes hands over a packet it
queued while no client was attached, twice: same sender and id is kept
once.

**Size.** `mesh.bin` is 92 KB (87 KB text, most of it newlib's printf,
which phase 4 drops) plus 46 KB of static data. The core itself is
about 9 KB.

## History

Every message -- received, or sent from the window or the command line
-- is appended to **`/user/mesh.log`**, and every later change in a
sent one's status after it (`sent`, `delivered`, failed and why). On
start, `mesh` reads the end of that file back, so the window opens on
the conversations as they were. `mesh_log.c` is the format, `mesh_log_io.c`
the file.

The file is **plain text**, one record per line, tab-separated, so it
can be read with `cat` and repaired with a text editor:

    # mesh log v1
    M  1758800100  db29e314  ffffffff  0  6a3b90c1  0  0  anyone near the river?
    M  1758800160  849b6ba0  11223344  0  1f00a2b3  1  0  yes, east bank
    S  1f00a2b3  3  0

(tabs shown as spaces). `M` is a message: time (epoch seconds; the
node's, or ours when the node's clock is not set), from, to (`ffffffff`
for a channel), channel, packet id, status (0 received, 1 sending,
2 sent, 3 delivered, 4 failed, 5 sent with no relay heard), error, and
the text with `\t`, `\n` and `\\` escaped. `S` is a status change for
the most recent `M` with that id. Node numbers and ids are 8 hex
digits, as the Meshtastic apps show them.

Decisions worth knowing:

- **Every write opens, appends and closes.** FatFs commits on close, so
  a record that was written is on the card; switching off between
  messages loses nothing. A few milliseconds per message is nothing at
  LoRa rates.
- **Loading is bounded.** Only the last 64 KB is read, however large the
  file, and only the newest 96 messages are held -- the model's ring.
  Starting mid-file, the partial first line is skipped, not counted as
  damage.
- **It cannot fill the card.** Past 256 KB the file is rewritten as just
  the messages held in memory. No big buffer is needed for that (the
  process has a 16 KB heap): the records are streamed out one by one.
- **A damaged line costs that line.** Anything that does not parse is
  skipped: a card pulled mid-write loses the record being written. Text
  read back is cleaned exactly as text off the air is, since a
  hand-edited file is no more trustworthy than a radio.
- **What was `sending` when mesh stopped is loaded as `sent, no relay
  heard`**: its ack can never arrive now.
- **History is never unread.** Conversations in it are ordered by their
  activity, but nothing loaded counts as new.
- **Duplicates.** The ids are in the file, so a packet the node replays
  after a restart (one it queued while no client was attached) is
  recognised as already held.
- **No card, or a read-only one**, and `mesh` says once, in the status
  bar, that history is not being saved -- and carries on.

`tests/test_log.c` (36 checks): every field round-trips, any chunking,
the worst-case line (233 backslashes) fits and reads back, status
records, and a stream with garbage, an overlong line and a missing final
newline around good records.

## User interface

A resizable wm window, keyboard-first, 1bpp like everything else here:

```
+-------------------+------------------------------------------+
| #LongFast         | #LongFast  (primary)  channel 0          |
| #LD             2 |------------------------------------------|
|-------------------| 10:50 e314: anyone on LongFast near ...? |
| ALI  Alice      1 | 10:51 me: yes -- Zeitlos here  [sent]    |
| e314 Meshtastic.. | 11:30 BOB: This is a long message that   |
| BOB  Bob the bu.. |   has to wrap across several lines of    |
|                   |------------------------------------------|
|                   | > Grüße € 5 an alle_                     |
+-------------------+------------------------------------------+
| 6ba0 !849b6ba0  fw 2.7.3.cf574c7  6 nodes           F1 help  |
+--------------------------------------------------------------+
```

**Left**, the conversations: the channels, a rule, then every node
(not our own) -- those with messages first, newest activity first, the
rest by when they were last heard. A channel reaches everyone on it; a
node is a direct message. Unread counts on the right of each row. The
selected one in inverse.

**Right**: a header saying what is shown -- for a node, its name, id,
hops (or "direct"), SNR, battery and when it was last heard; the
messages, wrapped, bottom-aligned, `HH:MM name: text` in local time
(`system.rtc.timezone`), our own marked `me` with their status in
brackets (`sending`, `sent`, `delivered`, or why they failed); and the
input line. The byte count appears near the 233-byte limit.

**Bottom**, the status bar: our node, the firmware, the node count --
or what is wrong: `serial` not running, no node on USB, the config
dump in progress, the LoRa region unset (the node will not transmit),
frames rejected by the plausibility check. Notices ("the node
rebooted", the node's own ClientNotifications) show there for four
seconds.

| key | |
|---|---|
| typing, Enter | send to what is shown |
| Up / Down | previous / next conversation |
| Tab | the next conversation with unread messages |
| PgUp / PgDn, wheel | scroll the messages |
| Left, Right, Home, End, Backspace, Delete | edit the line |
| Ctrl+W / Ctrl+U | delete a word / the line |
| Esc | clear the line; leave the log or help |
| F1 | keys |
| F2 | the node's console log (last 48 lines) |
| F4 | captions for direct messages on/off |
| Ctrl+Q, close icon | quit (telling the node, and `serial`) |

A click on a conversation shows it. A direct message arriving for a
conversation not on screen is shown as a caption at the top of the
desktop for four seconds (`zcaption.h`), until F4 turns that off. The
title bar carries the unread total: `mesh (3)`.

Text is UTF-8 throughout and laid out by display columns (`z_cp_width()`,
the renderer's own rule), so German, French and the euro sign draw as
themselves, Japanese in two-cell glyphs while `jfont` is running, and
anything else -- emoji, mostly -- as the missing-glyph box, never as
garbage and never splitting a character.

## Phase 4, as built

| file | what |
|---|---|
| `mesh_view.c/.h` | what is shown, decided without drawing: conversations and unread counts, word wrap, the input line. No Zeitlos dependency |
| `mesh_ui.c/.h` | layout and drawing, keys and mouse |
| `mesh.c` | the transport and the loop; the window by default, `nodes` / `send` / `dm` for scripts |
| `tests/test_view.c` | 45 checks |
| `tests/render.c` | the window drawn off the board: `make render WHAT=chat\|dm\|log\|help\|connecting\|noserial\|small` |

**The conversation shown is followed by identity, not position.** The
list reorders whenever something is said; the selection is "the DM with
!11223344", not "row 4". If what was shown disappears, the first entry
is shown instead.

**Unread counts live in the view, not the model**, so a reconnect --
which refills the node list and channels from a fresh dump -- keeps
them. Our own messages never count.

**Reconnecting keeps the node list.** It used to forget it along with
the channels; the message history then showed bare `!ids` until the
dump arrived. Now only the configuration is forgotten
(`mesh_model_forget_config()`); the dump updates every node anyway.

**Drawing is on demand**, by region: the sidebar, the header, the
messages, the input line, the status bar. A keystroke redraws the input
line only. During a config dump -- a hundred node infos in a row --
the conversation list is rebuilt once at the end, not per node.

**No printf.** `mesh.bin` went from 92 KB (phase 3, headless, with
printf) to 75 KB with the window. `-DZPORT_NO_PRINTF` removes
`zport.c`'s diagnostics, which were the last thing pulling the
formatter in; the command-line modes build their lines by hand and use
`puts()`.

**Rendered, and looked at.** `tests/render.c` draws the real
`mesh_ui.c` through `sw/common/tests/zrender.h` for seven states --
live on a channel with unread elsewhere, a DM with every ack status, a
line long enough to wrap, CJK and emoji names and text, the log, help,
the config dump in progress, `serial` missing, and a window resized to
300x200. Two things were found that way and fixed: the rule between
channels and nodes vanished under a selected first node, and a
reconnect turned every name in the history into an `!id` (above).

### Connecting without blocking

The first hardware run of the window showed an empty window until it
was dragged. The cause: `z_port_connect_arg()` waits up to two seconds
for serial's answer and **discards every other message meanwhile**
(zport.h says so), and `mesh` connected right after creating its window
-- so wm's first region and redraw were thrown away. With no node
plugged in it was worse, since each retry swallowed keys and redraw
requests too.

`mesh` now connects asynchronously: it sends CONNECT itself and takes
CONNECTED or REFUSED in its message loop like anything else; no answer
in two seconds means `serial` is not running. `z_port_connect*()` is no
longer linked. Belt and braces: the window also repaints whenever it
receives its first region, rather than trusting a REDRAW to follow.

Anything else in the tree that calls `z_port_connect*()` after creating
a window has the same hazard; `term` does it from a key handler, where
losing a redraw is less visible but not impossible.

### `serial`: dead clients no longer hold the device

A client that is killed never sends CLOSE, and `serial` kept the USB
connection marked "connected" to the dead pid -- every later client,
including a restarted `mesh`, was refused with "already connected"
until `serial` itself was restarted. `serial` now releases the
connection when a new CONNECT arrives and the old client is not
running (`z_proc_status()`), or when the same pid connects again (a
live client does not connect twice, and pids are reused). The same for
UART1. `mesh` itself does not rely on this: its close icon does not
kill it, and it says goodbye to the node and to `serial` on the way out.

## Risks

**1. No flow control from the ESP32 into the CP2102.** The node's UART
runs without RTS/CTS, the CP2102's receive buffer is a few hundred
bytes -- tens of milliseconds at 115200 -- and nothing stops the ESP32
talking when it is full. Bytes the host does not collect in time are
lost. USB-side backpressure (NAK) does not help, because the loss is on
the *other* side of the bridge. The biggest burst is the config dump:
a busy mesh's NodeDB is tens of kilobytes.

What keeps the host collecting: `serial` polls on the next tick while
data flows (about 1.4 ms, some forty times the rate needed), but it
**stops reading when its port to the client has eight unacked sends**
(`Z_PORT_MAX_PENDING_SENDS`). So `mesh` must ack every DATA as soon as
it has copied the bytes into its own receive ring, before parsing,
before drawing -- never after. That is a hard rule in the app, and the
phase 3 exit criterion measures it: a full dump from a node with a
large NodeDB, with the frame counter reporting zero resyncs.

**2. A corrupt frame believed.** No CRC means a dropped byte inside a
frame shifts everything after it, and protobuf is dense enough that the
shifted bytes often still parse. Defences: bounds-checked decoding;
a decoded message is applied only if every field's wire type matches
what that field number should carry (a wrong wire type is the most
common symptom of a shift); text payloads are validated as UTF-8;
the frame counter counts rejects, and a nonzero rate is visible in
the status bar rather than hidden.

**3. The auto-reset circuit.** See [DTR and RTS](#the-usb-side-cp210x).
Wrong here looks like "the node reboots every time Zeitlos sees it",
or worse, "the node sits in its ROM bootloader".

**4. Power.** A Heltec V3 transmitting at full power with its display
on draws more than a keyboard. Which Zeitlos boards can bus-power it
from their USB host port is to be measured in phase 0; a powered hub
is the answer where the port cannot.

**5. Firmware drift.** Meshtastic adds fields every release. Unknown
fields are skipped by design, so additions are harmless; the risk is a
field *changing meaning*, which is rare. The firmware version from
`metadata` is shown, and the version tested against is recorded in
this document.

## Phase plan

| phase | what | exit criterion |
|---|---|---|
| 0 | Bring-up and capture | Heltec enumerates on the Zeitlos host (`lsusb`: 10c4:ea60); descriptor, endpoints and bus power recorded here; a captured config dump and live traffic from a Linux host saved as test vectors |
| 1 | CP210x kernel driver -- **done, confirmed on hardware** (`make test_usb_serial`, 72 checks) | `usbserial` in a term window shows the node's console; unplug and replug recover; the node does not reset on bind |
| 2 | `zpb` codec and framer -- **done** (`make test` in `sw/apps/mesh`: 51 + 26 checks, also under ASan/UBSan with `make test_asan`) | host tests green: spec vectors, round trips, malformed and random input stays in bounds, resynchronisation after every kind of damage. Decoding *captured* frames moves to phase 3, which is where their meaning is |
| 3 | `mesh` core, headless -- **done off hardware** (86 host checks; wire format checked against the reference runtime) | connect, full dump, send and receive a channel message and a DM with a phone on the mesh, delivery status from ROUTING_APP, heartbeat, reconnect after node reboot -- all traced on the console (UART0); zero resyncs on a large dump |
| 4 | `mesh` window -- **done off hardware** (45 view checks; seven states rendered with `make render`) | the UI above; unread counts; log view; captions |
| 5 | Extras -- **started**: message history on the card (done off hardware, see [History](#history)) | message history on the sdcard; node positions and distance; battery/telemetry; TCP transport through `net` for a node on WiFi; setting our own names through AdminMessage |

Phases 1 and 2 do not depend on each other and can land in either
order. Phase 4 can start as soon as phase 3's model is fixed.

## Hardware test: history

1. `run mesh`; send a message or two. Close it, open it again: the same
   conversations and messages are there, with their statuses, and
   nothing is marked unread.
2. `cat /user/mesh.log` (or open it in `text`): one line per message,
   `S` lines after sent ones.
3. `run mesh send hello` from the shell, then open the window: that
   message is in the history too.
4. With no card in (or a read-only one): the status bar says history is
   not being saved, once; everything else works.

## Hardware test: phase 4

Needs this round's `mesh` and `serial` (and the round-1 kernel files, if
they are not committed yet).

1. `run serial`, then `run mesh` with no argument: a window, drawn
   straight away (the first run needed a drag -- fixed). Within
   seconds the status bar goes from "loading the node's
   configuration..." to your node, firmware and node count; the left
   side shows `#LongFast`, `#LD`, a rule, and `e314`.
2. Type a line on `#LongFast`, Enter. It appears as `me: ...
   [sending]`, then `[sent]` once a neighbour repeats it, or -- after
   the node's retries, about half a minute -- `[sent, no relay heard]`
   when none did. (The first hardware run showed `[no ack]` here; see
   "Acks" in [Phase 3, as built](#phase-3-as-built).)
3. Down-arrow to `e314` and send it a DM: `[delivered]`, or a reason.
4. Have something arrive (from a phone on the mesh, or from `e314`):
   on the channel shown, it just appears; elsewhere, an unread count on
   that row and `mesh (1)` in the title; as a DM elsewhere, also a
   caption at the top of the screen.
5. Umlauts and the euro sign from the keyboard layout, in the line and
   in what is sent (a phone shows them correctly).
6. F2: the node's console. Resize the window smaller and larger.
7. Close it with the close icon, and `run mesh` again straight away:
   it must connect (this is the `serial` fix above). Unplug the Heltec
   while it is open: "no Meshtastic node on USB"; plug it back: the
   dump again, the history keeping its names.
8. `run mesh send hi` still works from the shell, for scripts.

Worth reporting if it happens: a status bar showing `rejected N`
(frames failing the plausibility check -- the lossy-link risk), a
redraw you can see crawl, or a keystroke that lags.

## Hardware test: phase 3

**Done**: the node, firmware, channels and node list decoded
correctly on 2.7.3. Sending and receiving were not reported headless;
phase 4's test covers both.

Needs this round's kernel (the round-1 USB files) and `serial` as well
as `mesh`. With the Heltec on a USB host port:

1. `run serial`, then `run mesh`. Within a few seconds: the node's
   name and firmware, its channels, and its node list -- then silence
   until something is said on the mesh. Send the whole output if any
   of it looks wrong, and `run mesh log` if nothing arrives at all.
2. `run mesh send hello from zeitlos`. It should end with `[sent]` --
   the node heard a neighbour repeat it -- or, with no other node in
   range, `no word back in 30 s`, which is also a correct answer.
   Anyone with the Meshtastic app on the same channel should see it.
3. If there is another node you can reach: `run mesh dm SHORTNAME hi`.
   It should end `[delivered]`, or with a reason.
4. With `run mesh` running, have someone send to the channel (or send
   from the Heltec's own phone app, if it is paired). It should print
   as `[#LongFast] NAME: text (snr ..., rssi ...)`.
5. Unplug the Heltec while `run mesh` is running: "the node went
   away". Plug it back in: "reconnected", and the node list again.
6. At the end of any run, a line "link quality: N frame(s) rejected"
   appears only if something was damaged in transit. After a run with
   a large node list, that number is the one to report.

## Hardware test: phases 0 and 1

**Done** for phase 1: binds, console visible. Items 3, 5 and 6 below
(no reset on bind, unplug/replug, power) were not reported either way;
worth a glance during the phase 3 test. The capture (7) is still open
and still useful.

Needs the kernel and `serial` from this round. With the Heltec V3
running Meshtastic, plugged into a Zeitlos USB host port:

1. Boot log: `usb serial: cp210x, addr N ep in 1 out 1, mps 64/64`.
2. `lsusb`: `class=cdc (cp210x)` and `device 10c4:ea60`. If it shows
   another ID, or `state=FAILED`, send the whole `lsusb` output.
3. **The node must not reboot when it binds.** Watch its OLED: it
   should keep showing what it showed. If it restarts (or goes blank
   and stays blank -- the ROM bootloader), that is the DTR/RTS
   question in [The USB side](#the-usb-side-cp210x); try
   `CP_MHS_BOTH_CLEAR`.
4. `run serial`, then in a term window `usbserial`: the Meshtastic
   debug console scrolls past (log lines with timestamps). Nothing at
   all is also informative -- say so.
5. Unplug: the term window says the connection was closed by the
   other end. Replug and `usbserial` again: it works.
6. Power: does the board run from the Zeitlos port alone, including
   when it transmits (send it a message from a phone)? If it browns
   out, try a powered hub.

7. **Capture (phase 0), on a Linux PC, not Zeitlos:** with the node on
   the PC's USB,

       python3 tools/mesh_capture.py /dev/ttyUSB0 60 heltec.bin

   (needs `pip install pyserial`; close any Meshtastic app or CLI that
   has the port open). Send a message to the node from a phone during
   the minute. It prints a summary -- frame counts by kind, and whether
   the config dump completed. The `.bin` holds your node DB (names and
   positions of nodes you have heard), so send it privately rather
   than committing it; it becomes phase 3's test vectors, and its
   firmware version gets recorded here.

## Deliberately not done

- **Any radio or crypto on this side.** The node does it; see the top
  of this document.
- **Changing radio configuration** (region, modem preset, channel
  keys). The phone app and CLI exist for that, and a mistake there
  takes the node off the air. Read-only display of it is in scope;
  writing it is not, until someone needs it.
- **Firmware update of the node.** Same reason.
- **BLE.** There is no Bluetooth on Zeitlos.
- **FTDI, CH340, PL2303.** Each is its own vendor protocol
  (`docs/usb_host.md`, "CDC"). The CP210x work makes adding one a
  matter of a probe and a setup sequence, but none is needed here.
