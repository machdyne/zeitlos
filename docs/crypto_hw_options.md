# Hardware acceleration options

Target: ECDSA P-384 verification from 12.1s to about 1.2s — ten-fold
— and hashing sped up alongside it. Measured on Lakritz (PicoRV32 at
48MHz, 8KB I-cache, no D-cache, SDRAM at ~13 cycles a word).

## Where the time is

One P-384 verification is ~9,650 field multiplies at ~60,000 cycles
each. A field multiply is 144 multiply-accumulates (~39k cycles,
nearly all SDRAM stalls) plus the Solinas reduce (~21k cycles: int64
adds on rv32 are two adds with carry handling, and they touch memory
too). The DSP multiplier itself is idle most of the time.

**Ten-fold means a field multiply under ~6,000 cycles.** That one
number decides most of what follows.

## The options

### A. Multiply-only block: n×n → 2n limbs — NOT ENOUGH

Hardware does the 144 multiplies in ~150 cycles; software still does
the reduce at ~21k. Field multiply ≈ 22k cycles. **~2.7×.** The
reduce is now the bottleneck, so accelerating only the multiply
leaves most of the cost behind. Rejected.

### B. Montgomery multiply block (CIOS) — THE RIGHT ONE

Hardware does multiply *and* reduce: `R = A·B·2^(-32n) mod N` for
any odd `N` up to 12 limbs. One 32×32 DSP multiply per cycle, `2n²`
steps: **~300 cycles for P-384.**

Transfer per multiply: write A (12 words), B (12), GO; read R (12).
N and `n0inv` are loaded **once per verification**, not per multiply.
At ~4 cycles per MMIO access that is ~150 cycles. **~450 cycles per
field multiply against 60,000 — about 130×.** With the additions,
subtractions and point bookkeeping that stay in software, a P-384
verification lands around **0.2–0.3s. That is 40–60×**, comfortably
past the target.

What it costs:

| | |
|---|---|
| registers | A, B, N, T: ~50 words = ~1,600 flops, or LUT RAM. **No BRAM.** |
| arithmetic | one 32×32→64 multiply (4× `MULT18X18D`) and a 64-bit adder |
| control | a counter-driven FSM, ~150 lines of Verilog |
| interface | wishbone slave in the existing peripheral space |
| timing | the multiply is pipelined one stage; nothing else is on a long path |
| feature bit | a CSR flag like every other optional block, so software falls back |

**Prime-agnostic.** It is a Montgomery multiplier, not a P-384
multiplier: the same block serves P-256, the scalar field `n` (which
has no Solinas form), and — up to its limb limit — anything else.

**Not RSA-2048 at full width.** 64-limb operands would be ~8K flops
of registers or BRAM, which is the wrong trade. RSA verification is
6.9s and early anchoring already skips it on ECDSA chains. It can
stay in software.

**Software change:** field elements go back to Montgomery form when
the block is present, so `fe_mul()` is a ~40-line MMIO wrapper. The
Solinas path stays as the fallback for boards without the block; the
representation is chosen once at `curve_setup()`.

### C. A full ECC scalar-multiplication engine — OVERKILL

Point doubling and addition in hardware. Thousands of lines, curve-
specific, and the software above it becomes a thin shell that is
hard to test. It would be faster than B by perhaps 3× — from 0.25s
to 0.08s — and none of that is visible next to a 26-second body
transfer. Rejected.

### D. A data cache — HELPS EVERYTHING, NOT TEN-FOLD

The CPU has an I-cache and no D-cache, and every loop in this
profile is stalled on SDRAM. A D-cache would plausibly give 2–4× on
the HTML parser (12s), ChaCha20 (3.9s), the relay path, and the
software crypto. It would not reach ten-fold on ECDSA alone, and it
is a large change: `rtl/cache.v` caches physical instruction
addresses after the MTU, and a D-cache adds coherency questions with
VRAM and DMA that do not exist today.

**Worth doing eventually, and worth doing separately.** It is the
only option that touches the parser and the body path, which is
where the page load will be once verification is fast.

### E. Custom instructions via PCPI — NOT ENOUGH

A multiply-accumulate or add-with-carry instruction cuts the inner
loop's instruction count but not its memory traffic, and memory is
where the cycles are. **~1.5–2×.** Also ties the code to PicoRV32.
Rejected on its own; irrelevant once B exists.

### F. Bit-manipulation (Zbb rotate) — FOR THE HASHES ONLY

Nothing in ECDSA rotates. See below.

## The hashes

This needs an honest correction first. **SHA-256 is not measurable in
this profile.** It hashes the TLS transcript (~5KB), the HKDF/HMAC
inputs (bytes), and 147 root-store names once. Its total per page
load is well under 100ms and it never appeared in any timing line.
A SHA-256 block would be a standard piece of RTL — the compression
function in ~2–3K LUTs, 64 cycles per 64-byte block, no BRAM — and
**it would buy nothing visible for the browser.**

It may be worth having anyway: SSH uses SHA-256 HMAC on every packet,
and a hardware block would serve that. But that is a different
justification and should be measured there first.

**The symmetric cost that IS visible is ChaCha20-Poly1305: 3.9s per
258KB body.** ~730 cycles a byte. Two ways to attack it:

