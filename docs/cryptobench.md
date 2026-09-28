# cryptobench -- what the primitives cost here

`sw/apps/cryptobench`. Measures, on the machine it runs on, the
cryptography that the BBS and zfed ([fed.md](fed.md)) depend on, so
decisions about algorithms and hardware are made from numbers rather
than estimates.

```
run cryptobench              (posix)
(run "cryptobench")          (repl)
```

It takes a minute or two. Results go to the serial console **and to
`/cryptobench.txt`** on the card -- a program's output goes to the
console by default ([posix.md](posix.md)), so the file is the easy
place to read them. Run it with nothing else busy: the cycle counter
is a wall clock, and time other processes take counts against the
number (each result is the fastest of repeated runs, which keeps that
small). Do not run it while `web` is loading a page: both use the
montmul block, which holds one modulus at a time.

## What it measures

Each software primitive twice: at `-Os`, as the tree builds it and as
`net` ships it, and at `-O2` (the same sources compiled again, their
symbols renamed `o2_*` by the Makefile). Every one is first checked
against a published answer -- SHA-256 and SHA-512 of "abc", BLAKE2b,
RFC 8032's first Ed25519 vector (key and signature), a good and a
tampered signature, RFC 7748's X25519 vector -- and a failure is
reported above the numbers it spoils.

| | why |
|---|---|
| SHA-256, per 64-byte block | zfed's object ids; the BBS's passwords; the case for a SHA-256 block |
| the BBS password hash, 200 and 2000 rounds | the ~5 s login (bbs.md, "Passwords") |
| SHA-512 and BLAKE2b, 1 KB and 16 KB | what standard Ed25519 hashes; what zfed no longer uses |
| Ed25519 sign; check of 32 bytes and of 16 KB | signing an object's id rather than its bytes |
| X25519 | the zfed and SSH handshakes |
| XChaCha20-Poly1305, 1 KB and 16 KB | zfed's session frames |
| montmul, one 2^255-19 multiply with its transfers, and its parts | whether Ed25519 on the block is worth building |
| **the SHA-256 block**, through `zsha256.c` as every app now links it: 4 KB, and the password hash | [sha256_hw.md](sha256_hw.md) |
| **montmul's register file**: one MUL and one ADD command | [montmul.md](montmul.md#the-register-file) |

The software SHA-256 lines stay software on a board with the block:
the Makefile builds those copies with `Z_SHA256_NO_HW`, so they remain
the comparison. A third copy is the driver as apps get it. Its known
answers ("abc", and a million `a` in misaligned pieces) are checked
through whichever path it takes. The montmul section claims the block
first, as every user now must, and says so if another program holds
it. The register file's MUL is checked against the classic interface's
product, and its ADD against schoolbook arithmetic, before either is
timed.

For montmul it also checks, before timing, that a value survives a
round trip into the Montgomery domain and back, and that a product
agrees with schoolbook arithmetic mod 2^255-19. On a bitstream without
the block it says so.

## Testing

`make -C sw/apps/cryptobench test` builds it for the host with a
software model of the montmul block (the CIOS operation of
`rtl/montmul.v`): the known answers and the Montgomery arithmetic are
checked there before they meet hardware. Host timings mean nothing.
A wrong Montgomery constant was tried on purpose and is caught.

## Results

The first run, 2026-09-27, 48 MHz, a montmul bitstream (12 limbs).
Fastest of repeated runs; `-Os` unless marked.

| | cycles | time | |
|---|---|---|---|
| SHA-256 | 44,000 / block | 0.92 ms / block | -O2 no different |
| BBS password hash, 200 rounds | 20.4M | **0.42 s** | |
| BBS password hash, 2000 rounds | 205M | **4.26 s** | the default: the slow login |
| SHA-512 | 1,130-1,230 / byte | 386 ms / 16 KB | -O2 10% slower |
| BLAKE2b | 1,130-1,170 / byte | 401 ms / 16 KB | -O2 40% slower |
| Ed25519 sign, 32 bytes | 11.9M | 249 ms | -O2 17% slower |
| **Ed25519 check, 32 bytes** | 34.5M | **718 ms** | -O2 20% slower |
| Ed25519 check, 16 KB | 52.5M | 1,094 ms | the difference is SHA-512 of 16 KB |
| X25519 | 24.9M | 519 ms | -O2 20% slower |
| XChaCha20-Poly1305 | 440-510 / byte | 150 ms / 16 KB | **-O2 8-10% faster** |
| montmul, one 2^255-19 multiply | 3,417 | 71 us | correct; transfers included |

A second run, with the montmul breakdown, agreed with the first to
within a few percent at `-Os`. (The `-O2` numbers moved by up to 20%
between the two builds -- code placement in the 8 KB instruction
cache, most likely; `-O2` gives no dependable gain either way.) The
multiply, split up:

