# zlink -- files and a shell between two machines

Two Zeitlos machines, two wires and a ground between their GPIO ports,
and no network: copy files either way, or open a shell on the other
machine from `term`. The link itself -- tiers, wiring, line code, speeds
-- is [zlink.md](zlink.md); this page is the `zlink` command.

```
a$ zlink start -w                     # on both machines; -w: lend my files, writable
b$ zlink start -f                     # -f: lend my files, read-only
a$ zlink status
zlink: up, stream at 12000 kbit/s, GPIO port 0 pins 0 and 1
  this machine is the primary
  the other machine: files to read, a shell, a stream engine
  ...
a$ zlink ls /
notes.txt  2310
pics/
a$ zlink get /notes.txt
/notes.txt: 2310 bytes in 0.1 s, 31 KB/s
b$ zlink put /report.txt /inbox/report.txt
```

and in `term`'s Open bar: `port zlink0` -- a shell on the other machine,
after its password.

**Status.** Round 2 of [zlink.md](zlink.md). Everything here is tested
on the host -- two copies of the program, linked by simulated wires
(Testing, below) -- and builds for the target, but has **not yet run on
hardware**.

## Commands

| Command | Does |
|---|---|
| `zlink start [-f \| -w] [-m auto\|soft\|stream] [-r KBIT] [PORT [X Y]]` | start the service on this machine |
| `zlink stop` | stop it, and let go of the pins |
| `zlink` or `zlink status` | the link, both machines' capabilities, counters |
| `zlink ls [PATH]` | the other machine's directory (default `/`) |
| `zlink get REMOTE [LOCAL]` | copy a file from the other machine; LOCAL defaults to the same path |
| `zlink put LOCAL [REMOTE]` | copy a file to it; REMOTE defaults to the same path |
| `zlink speed [KB]` | send KB (default 64) of filler and time it |
| `zlink loop [-i] [-r KBIT] PORT TX RX` | one board: send 64 KB out of TX and check it on RX -- a jumper between the pins, or `-i` for the engine's internal loopback |

`start`'s options:

| | |
|---|---|
| `-f` | lend this machine's files to the other machine, to read |
| `-w` | to read and write |
| `-m auto` | soft zlink first, then the fastest stream rate both engines manage (the default) |
| `-m soft` | soft zlink only |
| `-m stream` | an engine from the start, `X` the TX pin and `Y` the RX pin, wired TX to RX. For the LVDS Pmod, and for wires that cannot be pulled both ways |
| `-r KBIT` | the highest stream rate to try: 12000, 6000, 3000, 1500 or 750 (another number means the next one down) |
| `PORT X Y` | GPIO port and pins; default port 0, pins 0 and 1 -- the Pmod's pins 1 and 2 |

Paths are as the filesystem sees them: give them in full, from `/`; 200
bytes at most. The exit status is 0; 1 if something failed (the message
says what); 2 for a usage error.

