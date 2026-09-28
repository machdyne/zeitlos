# ML-KEM: post-quantum key encapsulation

`sw/common/zmlkem.h` / `zmlkem.c`: **ML-KEM-768** (FIPS 203, 2024) over
the pq-crystals reference implementation, vendored in `sw/ext/mlkem`
(CC0 public domain, or Apache 2.0). Our interface is in `sw/common`; the
vendored code is unmodified, pinned to a commit (its README), and could
be swapped for **mlkem-native** -- the maintained implementation its
authors now recommend, of the same standard -- behind `zmlkem.h`
without a caller changing.

```c
zmlkem_keypair(ek, dk, coins64);             // coins: d then z, from the TRNG
zmlkem_encaps(ct, ss, ek, m32);              // false: ek fails FIPS 203's check
zmlkem_decaps(ss, ct, dk);                   // a tampered ct gives another secret
```

Randomness is always the caller's; the library's own `randombytes()`
is never reached (it is stubbed to halt, not to make a key from
nothing). The wrapper adds the input checks FIPS 203 requires and the
reference predates: every coefficient of an encapsulation key below q,
and a decapsulation key's copy of H(ek) right.

**Why**: a session recorded today could be decrypted by a quantum
computer later. zfed's handshake is hybrid -- X25519 and ML-KEM-768
together, secure while either holds ([fed.md](fed.md#quantum-computers)).
The same code can serve TLS's X25519MLKEM768 in `web` and OpenSSH's
`mlkem768x25519-sha256` in `net` and netserve later.

## Timing leaks

**KyberSlash** (2023-24): code that divided secret values by q leaked
them through the divide instruction's timing. The fixes are in this
version (divisions by q became multiplications). And
`make -C sw/common/tests -f Makefile.zmlkem` compiles the library for
this CPU and **fails the build if a divide or remainder instruction
appears anywhere but the two named places known to divide only public
values** (shake128/shake256 dividing an output length by the block
size). Checked by putting the pre-fix code back into `poly_tomsg()`:
it passes all 360 functional vectors -- right answers, leaky timing --
and only this check catches it.

## Memory

The reference keeps polynomial vectors on the stack: a decapsulation's
deepest path is ~15 KB (`indcpa_enc` alone 12.4 KB, by
`-fstack-usage`). So `fed` and `cryptobench` are in the LARGE tier
(kernel.h); DEFAULT's 16 KB of stack and heap together would overflow,
silently -- there is no stack guard. Code: ~16 KB.

## Testing

`make -C sw/common/tests -f Makefile.zmlkem` (AddressSanitizer, UBSan):
360 cases --

- **NIST's official FIPS 203 vectors** for ML-KEM-768 (the ACVP files):
  25 key generations, 25 encapsulations, 10 decapsulations (invalid
  ciphertexts among them), 10 checks each of encapsulation and
  decapsulation keys;
- **kyber-py**, an independent implementation: 40 key pairs, 40
  encapsulations, 80 decapsulations -- half of them of tampered
  ciphertexts, whose implicit-rejection secret must match exactly --
  and 120 keys the checks must refuse or accept.

`cryptobench` checks known answers on the board, times key generation,
encapsulation and decapsulation, and one Keccak-f[1600] permutation.
Permutations per operation, counted on the host: key generation 43,
encapsulation 44, decapsulation 44 (+9 for the key check). The product
estimates Keccak's share of each -- the number that decides whether a
Keccak block (which would also serve ML-DSA and SLH-DSA) is worth its
LUTs.

## Measured on the board

`cryptobench`, 48 MHz, software (`-Os`):

| | time | Keccak's share |
|---|---|---|
| key generation | 247 ms | ~61% |
| encapsulation | 282 ms | ~55% |
| decapsulation (with the key check) | 361 ms | ~51% |
| one Keccak-f[1600] permutation | 3.5 ms (169,336 cycles) | |

A zfed handshake adds ~0.61 s for the initiator (key generation and
decapsulation) and ~0.28 s for the responder: once per session, so
software is fine. The permutation is the outlier -- 64-bit lanes on a
32-bit CPU with no data cache -- and it is what ML-DSA and SLH-DSA
verification are mostly made of, once per object. **The Keccak block**
(docs/keccak_hw.md) roughly halves each operation on Sergei ML2, and
moving the sponge's bytes directly on this little-endian CPU saves
5-9% more on every board: key generation **114 ms**, encapsulation
**145 ms**, decapsulation **196 ms** with the block; 237, 272 and 347 ms
without.
