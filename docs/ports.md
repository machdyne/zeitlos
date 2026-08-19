# Zeitlos Ports Design

**Status: phases 1-3 below are implemented** (`sw/common/zport.h`/`.c`,
`sw/apps/portdemo`, `term` wired to it). Phases 4-5 and "Open
questions" are still just plan. Written up ahead of the code
originally, the way `docs/networking.md`'s "Staged plan" section
tracks done vs. not-done work for that subsystem -- update this
document as the remaining phases land.

## Testing this

`term` looks up `lisp0` by name (`sw/os/pidreg.h`) and connects to
whatever pid that resolves to -- so `run lisp` before `run term`, in
any order relative to `wm`/`net`, is enough for `term` to find it. The
fixed pid `Z_PID_LISP` (3, `sw/common/zlisp.h`) only matters as a
**fallback**, if name lookup ever fails (registry full, or lisp
started before it managed to register) -- and that fallback is *only*
guaranteed correct if `wm` (pid 1) and `net` (pid 2) started first, in
that exact order, which `sw/os/sh.c`'s `init` shell command still does
automatically, for whichever code path ends up needing it. `init`
remains the easiest way to bring everything up in one step either way
-- it now starts `lisp` itself too (in the same boot-order slot
`portdemo` used to occupy, see `init()`'s own comment):

```
> init
> run term
```

Type `help` -- `lisp`'s banner should appear on connect, followed by a
`>` prompt, and typed characters (including backspace) should behave
like an ordinary line-oriented command prompt, not a raw echo (see
`sw/common/zline.h` for how `lisp` does this on its end -- `term`
itself still only relays bytes, it does none of this). `ping`/`uptime`
are good next things to try; anything not yet recognized says so
plainly rather than pretending to understand it (Scheme evaluation
isn't wired in yet -- see `sw/apps/lisp/lisp.c`'s own header comment
for where that lands).

`portdemo` (`sw/apps/portdemo/portdemo.c`) is still there, and still
useful as a minimal test harness for the *port protocol itself* in
isolation from any command interpreter -- `run portdemo` starts it
manually (it's no longer started by `init`), but `term` won't find it
automatically anymore since it now looks for `lisp0` first. Its own
header comment still accurately describes its byte-for-byte-echo,
no-line-editing behavior.

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

Reachability now prefers name lookup over a fixed pid -- `sw/os/pidreg.h`'s
registry, `portdemo` registers as `portdemo0` and `term` looks it up --
with `Z_PID_WM`/`Z_PID_NET`'s original well-known-pid convention kept
as the fallback if that lookup ever fails. See "Testing this" above
for what that actually means for bring-up order in practice, and
`sw/os/pidreg.h`'s own header comment for the registry's design
(numbering, why it's not a permanent counter, the mutate-in-place
syscall convention it follows) -- there's no separate doc page for it
yet, that header comment is the actual source of truth.

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
- ~~A real name/pid registry~~ -- done, project-wide rather than a
  ports-specific workaround (as this section originally hoped for):
  `sw/os/pidreg.h`. `portdemo`/`term` migrated to it -- see "Testing
  this" and "Protocol sketch" above. Doesn't address the multiplexing
  question above on its own (a registered name still only ever maps
  to one pid at a time), just the pid-discovery half of the original
  limitation.
