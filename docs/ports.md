# Zeitlos Ports Design

**Status: phases 1-3 below are implemented** (`sw/common/zport.h`/`.c`,
`sw/apps/portdemo`, `term` wired to it). Phases 4-5 and "Open
questions" are still just plan. Written up ahead of the code
originally, the way `docs/networking.md`'s "Staged plan" section
tracks done vs. not-done work for that subsystem -- update this
document as the remaining phases land.

## Testing this

`portdemo` needs the fixed pid `Z_PID_PORTDEMO` (3) to be reachable at
all -- see "Protocol sketch" below for why fixed pids are how this
works right now. That pid is only guaranteed if `wm` (pid 1) and `net`
(pid 2) started first, in that order, which `sw/os/sh.c`'s `init`
shell command does automatically (it now starts `portdemo` right after
`net`, non-fatally if that binary's missing). **Use `init`, not
individual `run wm`/`run net` commands**, if you want `term` to
actually find `portdemo` -- manually `run`-ning things in a different
order (e.g. `run wm` then `run portdemo` without `net` in between)
will land `portdemo` on a different pid than `term` is hardcoded to
look for, and `term` will silently fall back to local echo instead
(not wrong, just not testing the thing you meant to test).

```
> init
> run term
```

Type something -- `portdemo`'s banner should appear on connect, and
everything typed afterward should echo back (byte-for-byte, so
backspace won't visibly erase anything against this specific demo --
see `sw/apps/portdemo/portdemo.c`'s own header comment on why that's
expected, not a bug).

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

1. ~~`zport.h`/`zport.c`~~ -- done. The protocol above, client +
   provider helper API, modeled on `zstream.c`'s style. One deviation
   from a plain "no timeout" RPC (`z_win_create()`'s own pattern): the
   client's connect blocks with a bounded ~2 second timeout, not
   forever, since a port provider (unlike `wm`) isn't guaranteed to be
   running at all.
2. ~~A demo virtual port app~~ -- done, `sw/apps/portdemo`
   (echo/banner, no hardware). Single connection at a time.
3. ~~`term` wired to the demo port~~ -- done, with a fallback: no
   port provider answering within the connect timeout means `term`
   still works standalone via local echo (phase 3's behavior),
   printing which mode it ended up in at startup.
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
