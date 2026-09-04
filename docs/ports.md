# Zeitlos Ports Design

**Status: phases 1-4 below are implemented** (`sw/common/zport.h`/`.c`,
`sw/apps/portdemo`, `term` wired to it, `sw/apps/net`'s telnet and ssh
providers, and `sw/apps/serial`'s UART1 provider). Phase 5 and "Open
questions" are still just plan. Written up
ahead of the code originally, the way `docs/networking.md`'s "Staged
plan" section tracks done vs. not-done work for that subsystem --
update this document as the remaining phases land.

## Testing this

`term` looks up `repl0` by name (`sw/os/pidreg.h`) and connects to
whatever pid that resolves to -- so `run repl` before `run term`, in
any order relative to `wm`/`net`, is enough for `term` to find it. The
fixed pid `Z_PID_REPL` (3, `sw/common/zrepl.h`) only matters as a
**fallback**, if name lookup ever fails (registry full, or repl
started before it managed to register) -- and that fallback is *only*
guaranteed correct if `wm` (pid 1) and `net` (pid 2) started first, in
that exact order, which `sw/os/sh.c`'s `init` shell command still does
automatically, for whichever code path ends up needing it. `init`
remains the easiest way to bring everything up in one step either way
-- it now starts `repl` itself too (in the same boot-order slot
`portdemo` used to occupy, see `init()`'s own comment):

```
> init
> run term
```

Type `help` -- `repl`'s banner should appear on connect, followed by a
`>` prompt, and typed characters (including backspace) should behave
like an ordinary line-oriented command prompt, not a raw echo (see
`sw/common/zline.h` for how `repl` does this on its end -- `term`
itself still only relays bytes, it does none of this). `ping`/`uptime`
are good next things to try; anything not recognized says so plainly
rather than pretending to understand it.

Scheme evaluation IS wired in now -- a bare word is a call, so `ps` is
`(ps)`. See `docs/scheme_api.md`.

To test the UART1 provider, on a board that has one
(`obst_uart_uart1`):

```
> init
> run serial
> run term
```

then `serial 9600` at the `repl` prompt, or F11 in `term` and type
`serial 9600`. F12 comes back. With nothing plugged into the port you
should see the provider's banner and then silence, which is the
correct result -- it proves the handover without needing a device on
the other end.

`portdemo` (`sw/apps/portdemo/portdemo.c`) is still there, and still
useful as a minimal test harness for the *port protocol itself* in
isolation from any command interpreter -- `run portdemo` starts it
manually (it's no longer started by `init`), but `term` won't find it
automatically anymore since it now looks for `repl0` first. Its own
header comment still accurately describes its byte-for-byte-echo,
no-line-editing behavior.

## Overview

The motivating app is `term`, a VT100/ANSI terminal emulator running
in an 80x25-character window. A terminal needs somewhere to send
keystrokes and somewhere output comes from -- a **port** is that
somewhere: a small, generic client/provider protocol so `term` doesn't
need to know or care whether it's talking to a real hardware UART, a
real telnet server over TCP, or a test harness. Example port
providers:

- A hardware UART port -- **done**, `sw/apps/serial`, over UART1
  (`docs/uart1.md`).

  NOT a wrapper over the `Z_SYS_UART_*` syscalls, which is what this
  bullet originally said. Those are UART0, and UART0 is the console:
  `sw/bios/bios.c` writes to it before anything else in the system
  exists, the kernel prints to it and `sh` reads from it. Handing it
  to a `term` window would take the machine's only diagnostic channel
  with it, and on a board where the console is the only I/O that is
  unrecoverable without a reflash.

  So the port provider is UART1, a second 16550 that exists only when
  a board builds it, reached through plain MMIO (`sw/common/zuart.h`)
  rather than a syscall. That also means `serial` exits at startup on
  a board without one, rather than staying resident to refuse every
  connection.
- A telnet client, riding on `sw/apps/net` (`docs/networking.md`) --
  **done**, see `docs/networking.md`'s "TCP + telnet" section. (This
  was originally sketched here as a "telnet-over-UDP proxy" gated on
  `net` gaining a message-based raw UDP API -- that was a placeholder
  written before either piece existed; what actually got built is a
  real TCP client, since telnet genuinely needs TCP, not a UDP-based
  workaround.)
- A demo virtual port (loopback/echo, canned banner) -- no real
  hardware needed, meant purely as a test harness for `term` and the
  port client API itself, built *before* any real provider.

This follows the same "an app owns a piece of hardware/resource and
mediates access to it via messaging" pattern `docs/window_manager.md`
(graphics) and `docs/networking.md` (the NIC) already established --
not a new architecture, a third application of the existing one.

## Why not `zstream.h`

`sw/common/zstream.h` (`docs/messaging.md`) already exists and is
well-tested (TFTP), so it's worth being explicit about why ports don't
just reuse it directly. `zstream` is a **pull-based, single-direction**
primitive: a consumer asks for exactly one chunk at a time from a
producer, which is an excellent fit for bulk transfer (a file, in one
direction, with natural backpressure from the pull rhythm itself). A
terminal port needs something with a different shape: **duplex and
bursty in both directions** -- keystrokes flow client→provider
whenever the user types, and output flows provider→client whenever the
remote end has something to say, neither side polling the other on a
fixed rhythm. Forcing that into two independent `zstream` pulls (one
per direction) is possible but adds real coordination complexity for
not much benefit at the traffic volumes involved here.

## Protocol sketch

Plain `z_msg_t` messages, not `zstream` -- fire-and-forget once
connected, deliberately simpler than `zstream`'s ack/pull machinery for
a first version:

```
CONNECT   client -> provider   tag=nonce   obj=Z_NONE (or provider-specific args)
CONNECTED provider -> client   tag=nonce   obj=Z_UINT32(conn_id)
REFUSED   provider -> client   tag=nonce   obj=Z_STR(reason)

DATA      either direction     tag=conn_id   obj=Z_BLOB (raw bytes)
DATA_ACK  either direction     tag=conn_id   obj=Z_NONE
CLOSE     either direction     tag=conn_id   obj=Z_NONE
```

The "provider-specific args" case above is now real, not just a
possibility left open in the sketch: `z_port_connect_arg()`
(`sw/common/zport.h`) lets a client supply CONNECT's payload
explicitly (`z_port_connect()` itself is now a thin wrapper passing
`Z_NONE`). First use: `sw/apps/net`'s telnet provider, which needs to
know a target IP *before* it can decide whether to accept a connection
at all -- see `docs/networking.md`'s "TCP + telnet" section, and
`sw/common/zterm.h`'s `Z_TERM_SET_PORT` for how that argument actually
gets from `repl`'s `telnet <ip>` command to `term`'s own
`z_port_connect_arg()` call.

Reachability now prefers name lookup over a fixed pid -- `sw/os/pidreg.h`'s
registry, `portdemo` registers as `portdemo0` and `term` looks it up --
with `Z_PID_WM`/`Z_PID_NET`'s original well-known-pid convention kept
as the fallback if that lookup ever fails. See "Testing this" above
for what that actually means for bring-up order in practice, and
`sw/os/pidreg.h`'s own header comment for the registry's design
(numbering, why it's not a permanent counter, the mutate-in-place
syscall convention it follows) -- there's no separate doc page for it
yet, that header comment is the actual source of truth.

`DATA_ACK` is also real now, not part of the original sketch: DATA's
`Z_BLOB` payload is a real heap allocation on the sender's side, and
`DATA_ACK` is how the receiver tells the sender it's safe to free once
it's genuinely done reading it -- see `z_port_send_ack()`/
`z_port_handle_ack()` (`sw/common/zport.h`) and `docs/messaging.md`'s
"Known limitations" for the full design writeup, including two real
follow-up bugs worth knowing about if this is ever touched again:

- The sender matches an ack to a pending send by FIFO ORDER
  (mailboxes are FIFO per sender/receiver pair, and every receiver
  acks exactly once, in read order), not by anything the ack itself
  carries -- an earlier version tried matching by the payload's data
  pointer instead, which never actually worked, since a pointer is
  only a meaningful value in the process that allocated it
  (`sw/os/msg.c`'s own header comment explains the cross-process
  addressing this ran into).
- That FIFO-order matching only holds if the ack itself reliably
  arrives -- `z_port_send_ack()` retries (bounded, `zport.c`) rather
  than fire-and-forgetting the ack the way every other send in this
  file does, since a lost ack doesn't just cost one missed
  notification, it permanently misaligns the FIFO for every send after
  it on that connection. Confirmed on real hardware as a receiving
  mailbox transiently full (the same keystroke-burst congestion this
  section's own "Flow control" discussion below already documents)
  right when an ack was being sent back.

This is strictly about memory lifetime, not the flow-control question
the next section covers -- see that section's own note on the
distinction.

### Flow control: an explicit, deliberate gap for v1

**`sw/apps/serial` is the first provider where this is not purely
theoretical.** A UART has a far end that does not wait: the 16550's
receive FIFO is 16 bytes, a scheduler slice is 1.365ms, and at 115200
that is 15.7 bytes of arrival per slice. So bytes CAN be lost, and are
lost in the wire's direction rather than in the port protocol's.

That is not this gap, though, and the distinction matters: the loss
happens between the wire and the provider, before any port message
exists. `serial` reports it from the 16550's own overrun bit rather
than hiding it, and no amount of port-level flow control would prevent
it -- the fix there is a deeper FIFO in gateware
(`rtl/esp32_rxfifo.v` is the precedent) or a slower baud rate. See
`docs/uart1.md`.


Fire-and-forget `Z_BLOB` messages have no backpressure beyond "the
mailbox is only `Z_MAILBOX_DEPTH` deep and `z_msg_send()` fails when
it's full" (`docs/messaging.md`). **Real-hardware finding: this
originally assumed "for interactive, human-typing-speed traffic...
that's plenty" -- it wasn't.** Typing a single short word into `term`
(connected to `repl`) could silently drop bytes/echo/response
partway through: `wm.c` sends two `Z_WM_KEY` messages per keystroke
(press + release, unbatched) straight to the focused window's
mailbox, each press round-tripping through `term` -> `repl` -> `term`
as a `Z_PORT_DATA` echo -- enough messages from a single short word to
exceed the original `Z_MAILBOX_DEPTH` of 8 with no crash, just quiet
data loss. `Z_MAILBOX_DEPTH` was raised to 32 (`sw/common/zmsg.h`) as
a direct fix once this was confirmed -- see that header's own comment
for the math. This is not the same as building the two-independent-
`zstream`s solution described below; it's just giving the existing
fire-and-forget mechanism enough slack for real interactive typing
speed, which is a different (and much cheaper) fix than the one this
section originally anticipated needing. The `zstream`-based approach
below remains the right answer for something that genuinely needs
real backpressure (a large paste, a chatty remote) rather than just
enough headroom for a burst of ordinary keystrokes.

**`Z_PORT_DATA_ACK` (see the protocol sketch above) is a related but
separate mechanism, worth not conflating with this section.** It
exists to answer "when is it safe to free a DATA message's payload",
not "how much can be in flight before something should slow down" --
see `docs/messaging.md`'s "Known limitations" for its own design
writeup. It does have a backpressure SIDE EFFECT (`z_port_send()`
refuses new sends once `Z_PORT_MAX_PENDING_SENDS` outstanding, unacked
sends pile up on one connection), which does give `zport` a real
memory ceiling it didn't have before -- but that cap is sized as a
safety valve against a peer that's stopped acking entirely (crashed,
or otherwise stuck), not tuned as this section's actual flow-control
answer, and doesn't address the "large paste/chatty remote" case this
section is about (ordinary acking keeps pace with ordinary traffic
long before that cap is ever approached). The `zstream`-based approach
below is still the plan for that, if it ever proves necessary.

For anything more than that -- a large paste over telnet, or a chatty
remote process -- the plan is to see whether the (now much larger)
simple version still shows real problems in practice before reaching
for something more structured -- e.g.
two independent `zstream`s per connection (one direction each), which
`net`'s existing async consumer API (`zstream_open_async()`/
`zstream_pull_async()`/`zstream_consumer_handle()`, built for TFTP PUT)
already has most of the machinery for. Deliberately not building that
up front: matches this project's own stated preference (see
`docs/networking.md`'s TFTP staged-bringup notes) for validating a
simple version against real use before adding the complexity a more
general solution would need.

## Planned phases

1. ~~`zport.h`/`zport.c`~~ -- done. The protocol above, client +
   provider helper API, modeled on `zstream.c`'s style. One deviation
   from a plain "no timeout" RPC (`z_win_create()`'s own pattern): the
   client's connect blocks with a bounded ~2 second timeout, not
   forever, since a port provider (unlike `wm`) isn't guaranteed to be
   running at all. `Z_PORT_DATA_ACK` (see the protocol sketch above)
   was added later, once sustained real-hardware use of the DATA
   channel showed the original "just leak every send" design running
   a process's heap out over a long enough session -- see
   `docs/messaging.md`'s "Known limitations" for the full story.
2. ~~A demo virtual port app~~ -- done, `sw/apps/portdemo`
   (echo/banner, no hardware). Single connection at a time.
3. ~~`term` wired to the demo port~~ -- done, with a fallback: no
   port provider answering within the connect timeout means `term`
   still works standalone via local echo (phase 3's behavior),
   printing which mode it ended up in at startup.
4. Real providers: ~~UART port~~ -- done, `sw/apps/serial` over UART1
   (`docs/uart1.md`), reachable via `repl`'s `serial [baud]` command
   or `term`'s F11 Open bar. One connection at a time, and here that
   is the point rather than a phase-1 limitation: there is one wire,
   and two `term` windows on it would interleave their keystrokes into
   one byte stream and split the replies between them at random.

   And telnet -- **done**,
   as a real TCP client (`sw/apps/net/tcp.c`/`telnet.c`), not the
   "telnet-over-UDP proxy" originally sketched in "Overview" above
   (see that section's own note on why the plan changed). Reachable
   via `repl`'s `telnet <ip>` command; see `docs/networking.md`'s "TCP
   + telnet" section for the design, and its own "getting back out of
   a telnet session" subsection for how `term`'s F12 key hands control
   back to `repl` once connected to a real remote (something
   `portdemo`/`repl` themselves don't need, since they can just answer
   a `quit`/`exit` command instead).
5. Revisit flow control (above) only if real usage shows the
   fire-and-forget gap actually matters.

## Choosing a port from the outside

`sw/common/zconnect.h` turns a target -- `port <name>`, `serial
[baud]`, `telnet <host>`, `ssh [user@]host` -- into the provider name
and scalar CONNECT argument this protocol carries. Both `repl`'s
commands and `term`'s F11 Open bar go through it, so there is one
implementation of each kind rather than two that drift.

See `docs/connections.md`.

## Open questions

- **Multiple concurrent `term` instances / port multiplexing.** Not
  addressed by the sketch above -- one provider, one client
  connection, for now.
- ~~A real name/pid registry~~ -- done, project-wide rather than a
  ports-specific workaround (as this section originally hoped for):
  `sw/os/pidreg.h`. `portdemo`/`term` migrated to it -- see "Testing
  this" and "Protocol sketch" above. Doesn't address the multiplexing
  question above on its own (a registered name still only ever maps
  to one pid at a time), just the pid-discovery half of the original
  limitation.
