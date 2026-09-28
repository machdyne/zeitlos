# The Keccak block

`rtl/keccak.v`: Keccak-f[1600] -- the permutation under SHA-3, SHAKE,
ML-KEM, ML-DSA and SLH-DSA -- in 24 cycles. Optional, like the SHA-256
block and montmul's register file: `` `define KECCAK`` in
`rtl/boards.vh`. **On by default on Sergei ML2; off on Lakritz**, which
has no room -- a Lakritz owner who wants it can make room by leaving
something else out (audio, say) and adding the line.

`sw/common/zkeccak.c` uses it when it is there; every caller of
`zkeccak_f1600()` gets it without changing -- ML-KEM included, through
`sw/common/zfips202.c` (below).

## Why

Measured in software on the board: one permutation **169,336 cycles
(3.5 ms)** -- 64-bit lanes on a 32-bit CPU with no data cache -- and
51-61% of each ML-KEM-768 operation ([mlkem.md](mlkem.md)). ML-DSA and
SLH-DSA verification, the post-quantum signatures zfed's migration plan
points to ([fed.md](fed.md#quantum-computers)), are mostly or almost
all Keccak, once per object checked.

## Shape

One round per clock, the 1,600-bit state in flip-flops: the simplest
thing that is right. A faster core would not show: moving the state
over the bus costs far more than 24 cycles.

The bus does **not** address the fifty state words one by one: that
would put a five-way multiplexer in front of every one of the 1,600
bits. The state is a rotating 50-word shift register instead:

| word | name | |
|---|---|---|
| 0 | MAGIC | `0x5A4B4543` ("ZKEC") |
| 1 | CTRL | write bit 0 START, bit 1 CLEAR; read bit 0 BUSY |
| 2 | CONFIG | `0x4B450001` -- version 1 |
| 3 | OWNER | the claim, exactly as montmul's and the SHA-256 block's |
| 4 | IN | write: shift the state down a word, this one entering at the top -- fifty load it, word 0 first |
| 5 | XIN | write: the same, XORed into the word leaving -- absorbing without reading |
| 6 | OUT | read: the bottom word, and rotate -- fifty return it and leave it as it was |

Word k is bits 32k+31..32k; lane i (i = x + 5y) is words 2i and 2i+1:
a `uint64_t state[25]` as it lies in this CPU's memory. While BUSY, IN,
XIN, OUT and CLEAR are ignored. Base `0x7d00_0000`; `CSR_FEATURES2`
bit 13.

**Size**, synthesized alone for the ECP5: **6,005 LUT4, 1,677
flip-flops**, no block RAM. (An earlier estimate of ~1,000 was for a
compact lane-serial design; this is the round-per-clock one.)

## Fitting

**Sergei ML2, routed: 52.07 MHz against 48** (52.25 before the block).
Synthesized alone the block is 6,005 LUT4 and 1,677 FF; a synthesis of
Sergei ML1 with it, montmul's register file and the SHA-256 block
together came to 71% of the ECP5-45's logic cells.

## Measured

`cryptobench` on Sergei ML2:

| ML-KEM-768 | software Keccak | the block | |
|---|---|---|---|
| key generation | 256 ms | **121 ms** | 2.1x |
| encapsulation | 292 ms | **153 ms** | 1.9x |
| decapsulation (with the key check) | 372 ms | **205 ms** | 1.8x |
| one permutation, through SHAKE | 180,062 cycles | **31,155 cycles** | 5.8x |

**The permutation itself: 4,233 cycles through the block, 133,327 in
software -- 31x** (`cryptobench`, "Keccak block, taken apart"): fifty
IN writes 1,645 cycles, START and the wait 165, fifty OUT reads 1,604
-- 32 cycles an access -- and the claim, clear and release the rest.
All of 100 calls used the block.

The ~31,000 above was a whole SHAKE block, and **~29,000 of it was the
sponge around the permutation**: the reference moves bytes into and
out of the 64-bit lanes one at a time, each through a 64-bit shift --
slow on a 32-bit CPU. `cryptobench`'s "Keccak's share" had been
computed from that figure, and so counted the sponge as Keccak; it now
shows the bare permutation and the SHAKE block separately. With the
block the permutations are only ~3% of an ML-KEM operation.

**So `sw/common/zfips202.c` now moves those bytes directly on a
little-endian CPU** (a lane holds its bytes in memory in FIPS 202's
order already), on every board, with the block or without. Held to
ML-KEM's 360 vectors both ways -- the new paths and, forced off, the
reference loops -- and each of the five changed places, broken on
purpose, fails them.

**Measured after it** (Sergei ML2):

| ML-KEM-768 | the block | software Keccak |
|---|---|---|
| key generation | 125 -> **114 ms** | 250 -> **237 ms** |
| encapsulation | 157 -> **145 ms** | 287 -> **272 ms** |
| decapsulation | 210 -> **196 ms** | 366 -> **347 ms** |
| a SHAKE128 block | 33,517 -> **22,604 cycles** | 171,831 -> **157,084 cycles** |

A SHAKE block's cost around its permutation fell from ~29,000 to
~18,400 cycles; the rest is the sponge's own bookkeeping, not worth
chasing now. With the block, the permutations are ~2-3% of an ML-KEM
operation; ML-KEM's remaining time is its polynomial arithmetic (an NTT
unit, docs/crypto_hw_options.md, is the option if it ever matters).

## The driver

`zkeccak_f1600(state)`: claim the block, fifty IN, START, wait, fifty
OUT, **CLEAR**, release -- ~105 bus accesses. The clear matters: ML-KEM
puts secret seeds through Keccak, and the next owner could read them
out. If the block is absent or someone else holds it, the software
permutation (the pq-crystals reference's) answers, with the same result.
The kernel clears and frees the block if a process dies holding it
(`k_hw_release_pid()`), as it does montmul and the SHA-256 block.

`sw/common/zfips202.c` provides SHA-3 and SHAKE under the names the
vendored `sw/ext/mlkem/fips202.h` declares, built on
`zkeccak_f1600()`: the reference's own sponge code (public domain),
with only the permutation taken out. ML-KEM links it in place of its
own `fips202.c`, and so uses the block without the vendored code
changing.

## Testing

- `make -C rtl/tests keccak`: 24 permutations against a reference written
  from FIPS 202 in `gen_keccak_vectors.py` -- which first has to
  reproduce hashlib's SHA3-256, SHA3-512, SHAKE128 and SHAKE256 or it
  stops -- then: fifty OUT reads leave the state as it was, XIN xors,
  CLEAR clears, IN and CLEAR while busy are ignored, the claim. 30
  cycles a permutation, bus included. Broken on purpose -- a rotation
  offset, the pi step, chi, iota, theta's rotation, XIN's xor, CLEAR
  while busy -- each fails it. (Accepting IN while busy does not: the
  round already has priority over the bus in the state's own logic.)
- `make -C sw/common/tests -f Makefile.zkeccakhw`: `zkeccak.c` against
  **the real RTL under Verilator** -- 200 random states against the
  software permutation; **all 360 ML-KEM-768 vectors** (NIST's and
  kyber-py's, [mlkem.md](mlkem.md#testing)) with every permutation in
  the RTL; the block released and its state cleared after every call;
  held by someone else, not used. Skipping the clear, swapping a lane's
  halves, ignoring another's claim, not waiting for the permutation --
  each fails it. ~236 simulated bus cycles a permutation.
- `make -C sw/common/tests -f Makefile.zmlkem`: ML-KEM's 360 vectors
  through `zfips202.c` in software.
- `cryptobench` times ML-KEM and a permutation with the block and
  without.
