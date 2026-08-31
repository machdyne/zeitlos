# SSH

An SSH-2 client. `ssh [user@]host` from a `term` prompt opens an
interactive shell on a remote machine, through the same zport
mechanism `telnet` uses.

Depends on the TRNG (`docs/trng.md`) and refuses to run without it.

## Algorithms

One per slot, because that is what `sw/ext/monocypher` provides:

| Slot | Algorithm |
|---|---|
| kex | `curve25519-sha256` |
| host key | `ssh-ed25519` |
| cipher | `chacha20-poly1305@openssh.com` |
| mac | implicit in the AEAD |
| compression | `none` |

There is therefore no algorithm negotiation worth the name — we send
one name per slot and check the server's list contains it.

**Monocypher has no AES**, so every `aes*-ctr` and `aes*-gcm` suite is
unavailable and ChaCha20 is the only option. OpenSSH enables it by
default so this works nearly everywhere, but a hardened server that has
disabled it gets a specific error rather than a generic failure:

    ssh: server does not offer chacha20-poly1305@openssh.com

Monocypher also has no SHA-256, which `curve25519-sha256` and RFC 4253
key derivation both need; `ssh/ssh_sha256.c` supplies it.

## Files

| File | Role |
|---|---|
| `ssh_sha256.c` | SHA-256, the Monocypher gap |
| `ssh_wire.c` | RFC 4251 codecs, bounds-checked |
| `ssh_crypto.c` | the AEAD, key derivation, fingerprints |
| `ssh_proto.c` | the session engine — no platform dependencies |
| `ssh.c` | binds the engine to `tcp.c`; prompts; entropy gate |

`ssh_proto.c` has **no `tcp.h`, no `zeitlos.h`, and no Zeitlos type**.
Randomness arrives through a callback. That is what lets the whole
handshake run on a host against a test server, and it is not
negotiable — if something in there ever needs the time or a random
number, it takes a callback rather than an include. Four real bugs were
found that way, in minutes, that would each have cost a flash cycle and
produced one useless line of output on hardware.

## Entropy is a hard gate

`ssh_connect()` returns false when `z_rng_secure()` is false, and
`net` refuses the request even earlier, at `Z_NET_SSH_PREPARE`, so the
failure arrives while `term` is still connected to `repl` and can
display it.

This is not caution for its own sake. Every ephemeral key comes from
the system CSPRNG; seeded from cycle-counter jitter instead of the
TRNG, a passive observer can reconstruct the session key, and **neither
end shows any symptom**. A weak SSH session is not a degraded session,
it is an open one, so this refuses rather than warning.

## How a connection is set up

The two-step handshake in `sw/common/znet.h` exists to solve one
problem: SSH needs a **username**, and `Z_TERM_SET_PORT` cannot carry
one.

The obvious design — put `{user, ip}` in `SET_PORT`'s `arg` map and let
`term` forward it — is broken, subtly. `z_resolve_obj()` in
`sw/os/msg.c` rewrites payload pointers to *physical* addresses when a
message is read. When `term` re-sends that object in its own
`Z_PORT_CONNECT`, the kernel translates it a **second** time —
`ptr - 0x80000000 + base` on an already-physical address, which
underflows into garbage. Telnet escapes this only because its arg is a
bare `Z_UINT32` with no pointers in it.

So:

1. `repl` sends `Z_NET_SSH_PREPARE {user, ip, port}` **directly to
   net**, one hop, where the strings resolve correctly.
2. `net` stores it and replies with a random `token`.
3. `repl` sends `Z_TERM_SET_PORT {name:"net0", arg:token}` — scalar,
   exactly like telnet's IP.
4. `term` connects; `net` recognises the token and starts SSH.

The token is single-use and expires after ~30 s, so a stale or
misdirected CONNECT cannot pick up someone else's credentials. It is
random rather than a counter, because it is the only thing standing
between a `Z_PORT_CONNECT` and the username it claims.

`z_obj_clone()` in `zobj.c` would fix the underlying limitation
properly and is worth doing on its own merits — it just should not be
on SSH's critical path.

## Prompts happen in band

There is no separate channel for asking the user something. Prompts are
written to the same port the session will use, and replies arrive as
ordinary port data that `ssh.c` intercepts before it becomes channel
traffic.

This works because `term` does no local echo once connected to a port,
so a password is invisible for free rather than by arrangement. It also
means `net` accepts the zport **immediately** rather than deferring
until the TCP handshake resolves the way telnet does — the handshake is
several seconds with a fingerprint confirmation and a password prompt
in the middle, and all of that renders through this port.

Host keys are trust-on-first-use, asked explicitly, with no
`known_hosts` yet. The fingerprint is shown in OpenSSH's format so it
can be compared against `ssh-keygen -lf` output from another machine.
Only the exact strings `yes` and `no` are accepted — a one-keystroke
answer to "is this the right machine" is how people say yes to a
question they did not read.

## Cost

Measured for `rv32im -Os` with `--gc-sections`, against the same `net`
built with `SSH_ENABLE=0`:

| | text | bss |
|---|---|---|
| `SSH_ENABLE=0` | 45,932 | 14,101 |
| `SSH_ENABLE=1` | 86,432 | 26,765 |
| **cost** | **+40.5 KB** | **+12.7 KB** |

