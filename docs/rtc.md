# RTC and time sync

Wall-clock time on Zeitlos: a counter in gateware (`rtl/rtc.v`), an
SNTP client in the net app (`sw/apps/net/ntp.c`), and a C API in
between (`sw/common/zrtc.h`).

## What was missing

`rtl/sysctl.v` has always had something called `rtc_ctr`, and it is not
a real-time clock. It is a 16-bit free-running counter whose wrap
raises the KTIMER interrupt, and what software gets from it —
`z_uptime_ticks()` — is ticks since boot at ~732Hz. That wraps every
~68 days and has no relationship to any date.

That is the right thing for measuring elapsed time and the wrong thing
for answering "what time is it", which until now nothing on this SOC
could do at all. Files had no timestamps, logs had no clock, and there
was nowhere for a date to come from.

The split is worth keeping straight, because using the wrong one is a
real bug and not an obvious one:

| question | use |
|---|---|
| how long since X, timeouts, retries | `z_uptime_ticks()` |
| what date is it, timestamps, clocks | `sw/common/zrtc.h` |

Timing a transfer with wall-clock time is how you get a negative
duration, because the wall clock can step backwards the instant an NTP
reply lands.

## The counter

`rtl/rtc.v` holds 32-bit seconds since the Unix epoch plus a 10-bit
sub-second field, both settable by software.

The prescaler is `SYSCLK / 1024` = **46875** cycles per sub-tick, which
at 48MHz is exact: 46875 × 1024 = 48,000,000 precisely. That exactness
is the reason the sub-second rate is 1024 and not 1000 — and it pays a
second time in `ntp.c`, where an NTP fraction (a binary fraction of a
second) converts to sub-ticks with a shift instead of a divide.

There is **no battery** on any board in this lineup, so the count
starts at zero on power-on and is lost on power-down. It survives
everything short of that — `wb_rst_i` comes from `sysctl.v`'s
`resetn_counter`, which releases once after the PLLs lock and never
asserts again, so a kernel restart, an app crash or reloading the OS
all leave the clock running.

Which is why `VALID` (register 3) exists rather than being implied by a
nonzero `SEC`. "It is midnight on 1 January 1970" and "nobody has told
me what time it is" are different states, and anything drawing a clock
needs to tell them apart. `sw/apps/clock` shows `--:--:--` for the
second one; displaying 1970 with total confidence would be worse than
displaying nothing.

### Register map

At `0x7000_03xx`, the fourth tenant of nibble 0x7 alongside `csrs.v`
(`0x7000_00xx`), `cache.v` (`0x7000_01xx`) and `socctl.v`
(`0x7000_02xx`). Same reasoning `socctl.v` gives for not taking a top
nibble of its own — `sysctl.v`'s map is full, and the one free nibble
(0x8) is the virtual window apps execute in, where a stale app pointer
dereferenced in kernel context would land on control registers.

| addr | reg | | |
|---|---|---|---|
| `0x7000_0300` | `MAGIC` | ro | `"ZRTC"` |
| `0x7000_0304` | `SEC` | rw | seconds since the Unix epoch, UTC |
| `0x7000_0308` | `SUB` | rw | read: sub-second, live. write: preload |
| `0x7000_030c` | `CTRL` | rw | bit 0 = VALID |
| `0x7000_0310` | `RATE` | ro | sub-ticks per second (1024) |
| `0x7000_0314` | `TZ` | rw | reserved: minutes east of UTC, unused |

### Optional, but on by default

`RTC` in `rtl/boards.vh`, defined at the **universal** level rather than
per-board. It needs no pins, no external part and no board support of
any kind — just a prescaler and a counter on `sys_clk` — so there is no
board-specific reason to want it or not want it. Every board gets one
unless somebody deliberately comments it out to reclaim the logic
(roughly a 32-bit counter, a 24-bit prescaler and a small register
file).

A build without it is not a build software has to be told about, and
that is the point of doing it this way. The FEATURE bit goes clear,
`z_rtc_available()` answers false, net skips its NTP client and the
clock app says on screen that this bitstream has no clock. Nothing hangs
and — unlike `CPU_MUL` — nothing has to be built differently on the
software side to match.

### The window stays decoded either way

**`0x7000_03xx` must be claimed by something whether or not the RTC is
built.** An address nothing decodes gets no ack at all on this bus and
the CPU waits for it forever; that is a dead hang, not a read of
undefined data.

