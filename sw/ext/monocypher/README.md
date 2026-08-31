# Monocypher 4.0.2

Upstream: https://monocypher.org / https://github.com/LoupVaillant/Monocypher

Vendored, not a submodule. Every other thing in `sw/ext/` (`ms`, `te`)
is a submodule because it is a Machdyne project that gets developed
alongside this tree. This is neither -- it is a frozen, audited release
of somebody else's library that we want byte-for-byte reproducible and
never want to accidentally track `master` of. CC0 makes copying it in
unambiguous.

## Provenance

Tag `4.0.2`, from the upstream tarball. Verify with:

    sha256sum sw/ext/monocypher/*.c sw/ext/monocypher/*.h

    02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d  monocypher.c
    fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3  monocypher.h
    97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b  monocypher-ed25519.c
    3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce  monocypher-ed25519.h

**Unmodified.** If any of those hashes ever stop matching, something
has been edited in place -- which is the one thing that must not happen
to a vendored crypto library. Fixes go upstream; local needs get their
own file next to the caller (see `ssh_sha256.c`).

The only structural change from upstream is layout: `src/monocypher.*`
and `src/optional/monocypher-ed25519.*` are flattened into this one
directory. `monocypher-ed25519.c/h` both `#include "monocypher.h"`, so
a flat directory is what they expect.

## What we use, and what it costs

| Function | Used for |
|---|---|
| `crypto_x25519`, `crypto_x25519_public_key` | `curve25519-sha256` key exchange |
| `crypto_ed25519_check`, `crypto_sha512_*` | `ssh-ed25519` host key verification |
| `crypto_chacha20_djb` | `chacha20-poly1305@openssh.com` |
| `crypto_poly1305_init/update/final` | the same, incrementally |
| `crypto_verify16/32`, `crypto_wipe` | constant-time compare, key erasure |

Measured for `rv32im -Os` with `--gc-sections` (which `sw/apps/net`'s
Makefile already enables): **23.4 KB linked**. The unlinked
`monocypher.o` is 63 KB -- BLAKE2b's unrolled compressor alone is
23.5 KB and drops out entirely, because nothing in the SSH path uses
it. Deepest single stack frame is 1,088 bytes
(`crypto_eddsa_check_equation`).

Do not add `-DBLAKE2_NO_UNROLLING` or similar to trim this. Section GC
already removes everything unused; the knob would only shrink code that
is not being linked anyway.

## There is no SHA-256 in here

Monocypher provides BLAKE2b and SHA-512, and no SHA-256. SSH needs it
for both the `curve25519-sha256` exchange hash and RFC 4253's key
derivation, so `sw/apps/net/ssh/ssh_sha256.c` supplies it.

That file is deliberately *outside* this directory. Adding it here
would break the "unmodified, hash-checkable" property above, and the
next person to update Monocypher would either lose it silently or have
to re-merge it by hand.
