# TLS

`sw/apps/web` speaks TLS 1.3. This describes what it implements, what
it deliberately does not, and why the insecure phase is gated the way
it is.

See [web_app.md](web_app.md) for the browser and
[http.md](http.md) for the layer above.

---

## Certificates are verified

The chain is checked against a root store on the card, the hostname
must match, the dates must hold, and the server must prove it holds
the leaf's private key. Any of those failing refuses the connection
and says which one.

**`WEB_TLS_INSECURE` is gone**, along with every code path it
guarded. `tls_set_verify()` must be called before `tls_start()`, and
`tls_start()` refuses without it — there is deliberately no way to
run this code with checking switched off. `make WEB_TLS=0` builds
without TLS *at all*, which is the option for a board where the
crypto does not fit, not a way to disable verification.

That is `docs/ssh.md`'s standard: refuse, do not warn. A warning
shown on every connection is one that gets clicked through, and
nobody can tell an intercepted page from a working one by looking at
it.

### The other kind of authentication: a pin

`tls_set_pin()` is the second, and only other, way to authenticate a
server, for Gemini, whose servers almost all present self-signed
certificates. The server must still prove it holds its key
(CertificateVerify, below); instead of a CA vouching for that key, a
callback compares it with the key the same host presented before.
Exactly one of `tls_set_verify()` and `tls_set_pin()` must be set, and
`tls_start()` refuses with neither and with both, so "refuse, do not
warn" still holds: there is no third, unauthenticated way in. See
[gopher_gemini.md](gopher_gemini.md#certificates-trust-on-first-use).

### The chain is verified AFTER our Finished

Textbook order is: check the certificate chain, then send Finished.
That order does not survive this machine.

A chain check costs **tens of seconds** here — three P-384 signatures
at roughly 16 seconds each, measured on hardware — and the server
spends all of it waiting for our Finished. Real servers give up:
en.wikipedia.org closed the connection before the check completed, so
the handshake finished against a socket that no longer existed.

So `tls.c` sends Finished first and verifies immediately after. The
cost is worth naming precisely: **the handshake completes with a peer
whose certificate has not been checked yet.** What that peer receives
in the window is a Finished message, which proves only that we derived
the same keys it did — it learns nothing about us.

Nothing crosses that window:

- `tls_write()` refuses unless `chain_ok`, not merely `ESTABLISHED`.
- An incoming application record with `chain_ok` false is an error.
- A bad chain tears the connection down before any request is sent.

Both checks are explicit rather than relying on the two flags being
set together, so that deferring verification cannot quietly become
skipping it. `tests/test_tls.c`'s `none` and `wronghost` runs both
still refuse, and they now exercise this path.

This becomes unnecessary the moment verification is fast enough — see
[x509.md](x509.md) on the remaining optimisations.

### CertificateVerify is not optional

A chain alone authenticates nothing. Certificates are public — anyone
can fetch the real one for any site and replay it. `CertificateVerify`
is the proof that the peer holds the corresponding private key, and a
client that checks the chain but not that signature has done no
authentication whatsoever.

The signed content is 64 bytes of `0x20`, the string
`TLS 1.3, server CertificateVerify`, a zero byte, and the transcript
hash **through** `Certificate`. The preamble exists to make the
signature useless in any other protocol, so getting it byte-exact is
the entire point of it being there. `tls.h`'s `th_pre_msg` — added in
for the Finished snapshot — already provides the transcript
at exactly the right instant.

PKCS#1 v1.5 is **rejected** here even though certificates use it
constantly: RFC 8446 §4.4.3 forbids it for handshake signatures, so
accepting it would mean accepting something no conforming server
sends.

### Two things must be on the card

| | |
|---|---|
| `/web/roots.der` | concatenated DER certificates |
| a set clock | `z_rtc_valid()`, which `net` gets from NTP |

Both are refusals when missing. Without a clock, an expired
certificate cannot be told from a current one — and an expired
certificate is the normal state of one whose key has since been
compromised.

Build the store with:

```
for f in /etc/ssl/certs/*.pem
do openssl x509 -in "$f" -outform DER
done > roots.der
```

The store is scanned **linearly** per handshake. A full Mozilla set is
around 150 certificates and 250KB off an SPI SD card, which is
not free; an index keyed on the issuer DN is the obvious fix and is
deferred until there is a measurement to justify a format.

## What is implemented

One cipher suite and one group:

| | |
|---|---|
| `TLS_CHACHA20_POLY1305_SHA256` | `0x1303` |
| `x25519` | `0x001d` |

Not a reluctant compromise: it is the only suite whose primitives this
tree already has, and every one of them is already in use elsewhere.

| Primitive | From |
|---|---|
| X25519 | `sw/ext/monocypher`, `crypto_x25519()` |
| ChaCha20-Poly1305 | `sw/ext/monocypher`, `crypto_aead_*_ietf()` |
| SHA-256 | `sw/apps/net/ssh/ssh_sha256.c` |
| HMAC, HKDF, key schedule | `tls_crypto.c` |

`crypto_aead_init_ietf()` is exactly RFC 8439's AEAD with a 12-byte
nonce, which is what TLS 1.3 needs — no hand-rolled construction.

The AES-GCM suites would need an AES implementation and a GHASH,
neither of which exists here and both of which are constant-time
minefields on a CPU with no cache and no AES instructions. Every TLS
1.3 server must implement `TLS_AES_128_GCM_SHA256`, and in practice
essentially all of them also implement ChaCha20 — it is what every
mobile client without AES hardware negotiates.

Also handled: middlebox compatibility mode (the 32-byte session id and
the dummy ChangeCipherSpec, both non-optional on the open internet),
ALPN naming `http/1.1`, SNI, `KeyUpdate`, `close_notify`, and an empty
`Certificate` in reply to a `CertificateRequest`.

### Answering a CertificateRequest, in order

A server may ask for a client certificate; this client has none, so it
answers with an empty `Certificate`. **Where** it answers matters, and
it used to be wrong. RFC 8446 puts the client's `Certificate` in the
client's flight -- after the server's `Finished`, encrypted under the
client handshake key, immediately before the client's `Finished`, and
in the transcript after everything the server sent. The first version
sent it the moment the request arrived. That put it in the transcript
ahead of the server's `Certificate` and `CertificateVerify`, so the
signature the server had made over its own transcript never matched
this side's hash: *"the server did not prove it holds its own key"*,
from every server that asked. It also went out unencrypted.

HTTPS servers almost never ask, so the web never showed it. Gemini
servers ask routinely -- a client certificate is how a capsule knows
who is visiting -- so **every Gemini capsule failed**, which is how it
was found ([gopher_gemini.md](gopher_gemini.md)). Now the request is
only noted when it arrives, and the empty `Certificate` goes out in
`handle_server_finished()`, after the switch to the client handshake
key; the client `Finished` covers it, and the application secrets --
taken from the transcript through the server's `Finished`, RFC 8446
section 7.1 -- do not.

A request during the handshake must have an empty context, and one
after it is post-handshake authentication, which this client never
offers (no `post_handshake_auth` extension); both are refused.

### Signature schemes

Offered in `signature_algorithms`, and checked in `CertificateVerify`:

| scheme | key | |
|---|---|---|
| `ecdsa_secp256r1_sha256` | P-256 | |
| `rsa_pss_rsae_sha256` | RSA, up to 4096 bits | |
| `ecdsa_secp384r1_sha384` | P-384 | the content hashed with SHA-384 |
| `ed25519` | Ed25519 | the content itself is signed, no pre-hash (Monocypher) |
| `rsa_pkcs1_sha256` | -- | offered for **certificate** signatures only; refused in `CertificateVerify`, as TLS 1.3 requires |

A TLS 1.3 server can sign only with a scheme that matches its key, and
ECDSA schemes are tied to one curve, so a key type missing from this
list is a handshake that ends in alert 40 before anything is verified.
P-384 and Ed25519 were added for that reason: Ed25519 is how many Gemini
capsules generate their own certificates, and P-384 turns up on both
Gemini and the web. Certificate chains signed with Ed25519 are not
verified -- no public CA issues them -- so an Ed25519 server is reachable
by pin (Gemini), not by CA.

### Not implemented

- **No session resumption.** Every connection is a full handshake,
  including a redirect to a host seen moments ago. See
  [tls_resumption.md](tls_resumption.md).
- **No session resumption or 0-RTT.** Both need a ticket store; 0-RTT
  additionally needs replay protection a client cannot provide alone.
- **No client certificates of its own.** A request for one is answered
  with an empty `Certificate` (above); a capsule that insists on one
  refuses the connection.
- **No HelloRetryRequest.** Detected and reported clearly rather than
  misparsed. It only happens when a server rejects every group
  offered, and x25519 is universal.
- **No renegotiation.** It does not exist in TLS 1.3.

---

## Where SHA-256 comes from

`sw/apps/net/ssh/ssh_sha256.c`, compiled into `web` directly by its
Makefile. That is a cross-app source reference and it is not where
this should end up.

The right home is `sw/common/zsha256.c`, with ssh's copy becoming a
forwarding header. It was not done that way for a mechanical reason:
these changes arrive as an archive unpacked over the tree, and **an
archive cannot delete a file**. Leaving `ssh_sha256.c` in place but
unreferenced is worse than one odd include path — it is a file someone
will eventually edit, wondering why their fix does nothing.

So there is one implementation, one odd reference, and this note.
Promoting it is a five-minute change for anyone who can delete a file.

---

## Testing

Two suites, deliberately separate.

### `make test` — offline, always runs

`tests/test_tls_crypto.c`, 38 checks against `tests/vectors.h`.

The vectors are **generated**, not transcribed:
`tests/gen_vectors.py` computes them with Python's `hashlib`/`hmac`
and the `cryptography` package — implementations with nothing in
common with the C under test. Hand-copying them out of the RFCs would
have tested the C against my typing.

The generator in turn is checked against **published constants**, so a
bug in it cannot quietly become the expected answer. It refuses to
emit anything if those disagree:

| | |
|---|---|
| RFC 4231 | HMAC-SHA256 cases 2 and 6 |
| RFC 5869 | HKDF-SHA256 cases 1 and 3 |
| RFC 8439 §2.8.2 | AEAD_CHACHA20_POLY1305 |
| RFC 8446 | the no-PSK early secret and its `derived` successor |

This suite exists *before* any handshake code for a specific reason. A
key schedule bug does not announce itself: the handshake completes,
the Finished check fails, and the server replies `decrypt_error` — an
intentionally uninformative alert, because telling a client which part
of its crypto is wrong is an oracle. So HKDF, the label construction,
the transcript, the nonce and the record layer all fail *identically*.
Removing four of them from the list of suspects up front is most of
the work.

### `make test-tls` — a live handshake, verified

`tests/test_tls.c` runs the client against a real OpenSSL TLS 1.3
server on localhost (`tests/tls_server.py`). The server generates a
self-signed CA certificate for `localhost` and writes out its DER;
the client is given that one certificate as its **entire root store**.

So a successful run now exercises the chain walk, the hostname match,
the validity dates and the RSA-PSS `CertificateVerify` — not merely
the record layer. It runs at chunk sizes of 0 (whole), 1, 7 and 536.
**536 is `net`'s MSS**, and a record layer that only works when whole
records arrive at once is one that works on a build machine and fails
on hardware.

Two further runs must be **refused**:

| mode | store | expected |
|---|---|---|
| `none` | empty | *the certificate chain does not lead to a known root* |
| `wronghost` | correct | *the certificate is not valid for this host* |
| `pinwrong` | none, pin mode | refused by the pin |

and one more must **succeed**: `pin`, with no root store at all and the
correct pin, which proves the pin replaces the chain check rather than
running beside it. Every run also checks that `tls_start()` refuses
with no authentication configured and with both kinds at once.

Those are the runs that prove verification is actually consulted
rather than being code that happens to be linked in and never
reached. A client that skipped it entirely would pass every positive
case above.

A recorded trace cannot substitute for any of this: the client's key
share is fresh every run, so every secret differs and nothing past
the ServerHello would decrypt.

Kept out of `make test` because it needs python3, the `cryptography`
module and the `openssl` binary. A suite that cannot run without a
certificate toolchain is one people stop running.

---

## Bugs this found

Worth recording, because both were invisible to a passing handshake.

**No reply to `close_notify`.** The first live run completed the
handshake, sent the request, and the server received and decrypted it
— then the test hung. OpenSSL's `unwrap()` blocks until the client
answers `close_notify`, and this client never did. On hardware that
would have shown up as every page load leaving a socket open until
`tcp.c` timed out, and since there is one TCB in the system, **the
next fetch would be refused as busy**. A test that only checked "did
the handshake succeed" would have passed.

**The Finished transcript snapshot.** `verify_data` covers the
transcript up to but *not including* the Finished message, and by the
time the message has been reassembled it is already in the running
hash. Snapshotting after the fact is impossible, so `tls.h` carries a
second context, `th_pre_msg`, copied before each message's header goes
in. Getting this wrong produces a Finished that never verifies, and
the server's only response is `decrypt_error`.

---

## Still true

`tcp.c` remains unmeasured over the open internet — stop-and-wait, a
window matched to the NIC's buffer, no fast retransmit. TLS adds a
full round trip
before any request goes out, so whatever that number is, HTTPS pays it
twice. See
[networking.md](networking.md#throughput).

See also [crypto_perf.md](crypto_perf.md) for where the handshake time
goes, and [crypto_hw_options.md](crypto_hw_options.md) for the hardware
that was considered to shorten it.