The bss is all static buffers (a 4 KB receive assembly buffer, a 4 KB
transmit buffer, a 2 KB outbound queue, a 1 KB stall buffer). Those
count against the app image, **not** against `net`'s 32 KB stack+heap
allowance in `kernel.h` — which is why `net` stays at
`Z_PROC_STACK_SIZE_MEDIUM`. Monocypher's deepest stack frame is 1,088
bytes (`crypto_eddsa_check_equation`).

Expect roughly **1.1–2.7 seconds of CPU** for a handshake (2× X25519 +
1× Ed25519 verify = 7.26M instructions at CPI 7.5–18), and 54–130 KB/s
of bulk throughput. `net` is blocked for the duration of each X25519,
so DNS, NTP and TFTP for other apps stall for about half a second at a
time during a handshake.

## Testing

Three host test suites, all clean under
`-fsanitize=address,undefined`:

    cd sw/test
    gcc -O2 -I../apps/net/ssh -I../ext/monocypher -o t_sha \
        test_ssh_sha256.c ../apps/net/ssh/ssh_sha256.c && ./t_sha

    gcc -O2 -I../apps/net/ssh -I../ext/monocypher -o t_ssh \
        test_ssh_crypto.c ../apps/net/ssh/ssh_wire.c \
        ../apps/net/ssh/ssh_crypto.c ../apps/net/ssh/ssh_sha256.c \
        ../ext/monocypher/monocypher.c && ./t_ssh

    gcc -O2 -I../apps/net/ssh -I../ext/monocypher -o t_session \
        test_ssh_session.c ../apps/net/ssh/ssh_proto.c \
        ../apps/net/ssh/ssh_wire.c ../apps/net/ssh/ssh_crypto.c \
        ../apps/net/ssh/ssh_sha256.c ../ext/monocypher/monocypher.c \
        ../ext/monocypher/monocypher-ed25519.c && ./t_session

The test server interleaves a window adjust and stderr output before
its reply to the shell request — placed there deliberately, because an
earlier version put the window adjust right after
`CHANNEL_OPEN_CONFIRMATION`, where the buggy client sent its shell
request early and then mistook the *pty* reply for the *shell* reply.
Two errors cancelling out, and a test that passed for the wrong reason.
Every check here has been confirmed to fail against the code it was
written to catch.

`test_ssh_session.c` runs the real engine against a minimal but genuine
SSH-2 server in the same process — real curve25519-sha256, a real
Ed25519 host key signature, real ChaCha20-Poly1305. Data is fed in
13-byte chunks so reassembly across segment boundaries is exercised
rather than assumed. It covers the full handshake, a cipher mismatch, a
tampered packet, an oversize banner, and two server-initiated rekeys.

### Bugs it found

Worth recording, because none would have produced a useful error on
hardware:

1. **Tag length on plaintext packets.** `send_packet()` always added 16
   bytes of tag, so every pre-NEWKEYS packet carried uninitialised
   buffer that the server read as the next packet's length.
2. **`k_mpint[36]` should be 40.** Worst case is 4 length + 1 pad + 32
   value = 37. It fits whenever the secret's top bit is clear, so it
   works about half the time. The test server had the identical
   off-by-one, which is how one session passed and the next failed.
3. **TX and RX keys installed together.** RFC 4253 §7.3: NEWKEYS
   applies to what the *sender* transmits after it. Installing the
   receive key when we send means decrypting the server's still-
   plaintext NEWKEYS with a new key.
4. **The padding rule is phase-dependent.** RFC 4253 §6 counts the
   4-byte length field toward block alignment; `chacha20-poly1305@
   openssh.com` treats that field as AAD and excludes it. Applying the
   AEAD rule during the plaintext handshake put every packet 4 bytes
   out of alignment, and OpenSSH closed the connection right after
   `KEX_ECDH_INIT` with no error. **The host tests missed this because
   the test server used the same wrong rule** — the two agreed with
   each other. The server now derives padding the same phase-dependent
   way *and* rejects misaligned inbound packets, which is what a real
   server does silently.
5. **"Any message" treated as a request reply.** `ST_PTY` accepted
   anything as the pty-req answer and `ST_SHELL` failed on anything
   that was not `CHANNEL_SUCCESS`. Servers legitimately interleave
   window adjusts and early shell output around those replies, so a
   perfectly healthy OpenSSH session reported *"server refused to
   start a shell"*. Channel traffic is now handled by a shared
   `handle_channel_msg()` valid in every post-open state, and only
   `CHANNEL_SUCCESS`/`CHANNEL_FAILURE` advance the state machine.
6. **Data loss while stalled.** `ssh_proto_feed()` returned without
   consuming while awaiting a host-key decision — but its caller is a
   TCP receive callback with a borrowed buffer, so those bytes were
   gone.

## Not implemented

- Port forwarding, X11, agent forwarding, subsystems, SFTP.
- Compression (`zlib` would be larger than this whole client).
- Public key authentication. `crypto_ed25519_sign` is available and
  this is the obvious next feature; it needs a key store first.
- `known_hosts` persistence (`ssh_hostkey.c`).
- Multiple channels — `tcp.c` has one TCB, so there is one session.

`pty-req` sends a fixed 80×25, matching `VT_COLS`/`VT_ROWS` in
`zvt100.h`. `term` windows are not resizable, so there is no
`window-change` story to implement.