| montmul, one 12-limb multiply | cycles | share |
|---|---|---|
| 24 words in (A and B) | 1,655 | 48% |
| start, and wait for the block | 699 | 20% |
| 8 words out | 439 | 13% |
| call and loop overhead around them | ~650 | 19% |
| *one register read, in a loop, for scale* | *31* | |

What they decided:

- **`-O2` is not the free win it looked like.** It slows everything
  built on 64-bit arithmetic -- SHA-512, BLAKE2b, Ed25519, X25519 -- by
  10-40%, and helps only ChaCha20, by 10%. The tree stays at `-Os`;
  `monocypher.c` at `-O2` for ChaCha alone is not worth a split build.
- **The 5-second login is the password hash**: 4.26 s at 2000 rounds.
- **zfed signs an object's id, not its bytes** (fed.md): checking a
  16 KB object signed whole costs 1.09 s, its id 0.72 s.
- **zfed hashes with SHA-256, not BLAKE2b**: per byte, SHA-256 is
  ~690 cycles, BLAKE2b ~1,150.
- **A signature check is 0.72 s**, a handshake about 1.8 s (two X25519
  and a check). fed.md's D5 is settled from these.
- **montmul as it is would speed Ed25519 up by at most ~2.6x** on its
  multiplies: 3,450 cycles against software's ~8,900 (worked out from
  X25519). **80% of the 3,450 is moving numbers** and the code around
  that; the block's own work is 20%. That decides what to change
  (fed.md, "Hardware").

### With the SHA-256 block and montmul's register file

Sergei ML2, 48 MHz, a bitstream with `SHA256` and `MONTMUL_REGS`
(timing: 52.25 MHz). The software lines agreed with the runs above to
within a few percent; every known answer was right, through both
blocks.

| | cycles | time | against software |
|---|---|---|---|
| SHA-256 through `zsha256.c` and the block | **1,390 / block** | 1.85 ms / 4 KB | 44,480: **32x** |
| BBS password hash, 2000 rounds | 30.2M | **0.63 s** | 4.23 s: **6.7x** |
| montmul, one MUL command, poll included | **776** | 16 us | classic 3,457: **4.5x**; a software field op ~8,600: **11x** |
| montmul, one ADD command, poll included | **190** | 4 us | a software field op ~8,600: **45x** |

Why these are more than simulation said (~180 for a block, ~561 for a
MUL): simulation counts bus cycles. On the CPU every register access
costs ~31 cycles with its loop, a pushed word is also a load from
memory with no data cache, and waiting on BUSY is a loop of such
reads. The blocks' own work -- ~144 cycles a SHA-256 block, ~700 a MUL
-- is now the smaller part.

Why the password hash gained 6.7x and not 32x: a round is 56 bytes,
two blocks, ~2,800 cycles in the block. The other ~12,000 cycles of a
round are the C around it -- the context, three small updates, the
padding, the copies, and the claim and the state's load and read-back
at each final. 0.63 s at 2000 rounds is fine for a login. If it ever
is not, the next step is a hash that keeps the block claimed and its
state inside across all the rounds, not a faster block.

### Using the register file: ECDSA, X25519, Ed25519

The same board and bitstream, with `web`'s ECDSA on the register file
and the binary-Euclid inverse, and `sw/common/z25519.c`:

| | before | now | |
|---|---|---|---|
| ECDSA P-256 verify, register file | ~1.6 s (classic registers) | **245 ms** | 6.5x |
| ECDSA P-384 verify, register file | ~2.7 s (classic registers) | **417 ms** | 6.5x |
| ECDSA P-256 / P-384, classic registers | ~1.6 s / ~2.7 s | 1.25 s / 2.28 s | the new inverse alone |
| X25519 | 544 ms (Monocypher) | **83 ms** | 6.5x |
| Ed25519 check | 722 ms (Monocypher) | **133 ms** | 5.4x |

The estimates made from simulation (~250 ms, ~50 ms, ~80 ms) were ~1.6x
low across the board: a command costs the CPU more to issue and wait
for than the ~40% simulation-to-board ratio of a single MUL suggested.
P-384 went from 1,082 ms to 417 with the inverse of s modulo n moved
from Fermat to binary extended Euclid (docs/montmul.md).

### ML-KEM-768 and the Keccak block

In software, first: key generation 247 ms, encapsulation 282 ms,
decapsulation 361 ms. Now, on Sergei ML2 with the Keccak block
([keccak_hw.md](keccak_hw.md#measured)): **114, 145 and 196 ms**; with
software Keccak, 237, 272 and 347 ms. A bare permutation is **4,233
cycles on the block against 132,542 in software** (31x). The first
figures quoted for "a permutation" were a whole SHAKE block, most of it
the sponge's byte handling -- the section "Keccak block, taken apart"
is what showed that.
