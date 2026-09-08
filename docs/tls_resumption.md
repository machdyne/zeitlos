# Avoiding handshakes: keep-alive, resumption, and what the
# accelerator is actually worth

Three related questions, with the numbers from Lakritz.

## Is the Montgomery block worth its 983 LUTs?

It depends on the chain, and the honest answer is "for most sites,
yes, but not all".

Measured:

| site | chain | accelerated? | verification |
|---|---|---|---|
| en.wikipedia.org | P-384 x3 | yes | 10.0s (was 46.7s) |
| google.com | P-256 leaf, P-384 intermediate | yes | 4.8s (was ~15s) |
| www.google.com | RSA-2048 + RSA-4096 | **no** | 8.6s |

ECDSA leaf certificates are now the common case, so the block earns
its area on most sites. What it cannot touch is an RSA link, and RSA
**roots** remain widespread even under ECDSA leaves.

Early anchoring (docs/x509.md) helps more than it looks: the walk
stops at the first certificate whose issuer is in the local store, so
a chain whose ECDSA intermediate is directly trusted never reaches
the RSA root at all. What the store contains therefore changes which
sites are fast.

### With one BRAM spare, could it do RSA?

Arithmetically it fits. RSA-4096 needs A, B, N and T at 128 words
each: 514 words, 2,056 bytes. One DP16KD is 2,304 bytes.

Two things stand in the way, and the second is the real one.

**Ports.** A DP16KD has two. The current design does two reads and one
write per cycle across separate arrays; sharing one BRAM serialises
that to two or three cycles per step. At 2n^2 steps and n=128 that is
~80k cycles, about 1.7ms per multiply — still 17 multiplies in 30ms,
which would be fine.

**R^2.** An RSA verification is 17 Montgomery multiplies **plus**
computing R^2 mod m, and that is inherently ~32n modular doublings —
4,096 of them at 128 limbs. The block multiplies; it does not double.
Accelerate every multiply to zero and the doublings still take about
a second in software.

There is no way around this with a multiplier alone: every
division-free route to R^2 is a fixed point of Montgomery
multiplication, because R^2 is what gets you INTO the Montgomery
domain. So a wide block would also need a **doubling command** — shift
T left, conditionally subtract N, repeat k times. That is a small
addition to the existing datapath (the conditional subtract is
already there, in S_SUB/S_CPY) and would put the 4,096 doublings at
roughly 22ms.

So: one BRAM, a wider datapath, and a doubling command would take an
RSA-4096 verification from 6.9s to something like 50ms. It is a real
option, it is a bigger change than the first block was, and it should
be measured against the alternatives below — which cost no gates at
all.

## Why we are not resuming sessions

Two different mechanisms are missing, and the cheaper one is missing
for a reason that no longer holds.

### HTTP keep-alive: not done, and the reason has expired

`http.c` sends `Connection: close`, and its own comment says why:
"because `net` has one TCB". Every fetch therefore opens a new TCP
connection and does a full TLS handshake.

For a same-host redirect that is pure waste. `/` to `/wiki/Main_Page`
on en.wikipedia.org is two handshakes at ~15s each, and the second
one re-verifies a chain the first already verified — the chain cache
saves the signatures but not the handshake.

**One TCB is not the obstacle it looks like.** Keeping a connection
open uses the same single TCB; it just does not close it between
requests. What it needs:

- `http.c` to offer `Connection: keep-alive` and honour the server's
  answer, which means trusting Content-Length or chunked framing to
  find the end of a response rather than relying on close. Both are
  already parsed.
- `web.c` to keep `sock` open when the next fetch is the same host and
  port, and to reset only the HTTP parser rather than the connection.
- A guard for the server closing anyway, which is always allowed.

This is the cheapest large win available: on a same-host redirect it
removes the handshake **entirely**, not merely making it cheaper.

### TLS 1.3 resumption: a bigger job

Resumption helps where keep-alive cannot — a new connection to a host
seen earlier, including after the first connection has closed.

`tls.c` already recognises `NewSessionTicket` and deliberately
discards it. Making use of it needs:

- **Storage**: ticket, the PSK derived from the resumption master
  secret, `ticket_age_add`, lifetime, and the cipher suite, keyed by
  host. In RAM, or on `/ram` to survive an app restart.
- **Key schedule**: the resumption secret and the binder key, with
  their own labels. `tls_crypto.c` currently starts the schedule from
  32 zero bytes because there is never a PSK; that becomes the PSK.
- **The binder**: an HMAC over the ClientHello **truncated before the
  binder list itself**. The extension has to be built, its own length
  accounted for, and the transcript hashed up to a point inside the
  message being written. This is the fiddly part and the usual source
  of bugs.
- **Extension ordering**: `pre_shared_key` MUST be the last extension
  in the ClientHello, which the current builder does not guarantee.
- **Both outcomes**: the server may accept the PSK and skip
  certificates, or decline and continue with a full handshake. Both
  paths have to work, and the second is the one that gets tested
  least.

Perhaps 400-600 lines across `tls.c` and `tls_crypto.c`, most of it
in the key schedule and the binder. It was not done earlier because
nothing needed it to render a page, and because a resumption bug
fails as "handshake works sometimes", which is the worst shape of
bug to chase on hardware.

## Recommendation

1. **HTTP keep-alive first.** Small, self-contained, removes a whole
   handshake on same-host redirects, and needs no new crypto.
2. **Then TLS resumption**, which covers the rest and helps RSA and
   ECDSA equally.
3. **The wide Montgomery block last**, if at all. It is the only
   option that fixes a *first* connection to an RSA site — but if
   keep-alive and resumption land, first connections are a much
   smaller share of what a browsing session actually does.

The ordering is deliberate: the first two cost no gates, and they
reduce how often the third matters.