| | gain | cost |
|---|---|---|
| Zbb `ror` | each quarter-round has four rotates, three instructions each on rv32i; one with `ror`. **~1.3–1.5×** | a CPU option, if PicoRV32's build supports it; timing-neutral |
| ChaCha20 block | the 512-bit state and quarter-round datapath in hardware, ~80 cycles per 64-byte block. **~50×**, taking 3.9s to under 0.1s | ~2K LUTs, no BRAM; Poly1305 stays in software |

`-O2` for `monocypher.c` is a free experiment before either: at
`-Os` the compiler may not be keeping ChaCha's 16-word state in
registers, and on this CPU that alone could be worth a factor.

## What ten-fold on the *page* needs

Verification is 36.5s of an 85s load. Option B takes it to under a
second — **the handshake goes from 46s to ~2s**, which is the
ten-fold asked for on those numbers. But the page load then reads:

| | now | after B |
|---|---|---|
| verification | 36.5s | ~0.7s |
| body transfer | 26.4s | 26.4s |
| HTML parse | 12.3s | 12.3s |
| decrypt | 3.9s | 3.9s |
| other | ~6s | ~6s |
| **total** | **85s** | **~49s** |

Ten-fold on the *page* needs the body path (networking, deferred)
and the parser too. B is the right first hardware step because it
removes the largest single item and has the best gain-per-line of
any option here; it is not the whole answer, and it is worth being
clear about that before building it.

## Built, and what it cost

`rtl/montmul.v`, `MONTMUL` in `boards.vh`, `FEATURES2` bit 3.

| | |
|---|---|
| 983 LUT4, 524 FF, 4 MULT18X18D, 32 DPR16X4 | measured with yosys |
| 517 cycles per multiply at 12 limbs | measured in simulation |
| P-384 verification 12.1s -> ~2.7s | measured on Lakritz |
| TLS handshake 46s -> 13s | measured on Lakritz |

**Both curves.** The block is 12 limbs wide and P-256 is zero-padded
-- the same integer with R = 2^384, which Montgomery permits since
the only requirements are R > N and gcd(R,N) = 1. That matters
because a single chain uses both: a P-256 leaf under P-384
intermediates is the common shape, and google.com and
en.wikipedia.org differ in exactly that way.

**The first version was 5,874 LUT4**, six times the estimate. The
whole difference was how the arrays were written, not what the block
computes: indexing them from half a dozen places and writing several
words per cycle gives flops behind multiplexers, not RAM. One read
port and one write port per array, and a compile-time limb count,
took it to 983.

### Three bugs that only showed up off-paper

- **The Montgomery constant was computed at the curve's width**, not
  the block's, so P-256 converted into the field with R = 2^256 while
  the block used 2^384. Caught by `hw_self_test()` ON HARDWARE, which
  is why that check exists.
- **The block holds ONE modulus**, and it was loaded once per curve at
  setup. A chain using both curves computed one of them against the
  wrong modulus, silently. Caught by the software model in tests.
- **The result read address was registered**, so every read returned
  the previous transaction's word. Caught in simulation.

None of the three was visible by reading the code, and each would
have produced wrong signatures rather than an error. The three layers
that caught them -- simulation for the block, a software model for
the code around it, and a self-test on the board -- are all load
bearing.

## Recommendation

1. **Build B**, the Montgomery multiplier. Verified in simulation
   against Python-computed products before it goes near a board.
2. Try `-O2` on `monocypher.c` and, if the core build allows it, Zbb.
   Both free.
3. Measure SHA-256 before deciding on a block for it; decide it on
   SSH's numbers, not the browser's.
4. Revisit the D-cache as its own project once the crypto is out of
   the way, because it is the only option that touches everything
   else in the table.

## Keccak (SHA-3, SHAKE): for the post-quantum algorithms

**Measured** (September 2026, `cryptobench`): one Keccak-f[1600]
permutation in software is **169,336 cycles (3.5 ms)** -- 64-bit lanes
on a 32-bit CPU with no data cache. It is 51-61% of each ML-KEM-768
operation ([mlkem.md](mlkem.md#measured-on-the-board)), and most of
ML-DSA verification (the post-quantum signature in zfed's migration
plan, [fed.md](fed.md#quantum-computers)) and nearly all of SLH-DSA's.

**The option**: a block like the SHA-256 one (rtl/sha256.v) -- a
compact core, one 64-bit lane at a time, the 1600-bit state in block
RAM, absorbing and squeezing through registers, OWNER-claimed. Moving
the state in and out (~50 register accesses each way at ~30 cycles)
would dominate, so an estimated **3,000-4,000 cycles a permutation,
~45x**; a core doing a round per cycle would hardly improve on that.
ML-KEM would roughly halve (its NTT arithmetic is the other ~45%);
ML-DSA and SLH-DSA verification become practical in volume.

**Where it fits**: an estimated ~1,000 LUT4 or so, to be confirmed by
synthesis -- room on the ECP5-45 boards (Sergei ML2); **not on Lakritz**
(511 LUT4 free) without taking something else out.

**Also possible, for every board**: a Keccak written for 32-bit CPUs
("bit-interleaved", as in the Keccak team's public-domain XKCP),
typically 1.5-2x faster there than the portable 64-bit code.

**Built** (docs/keccak_hw.md): the round-per-clock form, 6,005 LUT4 --
larger than the compact estimate above, and simpler; on Sergei ML2,
off on Lakritz.

**Not yet**: an NTT unit. After Keccak, ML-KEM's remaining time is its
polynomial arithmetic; worth revisiting once ML-DSA is measured.