So `sysctl.v` hands the window to `csrs.v` when `RTC` is off, exactly as
it already does with the cache window when `ICACHE` is off. `csrs.v` acks
anything in range, ignores writes, and reads back zero for offsets it
does not know — which is precisely the stub behaviour needed, and a zero
read correctly fails the MAGIC check.

With two optional tenants in one nibble that is four combinations, so
`cs_csrs` is written once as "the whole nibble minus whichever tenants
actually exist in this build" rather than as a separate expression per
case. Four hand-maintained cases would mean the one that mattered was
the one nobody tested.

That rewrite also closes a small pre-existing gap: on the current tree,
an `ICACHE` board leaves 256 words of nibble 0x7 decoded by nobody —
and they are exactly `0x7000_03xx`, the address the RTC now occupies.
`rtl/tb/tb_decode.v` walks every word in the nibble across all four
combinations and asserts each is claimed by exactly one tenant.

### Setting it atomically

A clock set needs to carry both a second and a fraction, and the bus
carries 32 bits at a time. So `SUB` is written first, staging a value,
and the `SEC` write adopts it and restarts the prescaler in the same
cycle — the new second begins exactly at the store.

The staged value is then **cleared**, so a later bare `SEC` write starts
at `.000` rather than inheriting a fraction from minutes ago. That is
the behaviour most likely to be broken by a later edit, and
`rtl/tb/tb_rtc.v` tests it specifically.

`z_rtc_set()` does both stores in the right order, which is why it is a
function rather than two stores at the call site.

Without this the NTP client would have to either throw the fraction
away or busy-wait for a second boundary.

### Reading it consistently

There is deliberately **no side effect on read**. Both registers are
live and nothing latches a snapshot, so a reader that wants a
consistent pair has to handle the second rolling over between the two
reads. `z_rtc_get()` does that: read `SEC`, read `SUB`, read `SEC`
again, retry if they differ.

Having a `SEC` read latch `SUB` into a shadow was considered and
rejected. It makes reads order-dependent in a way nothing else on this
bus is, and it breaks the moment two processes interleave their reads —
which on this system they can, since peripheral registers are not
arbitrated between processes (see `docs/gpu_blitter.md` on the same
hazard). The retry loop has neither problem and costs one extra load in
the overwhelmingly common case.

### No timezones: everything is UTC

The RTC counts UTC, NTP delivers UTC, `z_time_to_tm()` breaks down
whatever it is given without applying anything, and `sw/apps/clock`
displays UTC with the word `UTC` on screen. There is no conversion layer
and no local-time API.

That is a decision, not an omission. A timezone *offset* is easy; the
rules that produce the right offset are not, and there is no zone
database on this system and nothing in NTP that carries one. So the
choice was between a correct time honestly labelled and a plausible time
that is silently an hour out for half the year — and the second is a
worse thing to have on a wall clock than an unfamiliar one.

The `TZ` register at `0x7000_0314` is the storage for when this is
revisited: 16 bits of signed minutes east, sign-extended on read.
Minutes rather than hours because several real zones are not whole hours
off UTC (India is +5:30, Nepal +5:45, the Chatham Islands +12:45).

**Nothing reads or writes it**, and `zrtc.h` deliberately wraps it in no
accessors — a getter nothing calls and a setter nothing sets is exactly
the API that rots into being wrong. It is implemented in gateware rather
than left out so the slot is already in flashed bitstreams when a
timezone story arrives; adding it later would mean a reflash for what is
otherwise a software change. The hardware never consults it either way.

## Checking it is there — read this before using it

The RTC is optional, so software has to check. **Which probe it uses
matters**, because "no clock" arrives in two different shapes:

- **`RTC` off in this build** — `csrs.v` has the window, the read is
  acked and returns 0, the MAGIC check fails. Harmless.
- **bitstream predating `rtl/rtc.v`, on an `ICACHE` board** — nothing
  decodes that address, and an undecoded address on this bus **gets no
  ack at all**. The CPU waits for it forever. A dead hang, not a read of
  undefined data — the same hazard `rtl/cache.v` and `z_icache_flush()`
  already document.

So software gates on a CSR feature bit instead, at `0x7000_0008`, which
every bitstream ever built decodes. This is exactly the case
`rtl/csrs.v` was built for — its own header names `sw/apps/net` picking
an ethernet backend as the motivating example, and this is the same
shape of problem.

