# Zeitlos Ports Design (planned, not yet implemented)

**Status: design only.** Nothing in this document exists in
`sw/common`/`sw/apps` yet -- no `zport.h`, no port-provider apps. This
is the plan `term` (see below) is meant to be built against once
phase 1 (keyboard input, `docs/user_input.md`) and phase 2 (`term`'s
standalone VT100 core) are done. Written up now, ahead of the code,
so the shape of the thing is settled before implementation starts --
update this document (and remove this notice) as pieces actually land,
the way `docs/networking.md`'s "Staged plan" section tracks done vs.
not-done work for that subsystem.

## Overview

The motivating app is `term`, a VT100/ANSI terminal emulator running
in an 80x25-character window. A terminal needs somewhere to send
keystrokes and somewhere output comes from -- a **port** is that
somewhere: a small, generic client/provider protocol so `term` doesn't
need to know or care whether it's talking to a real hardware UART, a
telnet-over-UDP proxy, or a test harness. Example port providers:

- A hardware UART port -- thin wrapper over the existing
  `Z_SYS_UART_*` syscalls (`docs/app_runtime.md`).
- A telnet-over-UDP proxy, riding on `sw/apps/net` (`docs/networking.md`)
  -- gated on `net` gaining a message-based raw UDP API first (already
  its own backlog item there).
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
CLOSE     either direction     tag=conn_id   obj=Z_NONE
```

Same well-known-pid convention `Z_PID_WM`/`Z_PID_NET` already use
(`docs/messaging.md`'s "no dynamic pid discovery yet" limitation
applies here too) -- a port provider runs at a documented, fixed pid,
started before any client that wants to reach it.

### Flow control: an explicit, deliberate gap for v1

Fire-and-forget `Z_BLOB` messages have no backpressure beyond "the
mailbox is only `Z_MAILBOX_DEPTH` deep and `z_msg_send()` fails when
it's full" (`docs/messaging.md`). For interactive, human-typing-speed
traffic (or a demo loopback) that's plenty -- but it is *not* the same
safety `zstream`'s pull rhythm provides for something like a large
paste over telnet or a chatty remote process. The plan is to ship the
simple version first, see whether that gap ever actually matters in
practice, and only then reach for something more structured -- e.g.
two independent `zstream`s per connection (one direction each), which
`net`'s existing async consumer API (`zstream_open_async()`/
`zstream_pull_async()`/`zstream_consumer_handle()`, built for TFTP PUT)
already has most of the machinery for. Deliberately not building that
up front: matches this project's own stated preference (see
`docs/networking.md`'s TFTP staged-bringup notes) for validating a
simple version against real use before adding the complexity a more
general solution would need.

## Planned phases

1. `zport.h`/`zport.c` -- the protocol above, client + provider helper
   API, modeled on `zstream.c`'s style (thin wrappers, no hidden
   blocking on the provider side).
2. A demo virtual port app (echo/banner, no hardware) -- the test
   harness for everything else here, built and exercised before any
   real provider.
3. `term` wired to the demo port instead of local echo, once its
   standalone VT100 core (phase 2, see the top-level project plan) is
   working against a hardcoded test byte stream.
4. Real providers: UART port, then telnet-over-UDP (gated on `net`'s
   own UDP API backlog item).
5. Revisit flow control (above) only if real usage shows the
   fire-and-forget gap actually matters.

## Open questions

- **Multiple concurrent `term` instances / port multiplexing.** Not
  addressed by the sketch above -- one provider, one client
  connection, for now.
- **A real name/pid registry** would help here the same way it would
  help `docs/messaging.md`'s existing "no dynamic pid discovery"
  limitation generally -- worth building once, project-wide, rather
  than inventing a ports-specific workaround.
