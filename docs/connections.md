# Connections

Where a `term` window can be pointed, and how it gets there.

Four kinds of target, one shape: a provider to look up in the pid
registry, and a scalar argument that travels with the CONNECT.

| kind | provider | argument |
|---|---|---|
| `port <name>` | the name itself | none |
| `serial [baud]` | `serial0` | baud rate, or 0 for "as-is" |
| `telnet <host>` | `net0` | resolved IPv4 address |
| `ssh [user@]host` | `net0` | a token issued by `net` |

`sw/common/zconnect.h` builds them. `sw/apps/repl` and `sw/apps/term`
both use it.

## Two ways in

**From repl**, typed at a prompt:

```
> telnet 192.168.1.10
> serial 9600
> port portdemo0
> ssh me@10.0.0.5
```

repl works out the target and then tells the term window that asked to
go there (`Z_TERM_SET_PORT`, `sw/common/zterm.h`). F12 comes back.

**From term**, with **F11**:

```
open> telnet 192.168.1.10
```

A prompt bar across the bottom row. Enter connects, Escape cancels,
Backspace edits. Same four words, same code underneath.

## Why term can do it itself now

Every target used to come from repl telling term where to go. That is
fine when you are already at a repl prompt and useless when the thing
you want to reach *is* the terminal you are sitting in — or when repl
is not what this window is connected to, which is exactly the situation
where you most want a way out that is not "go back to repl first".

The alternative was term reimplementing hostname resolution and the ssh
token handshake. Two of each, drifting. The ssh one in particular is a
bounded request/reply with three distinct failure modes that each want
reporting differently, and having two versions of that is how one of
them silently stops matching `net`.

So the working-out moved to `sw/common/zconnect.c` and both callers use
it. repl hands the result to a term window; term connects to it
directly, one hop fewer.

## Why a prompt bar and not a dialog

Because this is a terminal.

A widget panel would need its own window — wm has no modal dialogs — so
a second window appears in front of the one you were typing in, and
there is a focus question to answer when it closes. A prompt bar is
what a terminal already is, it needs no new window, and it is
unambiguously keyboard-driven. That last part matters: F11 is most
useful exactly when whatever you were connected to has stopped
answering.

It draws in **reverse video, cell by cell, through term's own
`draw_cell()`** rather than with `z_win_draw_text()`. That puts it on
the character grid exactly, overwrites the row to its last column so
nothing shows through past the text, and makes it read as chrome rather
than as something the far end printed.

It does **not** write into the vt100 screen. Doing so would destroy a
row of the session with nothing to restore it from — the vt holds one
screen, not a stack. On dismissal the row is marked dirty and the
ordinary redraw path puts the session's content back. Nothing in the
session ever knows it happened.

## F11 and F12

| | |
|---|---|
| **F11** | open the bar — go somewhere new |
| **F12** | escape to `repl0` — go back where you started |

Adjacent on the keyboard and adjacent in meaning. Neither is a VT100
key anything sends on purpose, which is why term can intercept them
before the port sees them.

**The bar owns every key while it is up, including F12.** Otherwise
Escape and F12 would both be "get me out of this", which is two answers
to one question.

## This blocks, and it matters differently per caller

`z_conn_prepare()` can wait seconds — a DNS lookup with no answer, or
an ssh prepare to a `net` that does not recognise the subject. Both
waits are bounded; nothing spins forever. But while one runs the
calling process is not reading messages.

**In repl**, one mailbox serves every connected term window, so a slow
lookup from one window stalls every other window's output. That was
already true before this moved.

**In term**, one window — but term is also the process wm expects a
`Z_WM_REDRAW_DONE` from. A long enough stall and wm reports "timed out
waiting for pid N to ack a redraw", which is exactly the bug term's
`connect_port()` grew a custom message pump to avoid.

So the Open bar prints `open: telnet myhost ...` **before** calling,
because the window will not repaint during it. A terminal that freezes
with no explanation is worse than one that says what it is waiting for.

Fixing this properly means an asynchronous resolve-then-connect flow.
That is a real improvement and a bigger change than this one.

## Timeouts

| | |
|---|---|
| `Z_CONN_TIMEOUT_LOCAL_TICKS` | 2s — port, serial |
| `Z_CONN_TIMEOUT_NETWORK_TICKS` | 45s — telnet, ssh |

The network one is long for a reason found on hardware: `net`'s telnet
provider does not answer until a TCP handshake resolves, and `tcp.c`'s
retry budget alone is ~31.5s. At 2s, every connect to an unreachable
host failed on term's side before net had finished trying — so the
error you got was never the real one.

## Why ssh needs a token

`net` is handed the username **directly**, in one hop, and returns a
scalar token that gets forwarded instead.

A string forwarded through `Z_TERM_SET_PORT` would have its pointer
translated twice and land on garbage (`sw/common/znet.h`). The token is
a scalar, which survives. `net` tells a telnet connect from an ssh one
by checking the argument against the token it just issued — same shape,
different meaning.

The wait for that reply is **bounded**, following `zdns.c`'s loop rather
than `z_msg_wait()`, which spins forever. There is an ordinary way for
the reply never to come: a `net` that predates `Z_NET_SSH_PREPARE`, or
one built `SSH_ENABLE=0`, drops the subject silently. Core apps live in
the flash ZAR archive, so rebuilding one against a stale other is easy
to do by accident — and an unbounded wait in repl froze every connected
window at once, with no output anywhere. That is how it was found.

## No fallback pids

Nothing a user types gets a fixed-pid fallback. If the provider is not
running, the CONNECT fails and that terminal stays in local echo — the
same clean failure any unreachable target gives.

term's own startup connection to `repl0` does have one, because there
has to be something to talk to before anything has been typed.

## See also

- `sw/common/zconnect.h` — the interface and the per-caller blocking cost
- `docs/ports.md` — the port mechanism underneath all of this
- `docs/uart1.md` — the `serial` kind
- `docs/networking.md` — `net`, telnet and ssh