```c
if (!z_rtc_available()) {
    // no clock on this bitstream -- either `RTC is off in
    // rtl/boards.vh, or this build predates rtl/rtc.v
}
```

`z_rtc_available()` is the feature-bit check followed by the magic
check, in the safe order. Call it once, keep the answer, and only then
touch anything else in `zrtc.h`.

`Z_FEATURE_RTC` (bit 24) mirrors `RTC` in `rtl/boards.vh` the same way
every other feature bit mirrors its own define. Reaching the MAGIC check
at all means the bit was set, which means the window is decoded, which
means the read cannot hang.

## Time sync (SNTP)

`sw/apps/net/ntp.c` sets the clock from a public time server: once a few
seconds after the network is up, then hourly.

**Nothing waits for it and nothing depends on it.** A machine that never
manages a sync is a machine that does not know the date, not a broken
one. Unlike `dhcp.c` — which blocks once at startup because nothing else
can usefully happen until it finishes — this is a non-blocking poll in
net's main loop alongside `arp`/`tcp`/`dns`.

### Why hourly

The RTC is a counter on the same crystal everything else runs from, so
its drift is that part's tolerance — tens of ppm, call it a couple of
seconds a day. An hour keeps the error under a tenth of a second and
costs one 90-byte exchange. Daily would also be defensible; hourly is
cheap enough that there is no reason to choose it.

Failed attempts retry after 60s rather than an hour, because the
overwhelmingly likely reason for a failure at boot is that something
else is not ready yet (no DHCP lease, no nameserver, the link still
negotiating), and those resolve in seconds.

### Resolving the server name

The default server is `pool.ntp.org` — a hostname, not an address,
because that is what the pool is for: it hands back a different nearby
server each time it is resolved, which spreads load and survives any
single member being retired. Hardcoding one member's address would work
today and quietly stop working later.

That lookup **cannot** use `zdns.h`'s `z_dns_resolve()`. That function
blocks waiting for a reply from net, and this code *is* net — net's
message loop is what would have to produce the reply, and it is the
thing that is blocked. A guaranteed deadlock, and a subtle one, since
the call site looks perfectly innocent.

So `ntp.c` calls `dns_resolve_start()` directly, naming net's own pid as
the requester. The reply arrives as an ordinary
`Z_NET_DNS_RESOLVE_REPLY` in net's own mailbox — the kernel is happy to
deliver a process's message to itself, it is just a mailbox push
(`sw/os/msg.c`) — and net's message loop hands it to
`ntp_handle_dns_reply()`. A tag distinguishes it from a reply meant for
some other process. `dns.c` needed no changes at all.

A resolved address is kept for subsequent syncs, since `dns.c` has no
cache and a name that resolved an hour ago has almost certainly not
moved. It is dropped after a failed attempt, though — an unreachable
pool member is exactly the case where a fresh lookup gets a different
and possibly working address.

### The 2036 rollover

NTP counts seconds from 1900 and Unix from 1970; the difference is
2208988800, which is also very nearly where NTP's unsigned 32-bit
second count wraps — era 0 ends on 7 February 2036.

So the conversion has two cases, and the second is not hypothetical the
way most overflow handling is: it has a date. Getting it wrong produces
a clock that works perfectly until a specific Thursday, which is the
worst possible failure schedule, so it is handled rather than noted as a
TODO. (The RTC's own uint32 seconds then run out in 2106, and the
calendar code is good to there.)

### What gets rejected

Public servers are mostly well behaved, and "mostly" is doing real work
in a client with no authentication. Refused: a reply that is not mode 4;
one whose originate timestamp does not echo what was sent; stratum 0
(a Kiss-o'-Death packet, which is a server asking to be left alone and
specifically *not* a time source); stratum above 15; a leap indicator of
3 (the server says its own clock is unsynchronised); and a zero
timestamp. Each is a case where the packet parses fine and the number in
it is simply not the time.

The originate-timestamp echo is the only thing tying a reply to a
request. Without checking it, a late reply to a request three retries
ago would be accepted as the answer to the current one, which on a slow
link is not hypothetical.

A KoD backs off by the full hour rather than the short retry interval —
the server asked to be left alone, so leave it alone.

### Round-trip correction

The server's transmit timestamp describes the moment *it* sent, so by
the time it is being written the reply's own travel time has elapsed.
Assuming symmetric legs — which is what SNTP assumes and is close enough
on any normal path — that is half the measured round trip, and it is
added before the store.

### What is deliberately not there

No clock discipline: the RTC is stepped straight to what the server
said, never slewed. No stratum or dispersion arithmetic, no selection
between several servers, no KoD rate-limit state machine beyond
declining to use the packet, no authentication. This is one request to
one server every hour on a hobby SOC, which is the case plain SNTP was
specified for.

### Build options

In `sw/apps/net/Makefile`:

| variable | default | |
|---|---|---|
| `NTP_SERVER` | `pool.ntp.org` | hostname to resolve |
| `NET_STATIC_NTP` | `0.0.0.0` | pin an address, skip DNS entirely |
| `NTP_ENABLE` | `1` | `0` drops `ntp.o` from the link |

`NET_STATIC_NTP` is a standing override, not a fallback — if set, it
always wins and no lookup ever happens. Useful on a network with no
nameserver.

`NTP_ENABLE=0` is about not carrying the code; `ntp.c` also disables
itself at runtime on a bitstream without an RTC, so the switch is not
needed to avoid a crash.

## Asking for a sync

`sw/common/zntp.h` defines two message subjects, continuing `znet.h`'s
numbering (which ends at 305 — **the next subject added there must
start at 308**):

- `Z_NET_NTP_SYNC` (306) — sync now rather than at the scheduled time.
  Fire-and-forget: a sync is a round trip to a public server, and a
  caller that blocked on it would be blocking a UI on someone else's
  response time. Ignored if one is already in flight, so an impatient
  double-click costs nothing.
- `Z_NET_NTP_STATUS` (307) — replies with `enabled`, `synced` and `age`
  (in uptime ticks, because this asks how *stale* the clock is and the
  wall clock is the thing in question).

Nothing needs either of these just to **read** the time. The RTC is
memory-mapped hardware and `zrtc.h` reads it with a load instruction.

`sw/apps/clock` has no Sync button and sends neither: the system keeps
itself in step, so a button for it would do nothing observable in the
common case and would have nothing honest to report in the uncommon one.
The subjects exist for anything that genuinely needs to prod net.

## Calendar

`sw/common/zrtc.c` converts between Unix seconds and
year/month/day/hour/min/sec, using Howard Hinnant's `days_from_civil` /
`civil_from_days`. The trick in both is to shift the year so it starts
in March, which puts the leap day at the *end* of a year rather than the
middle and removes every special case from the month-length arithmetic.

Not newlib's `gmtime()`, for two reasons. It drags in newlib's timezone
machinery and a chain of locale-adjacent code, which costs several KB in
a binary whose main memory is a 1MB budget shared between every running
process. And `mktime()` interprets its input as *local* time using a
timezone this system has no notion of, so it is not actually the inverse
of what is wanted.

Proleptic Gregorian, no leap seconds — neither has the RTC, neither has
the NTP timestamp it is set from, and neither has Unix time itself, so
all three agree. Working range is 1970 to 2106.

Verified against `gmtime()` across the whole range, both directions.

Exposed to Scheme as `(current-time)` and `(current-date)` — see
`docs/scheme_api.md`.

## Testing

```
$ iverilog -o /tmp/tb_rtc rtl/tb/tb_rtc.v rtl/rtc.v && /tmp/tb_rtc
```

`CLK_HZ` is overridden to 10240 so a simulated second costs 10240
cycles instead of 48 million. Every ratio the block cares about is
preserved; the one thing it does not check is that 48MHz divides by
1024 exactly, which happens at elaboration.

The address decode has its own test, since it is the part with four
build combinations:

```
$ for a in "" -DICACHE; do for b in "" -DRTC; do \
      iverilog -o /tmp/t $a $b rtl/tb/tb_decode.v && /tmp/t; \
  done; done
```

It walks every word in nibble 0x7 and asserts exactly one tenant claims
each — nobody claiming it is a hang, two claiming it is a mux conflict.

## Flashing

The RTC is gateware, so it needs **`make flash`, not
`make dev-flash`** — and the same is true of turning it off, since
`RTC` in `rtl/boards.vh` only affects what gets synthesized.

Software built against it degrades gracefully on a bitstream without it
— net says "no RTC, ntp disabled", the clock app says so on screen —
with the one exception at the top of this document: a *pre-RTC*
`ICACHE` bitstream, where anything reading the RTC's own registers
without the feature-bit check first would hang.
