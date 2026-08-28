# SSH client

Status: **crypto layer complete and tested; session layer not yet
written.** See `docs/ssh.md` for the full plan.

## Here now

| File | Status |
|---|---|
| `ssh_sha256.c/h` | done — FIPS 180-4 vectors pass, incl. streaming |
| `ssh_wire.c/h` | done — RFC 4251 codecs, bounds-checked |
| `ssh_crypto.c/h` | done — `chacha20-poly1305@openssh.com`, KDF, fingerprints |
| `ssh_proto.c/h` | done — full session engine, tested against a real server |

## Still to write

| File | Purpose |
|---|---|
| `ssh.c/h` | binds `ssh_proto` to `tcp.c`; `telnet.h`-shaped API |
| `ssh_hostkey.c` | `known_hosts` via `zfsapp.h` |

KEX, auth and the channel all live in `ssh_proto.c` rather than
separate files. They were planned apart and turned out to be one state
machine with one buffer; splitting them would have meant exposing that
buffer three ways for no gain.

Plus integration: `net.c` (port provider + `Z_NET_SSH_PREPARE`) and
`repl.c` (the `ssh` command).

## Testing

The three finished files have no Zeitlos dependencies beyond
`sw/ext/monocypher`, so they build and run on the host:

    gcc -O2 -Issh -Isw/ext/monocypher -o t_ssh t_ssh.c \
        ssh/ssh_wire.c ssh/ssh_crypto.c ssh/ssh_sha256.c \
        ../../ext/monocypher/monocypher.c

That property is worth preserving as the rest lands: the parsing and
crypto are exactly the parts where a bug is silent, and exactly the
parts that need to be testable without a board.