For a second or so after the link comes up -- while the two machines
change from soft zlink to a stream engine ([zlink.md](zlink.md#from-soft-to-stream-round-2-done))
-- commands, requests and shells are refused with "busy -- changing to
a faster link; try again in a few seconds". Nothing may stall either
machine while the wires change hands.

## Who may do what

- **Files are opt-in.** A machine lends its files only if its zlink was
  started with `-f` (read) or `-w` (read and write); otherwise `ls`,
  `get` and `put` from the other machine are refused. Anyone with a
  cable to the machine gets what `-f` or `-w` gives -- there is no
  password for files -- so leave both off on a machine whose cable you
  do not control.
- **A shell needs the other machine's password** (the one `settings`
  sets, checked the way `netserve` checks it, with the same lockout
  after failures; three wrong in a row end the attempt). A machine
  with no password gives no shell at all.
- **Commands are for this machine.** `zlink0` refuses a command line
  from outside the machine (a telnet session's `netserve` relay); a
  term connecting from outside still needs to have logged in there, as
  every shell provider requires ([ports.md](ports.md#who-is-connecting)).

## The shell

In `term`, `port zlink0`. The other machine asks for its password -- not
echoed -- then connects you to its `posix` shell if that is running,
else to a running `repl`; with neither running it starts `posix`, or
`repl` if `posix` will not start (it needs about 4 MB). What you type
before the shell answers is kept for it (256 bytes). Close the term, or end the
shell, and the other side is told.

The shell sees the connection as **transport `zlink`, auth `system`**
in its identity map ([ports.md](ports.md#who-is-connecting)): the
password was checked, by the zlink service, the way netserve's
`system` means.

One shell each way at a time: this machine's term to the other, and
the other's term to this one, can both be open.

## Wiring

| mode | wires | how |
|---|---|---|
| auto, soft | X, Y, ground | X to X and Y to Y, or crossed: either works, and `zlink status` says which it found |
| stream (`-m stream`) | TX, RX, ground | each TX to the other's RX |

Soft zlink pulls wires low and lets pull-ups raise them: external
4.7k pull-ups to 3.3V on X and Y make it faster than the pins' own.
**100 Ohm in series in each wire** is cheap insurance against a wrong
bitstream or program putting two outputs on one wire
([zlink.md](zlink.md#pmod-to-pmod-direct)). Keep wires short for the
top stream rate; the rate ladder steps down by itself on longer ones.

## Speeds

| | |
|---|---|
| soft | estimated 10-35 KB/s; set by how fast both machines notice the wires |
| stream | at best the rate it settled on, less 8b/10b's 20% and framing: about 1.1 MB/s at 12 Mbit/s. Two frames in flight, and the CPU's work on each (CRC, reading the FIFO a byte at a time), may hold it lower |
| a file | also limited by the SD card: 600-850 KB/s reading ([sdcard.md](sdcard.md)) |

None of these has been measured on hardware yet; `zlink speed` is how.
While a transfer runs, the service does not sleep between frames, so it
takes most of a CPU's time -- over soft zlink also while any frame is
on the wires.

## How it works

One program, two roles. `zlink start` launches a second copy as the
**service** (`zlink serve ...`, which nobody types): it owns the pins
and the link, registers as `zlink0`, and serves this machine's
commands and the other machine's requests. Every other `zlink` command
is a **client**: it connects to `zlink0` with its command line as the
port connect's argument, and relays what comes back to the shell's
terminal, `\n` as `\r\n`. The exit status travels as the last two
bytes, `0x01` and the status digit.

The link layer ([zlink.md](zlink.md#the-link-layer-round-2-done))
carries messages of up to 448 bytes on channels:

| channel | | |
|---|---|---|
| 0 | control | `U` rate: an offer to change to a stream engine; `u` rate: yes; `n`: no ([zlink.md](zlink.md#from-soft-to-stream-round-2-done)) |
| 1 | requests | to the machine that serves files |
| 2 | responses | from it |
| 3 | shell in | `O` open, `D` bytes, `C` close |
| 4 | shell out | `D` bytes, `C` reason: closed |

Requests and responses carry an **id** after the type byte, so a
response to something abandoned (a `get` interrupted with Ctrl-C) is
recognised and dropped:

| request | response |
|---|---|
| `L` id path | `T` id text ..., then `K` id |
| `G` id path | `S` id size32, `D` id bytes ..., `E` id crc32 |
| `P` id size32 path | `K` id; then `D` id bytes ..., `E` id crc32; then `K` id |
| `A` id | (abandon whatever is running) |
| `B` id bytes ... `b` id | `R` id count32 -- the speed test |
| | `X` id message: refused or failed |

Numbers are little-endian; the CRC is zlink's CRC-32 over the whole
file. A file that arrives with the wrong CRC or size is deleted, on
either end, and the command fails. So is one cut short by the link
going down.

Flow control is the port layer's own: a `put` or `get` reads the next
chunk only when the link has room, and output to a slow terminal holds
back the link. In the other direction a shell's output (posix sends up
to 4 KB in one message) is read in place, a link message at a time,
and acknowledged only when all of it has gone -- so a shell that
prints faster than the link carries stops when eight of its messages
are waiting (`Z_PORT_MAX_PENDING_SENDS`). Three slots of the link's
output queue are kept for closes, aborts and refusals, so those never
wait behind data.

## Limits

- One `ls`/`get`/`put`/`speed` at a time per machine; a second is told
  so. `status` and `stop` always answer.
- Files, not directories: no recursive copy, no resume of an
  interrupted transfer, paths up to 200 bytes.
- Not started at boot by itself: `zlink start` it, or give cron the
  line `at boot  zlink start -w` ([cron.md](cron.md); with
  `wait_for_ntp: yes` it waits for the clock first).
- The service takes 32 KB (stack and heap) and about 31 KB of
  buffers.

## Testing

```
make -C sw/common/tests -f Makefile.zlink   # link layer and transports
make -C sw/apps/zlink/tests                 # this program, two machines, ~40 s
```

`test_zlink_app` runs two copies of the real `zlinkapp.c` as two
processes, sharing a block of memory that is their wires and their two
stream engines. Around each service it plays the kernel's messages, a
`zlink` command line, `term`, `posix`, a filesystem (a directory each),
the password, GPIO and the engine. The wires are modelled as wires:
either machine can pull either low, an engine drives its TX wire, and a
wire an engine drives reads as noise to soft zlink. Two drivers on a
wire, or soft pulling one an engine drives, fails the test at once.

| scenario | |
|---|---|
| 1 | soft only (one machine has no engine): `status`, `ls`, `get` and `put` both ways checked byte for byte, a `put` refused by a read-only machine, a missing file, a usage error, `speed`, a shell with a wrong then a right password (and the identity map the shell receives) that answers with one 4 KB message, all of which must arrive; a shell refused by a machine with no password |
| 2 | both have engines, crossed cable: the upgrade to 12 Mbit/s, 300 KB both ways, 1 MB of `speed`; then one machine stops and the other, after its quiet time, is back on soft, alone |
| 3 | the stream only works up to 3 Mbit/s: 12 and 6 fail, the ladder settles on 3 |
| 4 | `-m stream` on fixed pins: no soft at all |
| 5 | the secondary's answer to the first offer is lost: it goes onto its engine anyway, the primary goes quiet, and the next try works |

The wire check has been seen to catch a fault: with the quiet time on
the way back to soft removed, scenario 3 fails with "A pulls wire 0 low
while B's engine drives it".

The program was also reviewed line by line against the port protocol's
rules ([ports.md](ports.md)) -- which the test's simplified port layer
does not enforce -- and what that found is fixed: a shell's long output
was cut short, a stale upgrade offer could be accepted late, messages
for a link that had gone down were still acted on, and a few smaller
ones (an abort that could be lost, the speed count after an abort,
acks from a term that reconnected).

On hardware, to start: `zlink loop` with a jumper checks a board's
engine and pins; then two boards, `zlink start` on both, `zlink
status`, `zlink speed`.
