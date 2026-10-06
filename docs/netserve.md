# netserve: network servers

SSH and telnet into Zeitlos, a web server for static files, and an
echo service for testing the path.

**Status: telnet, HTTP and echo run on a board (RMII), after the fixes
in "Found on hardware". SSH -- password and keys -- is host-tested
against the real client engine and not yet reported from a board.
Listener lists and `noauth` ("noauth", below) are host-tested and not
yet reported from a board.**

```
peer --TCP-- net --port-- netserve --port-- repl0 / posix0 / console0 / serial0
```

| | |
|---|---|
| `sw/apps/net/tcp.c` | accepting connections: a pool, listening (networking.md, "Connections") |
| `sw/apps/net/relay.c` | net's side: each accepted connection relayed to its listener |
| `sw/apps/net/netserve/` | the servers; `authkeys.c` reads `/user/authkeys` |
| `sw/apps/net/ssh/ssh_server.c` | the SSH protocol, the other half of the client's `ssh_proto.c` |
| `sw/common/znet.h` | `Z_NET_LISTEN`, and what an accepted connection carries |

**Ethernet boards only.** On a board networked through the ESP32 link
(the ULX3S), the ESP32 translates addresses for Zeitlos, and nothing
from the LAN can connect in ([esp32link.md](esp32link.md)): the servers
start, and nobody reaches them.

## Configuration

In `/zeitlos.cfg` ([config.md](config.md)). Everything is **off** until
set.

```
apps.netserve.ssh: 22 posix0
apps.netserve.telnet: 23 repl0
apps.netserve.http: 80 /www
apps.netserve.echo: 7
apps.netserve.allow: subnet
```

| key | | |
|---|---|---|
| `apps.netserve.ssh` | `off` | one or more **listeners** (below); `22 posix0` by default. Needs the same password -- and a seeded TRNG |
| `apps.netserve.ssh_auth` | `both` | `both`, `key` (only keys in `/user/authkeys`; no password needed at all) or `password`. Not for `noauth` listeners, which take anyone |
| `apps.netserve.ssh_sessions` | `2` | SSH sessions at a time, 1 to 4; about 10 KB of netserve's memory each ("Cost", below) |
| `apps.netserve.telnet` | `off` | one or more **listeners**: a port, then a port name to connect sessions to -- `repl0`, `posix0`, `console0`, `serial0`, `bbs0`, or any zport provider. The port defaults to 23, the name to `repl0` |
| `apps.netserve.http` | `off` | a port, then the directory to serve: `80 /www`. No password: it serves files, read-only |
| `apps.netserve.echo` | `off` | a port (7): everything sent comes back. No password; for testing |
| `apps.netserve.allow` | `subnet` | `subnet` accepts connections only from this machine's own subnet; `any` from anywhere. The default for every listener; one can say otherwise |

### Listeners

`telnet` and `ssh` each take a **list**, entries separated by
semicolons, up to four:

```
apps.netserve.ssh: 22 bbs0 noauth any; 2222 posix0
apps.netserve.telnet: 23 bbs0 noauth any
```

Each entry is a port, then optionally a port name and flags, separated
by spaces, the flags in any order:

| word | |
|---|---|
| a number | the TCP port; the service's default (22, 23) if there is none |
| a name | the port name sessions connect to; the service's default if there is none |
| `noauth` | **no password.** The session goes straight to the port name, which must log its users in itself -- see "noauth" below |
| `any` | accept from anywhere, whatever `apps.netserve.allow` says |
| `subnet` | accept from this subnet only, whatever `apps.netserve.allow` says |

The one-entry form everyone already has -- `22 posix0` -- is simply a
list of one.

**Anything not understood refuses that entry, and the console says
which and why**: an unknown word (`noath`), a second name, a port that
is not a number or not 1-65535, a port another entry or service
already has, a fifth entry. A typo in a flag must never quietly become
a different policy. The other entries are unaffected.

**Semicolons, not commas:** commas are legal in FAT file names, so a
comma could one day be part of a value; a semicolon cannot.

**The password rule is per listener.** One that asks for the machine's
password stays off, saying so, while that password is missing or
shorter than 10 characters; a `noauth` listener in the same list runs.

`init` starts `netserve` at boot when a service is set; otherwise `run
netserve`. It exits at once, saying so, if nothing is.

**Telnet refuses to start without a password of 10 characters or
more** ([security.md](security.md)): it hands out a shell, and its
password crosses the network in the clear. Set one with `passwd` at
the console, or in settings. The console says why telnet stayed off.

## noauth

For programs that do their own logins -- a BBS ([bbs.md](bbs.md)) --
netserve can carry sessions without asking for anything:

```
apps.netserve.telnet: 23 bbs0 noauth any
apps.netserve.ssh: 22 bbs0 noauth any; 2222 posix0
```

**Telnet:** no banner, no password prompt. netserve offers WILL ECHO
and WILL SGA as always, and connects: the first thing the caller sees
is the port's own screen.

**SSH:** the session is **encrypted as ever**, and the host proves who
it is with its key as ever; the *user* proves nothing. Whatever the
client tries first gets in:

| the client sends | answer |
|---|---|
| `none` -- OpenSSH, PuTTY and Dropbear send this first | SUCCESS: in, with no prompt |
| `password`, any password | SUCCESS. The password is thrown away, never logged |
| `keyboard-interactive` | SUCCESS, with no questions |
| `publickey`, asked about | PK_OK, as for a listed key: a client asking a question expects that answer |
| `publickey`, signed -- any key | SUCCESS; the signature is not checked, since nothing depends on it |
| anything else | SUCCESS |

Some clients, notably ones built on SSH libraries, skip `none` and
always send a password; accepting any password is what lets them in.
The user name the client sent is passed on ("Who is connecting",
below), so a BBS can offer it at its own login prompt.

The console log says so on every login:

```
netserve: ssh (noauth) session 1 from 93.184.216.34
netserve: session 1: ssh: user 'visitor' logged in (none, unauthenticated)
```

### Who is connecting

Every session's CONNECT to its port carries a map saying who it is
(zport.h, "Who is connecting"; [ports.md](ports.md#who-is-connecting)):
the transport (`telnet`, `ssh`), `auth` -- `system` if the machine's
password or a listed key was checked, `none` from a `noauth` listener --
the peer's address, the local port, and for SSH the user name the
client sent. A user name is a claim, never a proof.

### What keeps `noauth` safe

**A shell refuses it.** `repl`, `posix`, `console` and `serial` refuse
any session whose `auth` is not `system`, and say so on the console:

```
posix: refused an unauthenticated telnet session from 93.184.216.34 (a noauth listener -- docs/netserve.md)
```

The caller is told `the port refused: this port needs a logged-in
session (not noauth)`. So `2222 posix0 noauth` -- a slip of the pen, or
a copied line -- opens nothing: it fails closed, in the program that
would have handed out the shell, without netserve having to know which
ports are shells.

A program written for `noauth` sessions must do the opposite: log each
caller in itself, and treat the map's user name as a suggestion.

**Everything else about the listener still applies**: the subnet rule
(which is why the examples say `any` -- a public BBS wants the
internet), the session limits, the login timeout for SSH.

## A telnet session

```
$ telnet 192.168.1.50

Zeitlos -- telnet is not encrypted: use it on a network you trust.

password:
connecting to repl0...
> ps
```

- **The password** goes through the kernel's check ([security.md](security.md)):
  the same one tally of failures as the lock screen, so the delays
  after repeated failures apply to telnet too. Three attempts per
  connection, 60 s to log in. Nothing typed at the prompt is echoed.
  Each attempt is logged on the console with the peer's address.
- **The target** is started if it is not running -- `repl` and `posix`
  are, like term's REPL and POSIX buttons -- once for however many
  sessions are waiting on it.
- **Telnet options:** netserve offers WILL ECHO and WILL SGA, the
  pair that means "character at a time, the server echoes" -- which is
  what the ports expect, since `term` does no local echo either. Every
  other option is refused, once: never answering an option twice is
  what keeps negotiation from looping. If the client refuses our echo
  (DONT ECHO) it echoes for itself, password included; the banner has
  said the line is not private anyway.
- **Enter** arrives as CR LF, CR NUL or a bare LF depending on the
  client; the port always gets one CR, which is what `zline` takes as
  Enter. A literal 0xFF is `IAC IAC` on the wire both ways.
- **Leaving:** `quit` in repl closes the port, and netserve closes the
  connection after the last output has gone. Closing the telnet client
  closes the port.
- **Full-screen programs** under posix -- `vi` -- work as they do in
  `term`, by the same **terminal handoff**. posix sends its client
  `Z_TERM_SET_PORT` naming the child's port; netserve, like term, leaves
  posix (CLOSE) and connects the session to the child. When the child
  exits, posix calls the session back with another `Z_TERM_SET_PORT`,
  and netserve obeys it **in any state**, as term does: `vi` exits
  without closing its port, so a session still attached to it is
  simply let go -- its unacked sends freed (`z_port_forget()`, zport.h)
  -- and reconnected to posix, with what was typed meanwhile. A child
  that does close first leaves the session waiting for the call-back,
  and it goes back by itself after ten seconds if that never comes. Acks still due
  from the port it left are received and freed, not leaked. The message
  names no session, so it is taken for the one that typed most
  recently; two sessions starting `vi` under one posix at the same
  moment could be confused, as posix itself matches a returning
  terminal by pid.

## SSH

```
$ ssh phil@192.168.178.154
phil@192.168.178.154's password:
posix -- a Unix-shaped shell for Zeitlos
$
```

The same sessions as telnet -- a port, `vi` and the terminal handoff,
the backend started on demand -- encrypted, and with the password
never on the wire. `sw/apps/net/ssh/ssh_server.c` is the protocol;
netserve binds it to a session.

- **One suite**, the client's (docs/ssh.md) from the other side:
  curve25519-sha256 key exchange, an ssh-ed25519 host key,
  chacha20-poly1305@openssh.com. Every current OpenSSH, PuTTY and
  Dropbear client has all three.
- **Strict KEX** whenever the client offers it (OpenSSH 9.6 and later),
  which is the fix for the **Terrapin** attack (CVE-2023-48795) against
  exactly this cipher: sequence numbers reset at each NEWKEYS, only key
  exchange messages during the first one, and the client's KEXINIT
  must be its very first packet.
- **A key or the password** (`apps.netserve.ssh_auth`; see "SSH
  keys" below) -- or nothing, on a `noauth` listener ("noauth", above). The password is the machine's, through the kernel --
  the same tally of failures and the same delays as the lock screen
  and telnet. Any user name: this is a single-user system, and the
  name is logged. Three wrong passwords or bad signatures, or 60 s
  without logging in, and the connection is closed.
- **One session channel** per connection: `pty-req` and `shell`.
  `exec`, `subsystem` -- so sftp and scp -- and forwarding of any kind
  are refused.
- **Flow control:** the window offered to the client is exactly the
  1 KB the session holds for it, re-opened only as bytes reach the port;
  what goes to the client never exceeds its window.
- **Rekeying** the client asks for is done; channel data waits for it.

**The host key** is a 32-byte Ed25519 seed in the flash key/value store
(`apps.netserve.hostkey`, [kvstore.md](kvstore.md)), made from the TRNG
on the first start and kept -- so it survives a new card, and a client
warned once is not warned again. Its fingerprint is on the console at
every start:

```
netserve: ssh host key SHA256:4ADgAqIUtDClAPaRQO8ax85ZPlUk7o/vfGoisvYx7JQ
```

Compare it with what `ssh` shows on the first connection. Any app can
read the store (docs/security.md), so the key is exactly as private as
the code this machine runs.

**SSH stays off** -- and says why on the console -- without a password
of 10 characters or more (unless it takes keys only), without a seeded TRNG (an ephemeral key from
a weak source would expose every session to anyone who recorded it),
or if the host key cannot be stored (a new key every start would teach
users to click through the warning).

**Cost:** the key exchange is three curve25519 operations and an
Ed25519 signature, about a second or two of this CPU, during which
netserve's other sessions wait. netserve is about 155 KB of code with
SSH and its key checking in (RSA verification comes from `web`).

**SSH sessions** come from a pool allocated when netserve starts,
`apps.netserve.ssh_sessions` of them -- 2 by default, 4 at most -- each
about 10 KB: the engine (~6.8 KB, most of it its 4 KB receive buffer),
2 KB of output waiting for room toward the peer, and the 1 KB the
client may send before the port takes it. The pool is on the heap, not
in `.bss`, so a machine with no SSH listener pays nothing for it. A
session over the limit is refused (`netserve: too many ssh sessions`);
if the memory is not there, netserve starts with as many as fit and
says so. netserve's memory tier is LARGE (64 KB of stack and heap,
`sw/os/kernel.h`): moving the default pool out of `.bss` shrank the
image by 19 KB, so the default costs about 13 KB more than before.

## SSH keys

```
$ scp ~/.ssh/id_ed25519.pub ...        # or tget it, or type it in te
```

`/user/authkeys` lists the keys that may log in, in OpenSSH's
`authorized_keys` format -- a `.pub` file's line as it is, one per
line:

```
# phil's machines
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIB5... phil@laptop
ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC... phil@desktop
SHA256:TzWWthIJBdRR6Kh1nInJtskY417iZBfXfvGcLJ7/wMQ
```

- **Ed25519 and RSA** keys, RSA from 2048 to 4096 bits.
  `rsa-sha2-256` signatures; the old SHA-1 `ssh-rsa` signature is
  refused, as OpenSSH itself now refuses it.
- **A fingerprint** -- `SHA256:...` as `ssh-keygen -lf` prints it, or
  64 hex digits -- lets that key in, whatever its type: the client sends
  the whole key when it logs in, and a key whose hash matches IS the
  listed key. Handy for a key too long to type.
- **Refused and reported** on the console, once per version of the
  file: a line with OpenSSH options before the key (`from=`,
  `command=`, `no-pty` ...) -- ignoring a restriction would let in more
  than the file says -- an RSA key outside 2048-4096 bits, and anything
  malformed. The console also says how many keys were read.
- **Read at every login**, so an edit takes effect at once. No card, no
  file, no key logins -- the password still works if it is allowed.
- **Every key login is logged** with the key's fingerprint.
- **Server-sig-algs** (RFC 8308) is sent to a client that asks for it,
  as OpenSSH does: without it OpenSSH will not offer an RSA key.
- A client with several keys asks about each before signing with one;
  a key not in the list does not count as a failed attempt. A listed
  key with a bad signature does.

An RSA check takes a few seconds of this CPU, an Ed25519 one about a
second -- the reason to prefer Ed25519 keys here.

`apps.netserve.ssh_auth: key` makes the server take keys only: no
password is offered, and none needs to be set. That is the setting to
use if the machine is ever reachable from outside the local network.

## The web server

```
$ curl http://192.168.178.154/
<h1>home</h1>
```

Static files from one directory on the card, read-only.

- **GET and HEAD.** Anything else is `405`, with `Allow: GET, HEAD`.
- **One request per connection** (HTTP/1.0 in effect, `Connection:
  close`): the answer, then the close. A browser simply opens another.
- **A directory** gets its `index.html`, else `index.htm` -- with or
  without the trailing slash. There are no directory listings.
- **Content-Type** from the extension, in any case: html, htm, txt,
  md, css, js, json, png, jpg, jpeg, gif, svg, ico, pdf, wasm; anything
  else is `application/octet-stream`.
- **Nothing outside the root.** The path is %-decoded first, then
  refused if any segment is `..`, or it has a backslash or a control
  character; `%00` is refused outright. The query string and fragment
  are ignored.
- **Limits:** 1 KB for the request line and headers (`431`), 160
  characters of path (`414`), 10 s to send the request (`408`). A
  client that half-closes before the request ends gets `400`; one that
  half-closes after it is answered in full.
- **The file streams** from the card a chunk at a time as room appears
  -- never whole in memory -- and its handle is closed the moment it is
  done. Handles come from the kernel's table of eight for the whole
  system, so a request that finds none free is refused.
- **Every request is logged** on the console: address, request line,
  status, bytes.
- **The subnet rule** (`apps.netserve.allow`) applies here too.

Throughput is TCP's: one segment of up to 536 bytes in flight at a
time (networking.md, "Known limits"), so a page arrives at LAN speed
for small files and a few tens of KB/s for large ones. A client that
delays its ACKs (Windows, 200 ms) makes large files slow. Sending two
segments at a time, and full-size ones, is the open item that would fix
that (networking.md).

## Half-close

A client may finish SENDING and still want the answer: `nc` sends its
FIN as soon as its input ends, and some HTTP clients do after the
request. TCP calls this half-close. net's relay opts in
(`tcp_set_half_close()`, tcp.h): the peer's FIN arrives as
`TCP_EVENT_EOF`, the connection stays open the other way, and the
listener hears `Z_NET_EOF` (znet.h) once every byte before the FIN has
reached it. netserve then finishes each kind its own way -- echo sends
back the rest, HTTP answers a complete request (or `400`s an
unfinished one), telnet passes the last of its input on -- and closes.
Only then does the board send its own FIN.

Without it, the first board echo test lost everything still in flight
when `nc`'s FIN arrived (see "Found on hardware").

## How it works

### net: accepting and relaying

A process asks `net` to listen (`Z_NET_LISTEN`, [znet.h](../sw/common/znet.h)).
A SYN to that port takes a TCP slot and a **relay** (`relay.c`); once
the handshake completes, `net` offers the connection to the listener
as a zport CONNECT -- the listener is the provider, `net` the client --
carrying the peer's address, the ports, and whether the peer is on
this subnet. The listener answers CONNECTED or REFUSED **with the same
tag**, which `z_port_accept()` does not do (it answers with tag 0): a
listener with several connections arriving at once has to say which it
means. The number it sends is its name for the connection, and
that name is not reused while a relay might still hold it
([networking.md](networking.md), "Accepted connections"). netserve's
names count up and skip 0; they are not the session slot.

Six relays, one per inbound TCP slot (`TCP_MAX_CONN` less the two kept
for outbound). Each has a 1 KB buffer toward TCP and 512 bytes from it,
and the flow control runs both ways:

- **Peer to listener.** TCP has already acked what it delivered, so
  dropping is never an option. The advertised window is what is left
  of the 512 bytes -- set before the SYN-ACK goes out, so even the
  peer's first burst fits -- and it closes while the listener falls
  behind, reopening (announced) when it catches up.
- **Listener to peer.** DATA that does not fit is held, **unacked**.
  Its payload stays valid in the listener's memory until then (zport),
  and a listener with eight sends unacked stops sending. Acks go out as
  the held data moves on.

When the listener closes, what it sent last goes out first and then the
FIN. `net` hands the listener's messages to the relays before any of
its own handlers see them: those match DATA by tag alone and would
claim a relay's.

Listening is last-writer-wins: a restarted `netserve` reclaims its
ports, and the old owner's connections are reset.

### netserve: many sessions, one loop

One process, up to six sessions, and a loop that never blocks on one of
them. That is why **every connect is asynchronous**: zport's
`z_port_connect()` waits for the answer and discards every other
message meanwhile, which would lose another session's data. A CONNECT
to `repl0` is answered with tag 0 like any other, so the answer is
matched to the oldest open CONNECT to that provider -- providers answer
in order.

**A CONNECT outlives its session.** A provider answers every CONNECT,
in order, whether or not the session that sent it still exists. A
session that ends while its CONNECT is unanswered keeps its slot until
the answer comes (or the 15 seconds a connect is given), and a late
CONNECTED is closed at once. Otherwise that answer went to the next
session waiting on the same provider -- handing it a connection meant
for the dead one -- and the session's own answer then went nowhere,
leaving a slot held in the provider. The wait also keeps the identity
map valid: the provider reads it from netserve's memory after the
CONNECT is sent ([messaging.md](messaging.md)), and the map lives in the
session's slot.

Backpressure works as in `net`: input that does not fit is held,
unacked, and processed as room appears. A session that has ended is
kept until every send it made has been acked, because zport frees a
send's memory on its ack; after five seconds it is freed anyway, in
case the other side died and never will.

The password check blocks netserve's loop for the half second it takes.
The kernel runs one check at a time system-wide regardless.

## When it stops: stall reports

A connection with anything waiting, in either direction, and nothing
moving for three seconds prints its whole state, once every three
seconds while it lasts:

```
net: relay 1 stalled: out 649 buffered + 0 msgs held; in 0 buffered, window 512;
     0 sends to the listener unacked; tcp segment unacked (512 bytes, 3 retries);
     segments in 1, dup 0, gap 0
netserve: session 1 stalled (state 4): to peer 512 buffered, 1 msgs held;
          to port 0 buffered, 0 msgs held; unacked: 8 to net, 0 to the port
```

- **relay, `tcp segment unacked` with retries climbing:** our segment
  is not reaching the client, or its ack is not getting back -- the
  network side. After seven retries TCP gives up (`tcp: giving up`).
- **relay, `in N buffered, window 0`:** the peer has been told to stop,
  and the data it sent has not been taken by netserve -- look at
  `sends to the listener unacked` and netserve's own line.
- **netserve, `8 to net` unacked:** net has stopped taking data.
- `zport: z_obj_blob() failed to allocate`: netserve's heap ran out.
- **No line at all** while something is clearly stuck means nothing
  is waiting inside Zeitlos: the bytes were never received, or went
  out and were not acked by anything we can see -- a packet capture
  on the other machine is the next step.

**Separating the network from the rest:** the echo service has no
posix and no telnet in it. `head -c 3000 /dev/zero | tr '\0' x | nc -q2
<ip> 7 | wc -c` should print 3000. If it does not, large segments are
the problem, not netserve.

## Found on hardware

The first board tests found bugs in netserve, in net's relay and
TCP, and one that was already there.

**`help` and `vi` hung in a telnet session to posix.** Both print a lot
at once, and posix sends up to 4 KB in one message (`OUT_BATCH`). A
message that did not fit was held until it did -- and one larger than
the whole 512-byte buffer never does. Every held message is now
consumed **in pieces** as room appears, with an offset, and acked only
when its last byte is used: in netserve both ways (a large paste has
the same shape) and in net's relay. The host tests now send a
4,000-byte message from the backend, a 600-byte paste, and a 2,600-byte
message through the relay; against the old code the first delivers 0
bytes.

**Bytes lost coming in (second board test).** `nc <ip> 7` with 3,000
bytes echoed 512. Reproduced on the host (2,488 of 3,000) by a peer
that sends as fast as the window allows, several segments at a time:
tcp.c acked a segment BEFORE handing it to its listener, so the ACK
advertised the window as it was before net's relay had filled its
buffer; the peer sent another full segment into room that was gone,
and the relay dropped bytes TCP had already acked -- lost for good.
Now tcp.c acks after delivery, the relay lowers its window the moment
it takes data, and a drop past the window is logged (`net: relay N: M
bytes past the window dropped`). Checked by `tests/test_tcp_pool.c`
(the window in that ACK) and `tests/test_e2e.c` (all 3,000 back).

The board's own 512, and later 1,048, turned out to be a different
thing: `nc` half-closes when its input ends, and this TCP has no
half-close -- a FIN from the peer ends the connection both ways, and
echo still in flight is discarded. A packet capture showed every byte
going in and the board's FIN following the client's at once. With
`nc`'s input held open (`(...; sleep 5) | nc ...`) all 3,000 come
back. Half-close is now supported; see "Half-close".

**`vi` did not come back to posix** (third board test): netserve only
accepted posix's call-back once the child had closed its port, and `vi`
exits without closing it -- the session stayed attached to a process
that no longer existed, typing into nothing. Fixed as above; the host
test now has a child that exits without a word.

**`vi` also needed the handoff** above: it waited for a terminal that
netserve, ignoring `Z_TERM_SET_PORT`, never sent it.

**posix ran the kernel log as commands** -- `wm: launch arg set:
'launch arg set: ...` in a loop. Not netserve's: a `term` connected to
`console0` had gone without a CLOSE (killed), `console` kept streaming
the log to its pid, posix -- started lazily -- got that pid, and took
the stream as typing on its own connection 1, because posix and repl
matched messages to connections **by tag alone**. Each log line ran as
a program, and running one logged a new line. Now posix, repl and
netserve match by sender and tag, and answer a stranger's DATA with
`z_port_reject_stranger()` (zport.h): an ack, so the sender can free
it, and a CLOSE, so it stops. `term` already checked the sender.

**Killing netserve left its connections hanging.** Nothing tells net
when a listening process exits or is killed: its relays went on
offering the dead process the client's keystrokes, the client waited
on a connection nobody would answer, and a stall report came every
three seconds for as long as it stayed open. net now checks each
listener about once a second (`z_proc_status()`); when one is gone,
every connection it had is reset -- the client hears "connection
reset" at once -- its unacked sends are freed (`z_port_forget()`), and
its ports stop listening, free for the next netserve. A listener that
asks for a port it already holds has restarted under its old pid
before that check saw it go, and its old connections are reset the
same way. `tests/test_relay.c` kills a listener mid-session, and
restarts one on its old pid.

The other way round is the listener's to notice. Replacing net leaves
the listener holding ports the new net has never heard of: a new net
starts with an empty listen table, and the kernel gives it the slot
the old one just left, so the pid does not change. Once the new net
is running, `z_proc_status()` cannot tell it from the old one. The
gap while it is being loaded is well under a second, so a listener
that only looked once a second would miss it and then never ask
again. While a listen is held, netserve (and zerdesk) check at
`Z_LISTEN_POLL_TICKS` (`sw/common/zlisten.h`). On the gap they forget
every connection -- the relays died with that net, and a CLOSE would
be delivered to whoever holds the pid now -- clear the listen flags
and ask for the ports again. They do not ask again while the same
net is still running: that resets the connections it already has.

The ports a dead netserve's sessions were connected to -- repl, posix
-- were not told either, and each held a session for it. They now
check their peers about once a second, as does netserve for its
backends: a session whose port's process dies tells its user so and
closes (through the encrypted channel, for SSH) -- see ports.md, "A
peer that died".

## Found by the tests: a missing CLOSED

A connection **this machine closed first** ended in TIME_WAIT without
ever telling its owner: no `TCP_EVENT_CLOSED`. Nothing had closed
gracefully before (`net`'s other sessions all abort), so it never
showed; the relay closes gracefully, and leaked one relay per session.
`tcp.c` now delivers CLOSED once both FINs are through, whoever sent
the first. `tests/test_relay.c` found it.

## Found by the tests: memory lost at every session's end

A session that ends waits for every send it made to be acked before its
slot is reused (zport frees a send's memory on its ack). But
`z_port_handle_ack()` ignores a connection already closed -- and the
session's ports ARE closed by then -- so the count never reached zero,
the session was freed by its 5-second deadline, and up to eight
buffers, about 4 KB of netserve's heap, were lost with it. Every
finished telnet or HTTP session did this; enough of them would have
run netserve out of memory. net's relay had the same pattern in its
sends to netserve. Both now count acks on a closed port
(`z_port_handle_ack_closed()`, zport.h), and a relay whose listener
has not acked everything waits for it (`R_DRAIN`) before its slot is
reused. Found by the HTTP tests, which open more sessions than any
before them; both halves are checked.

## Found by the tests: re-reading the configuration

`read_config()` rebuilds the listener lists, and the first version
cleared each listener's "net is listening for us" flag with the rest.
net answers a LISTEN once, so every connection to a port that had not
changed was then refused as `no service on that port`. A listener
whose port survives the re-read now keeps the flag. It showed as a
segfault in the test, several sections later -- see "Testing".

## Testing

On the host -- `make test` in `sw/apps/net` and in
`sw/apps/net/netserve` (needs `sysctl vm.mmap_min_addr=0` for the
tests with a scripted kernel; they skip otherwise):

| test | checks | what |
|---|---|---|
| `net/tests/test_relay.c` | 41 | the real `tcp.c` and `relay.c`: the window in the SYN-ACK, the offer, early data, both directions, holding, the window closing and reopening, a close with several segments still queued, the peer leaving mid-offer, REFUSED, a takeover, every relay busy, a relay waiting for its listener's acks, a listener killed mid-session, one restarted on its old pid |
| `net/netserve/tests/test_e2e.c` | 7 | end to end: a TCP client, the real tcp.c, relay.c and netserve.c, a scripted posix: `help`'s 1.4 KB and two 4 KB messages reach the client under real flow control; the board's echo test, 3,000 bytes in and all back, with and without the client's FIN on the last segment |
| `net/netserve/tests/test_authkeys.c` | 25 | the authkeys parser with OpenSSL-made keys and Python-computed expectations: every line form, every refusal, fingerprints as ssh-keygen prints them, Ed25519 agreeing with OpenSSL, RSA and Ed25519 signatures good and tampered |
| `net/ssh/tests/test_ssh_server.c` | 56 | the server engine against the REAL client engine: the handshake, the fingerprint, a wrong password then the right one, 40 KB out within the client's window, the server's own window, three failures, the kernel's refusal, a flipped bit, strict KEX (offered, Terrapin's IGNORE-first refused, non-kex mid-exchange refused), no algorithms in common, HTTP on the SSH port, an absurd length, a client sending past the window, channel data before logging in; public keys: the PK_OK question, Ed25519 and RSA logins, an unlisted key (not counted), SHA-1 ssh-rsa refused (by the engine itself), bad signatures counted, keys only, passwords only, server-sig-algs; `noauth` (SSHS_AUTH_ANY): `none` in with no password asked, any password (the hook never called), keyboard-interactive, an unknown method (its name made printable for the log), a key asked about (PK_OK) and a key in no list signed |
| `net/netserve/tests/test_netserve.c` | 247 | the real `netserve.c`: the policy, listening, echo, the subnet rule, option negotiation (and no loops), the password never echoed, a wrong one, three wrong, the kernel's delay, the login timeout, starting repl once for two sessions, CONNECT answers matched in order, Enter normalised, 0xFF both ways, backpressure, a 4 KB message and a 600-byte paste, a stranger's DATA, the terminal handoff and its call-back and fallback, a peer that vanishes; echo finishing a half-close; HTTP: files, indexes, types, %-decoding, every refusal (`..` in each disguise, `%00`, 404, 405, 408, 414/431, 400), HEAD, a 5 KB file streamed, a request in pieces, half-closes before and after the request, no file handle free, no handle or session left behind; SSH through netserve with the real client engine: host key made once and kept, off without a seeded TRNG, login, keystrokes to repl, 4 KB back, a 3 KB paste through the window, repl quitting, three wrong passwords, the login timeout, the engine wiped and released; SSH policy (keys only needs no password, the default needs one) and `/user/authkeys` (none, listed, unlisted, an edit taking effect, a refused line reported once); a backend dying silently under telnet (told, closed, its sends freed, nothing sent to it), under SSH (the message inside the channel), and during a handoff (waits for the call-back); a CONNECT answered after its session ended (the slot kept for the answer, a late CONNECTED closed, never given to the next session; a late REFUSED); listener lists (flags in any order, defaults, every refused entry, a port taken, four at most, the password rule per listener, noauth SSH without a password or with no TRNG, the old one-entry form); the subnet rule per listener both ways, and ports kept listened on across a re-read; a noauth telnet session (no banner or prompt, `bbs` started, its identity map, typeahead, its screen) beside a password one (`auth system`); a noauth SSH session from the internet with no password (never checked; identity with the user name; the BBS's screen through the channel); `z_port_ident()` and `z_port_refuse_unauthenticated()` on every kind of argument; `apps.netserve.ssh_sessions` (limits, the pool as the limit, not replaced while in use) |
| `net/tests/test_tcp_pool.c` | 73 | networking.md, "Connections" |

A connection netserve should have accepted and did not is reported as
a failure at the test's own line (`conn_since()`); the tests used to
index the message log with an unchecked `find()`, and turned such a
failure into a segfault further on.

Each behaviour was confirmed to fail its test when removed, with two
exceptions that are equivalent in today's code and say so where they
are: relay.c's flush-before-FIN test (the loop order already
guarantees it) and tcp.c's generation check after a DATA event.

### Testing on a board

1. **Flash and copy.** The kernel (phases A and B) and `net` are core
   apps in flash; rebuild the flash image, or put `net` on the card --
   a card copy shadows the flash copy. Put `netserve` in `/apps` on the
   card (`tools/tftp-dist.sh` includes it).
2. **A password**, 10 characters or more: `passwd` at the console.
3. **`/zeitlos.cfg`:**
   ```
   apps.netserve.ssh: 22 posix0
   apps.netserve.telnet: 23 repl0
   apps.netserve.http: 80 /www
   apps.netserve.echo: 7
   ```
   and your public key's line in `/user/authkeys`, an `index.html` in
   `/www`.
4. **Reboot.** The console should show, in some order:
   ```
   netserve: starting as netserve0
   netserve: telnet on port 23 to repl0, from this subnet only
   net: listening on port 23 for pid N
   netserve: listening on port 23
   ```
5. **From another machine on the same network:**

   | try | expect |
   |---|---|
   | `nc <ip> 7`, type a line | the line comes back |
   | `nc <ip> 99` | refused at once (a RST), not a timeout |
   | `telnet <ip>`, a wrong password three times | "Login incorrect", then closed; the console logs each |
   | `telnet <ip>`, the right one | `connecting to repl0...`, then a repl prompt; `ps` works |
   | `quit` in that session | the connection closes |
   | two telnet sessions at once | both work |
   | ssh and telnet at once | both work |
   | a telnet session while `web` fetches a page | both work -- the connection pool |
   | a telnet session, then Super+L locks the screen | the session carries on: the lock is the screen's, not the network's |
   | telnet to posix0: `help`, then `vi`, then `:q` | the help text; vi full screen; back at the posix prompt |
   | `curl -v http://<ip>/`, and a large file | the page, then the file; each request logged |
   | `curl http://<ip>/../zeitlos.cfg` | `400`, nothing leaves `/www` |
   | `head -c 3000 /dev/zero \| tr '\0' x \| nc -q2 <ip> 7 \| wc -c` | 3000 -- the half-close |
   | `ssh <ip>`, first time | the fingerprint on the console at boot is the one ssh shows |
   | `ssh <ip>` with your key in `/user/authkeys` | in, no password; the console logs the key's fingerprint |
   | the same with an RSA key, and with a `SHA256:` line | in |
   | `ssh <ip>`, a wrong password three times | disconnected |
   | `apps.netserve.ssh_auth: key`, then a password login | refused: keys only |
   | `apps.netserve.telnet: 23 repl0 noauth` | `telnet <ip>`: `the port refused: this port needs a logged-in session`; the console names repl |
   | `apps.netserve.ssh: 2222 posix0 noauth` | the same from posix |
   | `apps.netserve.ssh: 22 posix0; 2222 posix0 noauth`, `ssh -p 22` | still asks for the password |
   | `apps.netserve.telnet: 23 portdemo0 noauth`, `run portdemo` first | straight to portdemo's banner: no prompt |
   | the same over SSH (`ssh -p 2222 anyone@<ip>`) with OpenSSH, PuTTY, Dropbear's `dbclient`, SyncTERM, NetRunner | in with no password prompt -- or, for a client that insists on one, with any password |

   Worth reporting: how the typing feels (each keystroke is a round
   trip through net and netserve), and any `net: relay` or `netserve:`
   line that looks wrong.

## Not yet

- **Throughput:** TCP sends one segment of at most 536 bytes at a time
  (networking.md, "Known limits"); two at a time, and full-size ones,
  would speed up large HTTP files most.
- SSH `exec` (`ssh host command`), and scp/sftp.
- **Telnet BINARY** (RFC 856). Enter is normalised to one CR and
  nothing else is negotiated, which is right for typing and wrong for
  a file transfer (XMODEM) over telnet -- not needed while the BBS
  carries no files ([bbs.md](bbs.md), "Files"); over SSH the channel is
  already 8-bit clean.
- **Window size.** A telnet client's NAWS is refused; the ports are
  80x25 (`term`'s size), and a larger client window simply shows it
  in the top-left corner.
- **More than one socket from `web` at a time.** `net` relays two
  outbound sockets now (`NET_SOCK_SLOTS`, networking.md), but `web`
  uses one, so a second fetch waits for the first.
