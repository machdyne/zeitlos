# Crypto performance: where the time goes, and what to build

Measured on Lakritz (PicoRV32 at 48MHz, 8KB I-cache, no D-cache,
SDRAM) loading `https://en.wikipedia.org/wiki/Main_Page`:

| phase | time | share |
|---|---|---|
| certificate verification (3 × ECDSA P-384) | 46.7s | 54% |
| body transfer (IPC + loss, see networking.md) | ~14s | 16% |
| HTML parse / index | 11.9s | 14% |
| body decrypt (ChaCha20-Poly1305, 258KB) | 3.9s | 5% |
| root store index, DNS, connect, storage | ~2.2s | 3% |

Verification dominates. The rest of this document is about it, with
a note on the two hashes at the end.

## Why it is slow: memory, not arithmetic

A P-384 ECDSA verification is roughly 9,650 Montgomery multiplies of
12 limbs each — about **2.8 million 32×32 multiply-accumulates**.
Measured at 15.5s, that is **~270 cycles per multiply-accumulate**
against a DSP multiplier that takes 2.

The multiplier is not the problem. The inner loop is

```c
pr = (uint64_t)a[i] * b[j] + t[j] + carry;
t[j] = (uint32_t)pr;  carry = pr >> 32;
```

— two loads, a store, and the 64-bit carry arithmetic that rv32 has
no single instruction for. With no D-cache, every one of those three
memory accesses is a ~13-cycle SDRAM round trip. The CPU is stalled
on data most of the time.

That rules some things out:

- **A faster multiplier or a MAC instruction does little.** It
  shortens the 2 cycles and leaves the ~40 of memory. Measured
  ceiling: perhaps 1.5×.
- **A rotate instruction (Zbb `ror`) does nothing here.** Montgomery
  multiplication has no rotates. It helps ChaCha20 and SHA-256 (see
  below), which together are under 6% of the load.
- **Better C helps some, and is worth doing** — but it cannot remove
  the memory traffic, only reduce the number of trips.

## What to do, in order

### 1. Software: Solinas reduction — DONE, and it under-delivered

`ecdsa.c` now reduces with the Solinas identity instead of a second
Montgomery pass. Measured on an x86 host at `-O2`:

| | before | after |
|---|---|---|
| P-256 | 1.17 ms | 1.12 ms |
| P-384 | 3.04 ms | 2.47 ms |

**About 1.2×, where 2× was predicted.** Worth being plain about why
the prediction was wrong: on x86 a 32×32 multiply is a few pipelined
cycles, so replacing 144 multiply-accumulates with ~100 additions
buys little. The argument for the target is that a multiply-add there
costs three SDRAM round trips and an addition on a small stack array
costs far less — so the gain should be larger on hardware than it is
here. **That is an argument, not a measurement**, and the hardware
number is the one that counts.

Two things learned in the process, both recorded in the code:

- The P-384 term table, transcribed by hand from FIPS 186-4 D.2.4,
  had five of ten terms shifted or reversed and passed 3 of 30 test
  products. It is now **derived from the prime** — `2^384 = 2^128 +
  2^96 - 2^32 + 1` read straight off — which cannot go wrong that
  way. P-256 keeps a table because its `2^224` term needs seven fold
  passes where the pre-expanded form needs one.
- The first P-256 version reduced each of the nine terms modulo p
  separately and came out **slower than the Montgomery reduction it
  replaced**. Accumulating all nine into one signed pass and
  normalising once fixed it. Fewer multiplies is not automatically
  faster if each one is wrapped in three other passes.

`tests/test_solinas.c` checks both reductions against products
reduced in Python, corner cases included. That test exists because a
wrong reduction is quiet: the curve arithmetic above it fails loudly,
but a reduction wrong for *some* inputs verifies most signatures and
fails a few, which looks like a network fault or a bad certificate.

### 2. Software: 4-bit windowing, not yet done

Precompute 16 multiples of each base point and consume four scalar
bits per step: 384 doublings and ~192 additions instead of 384 and
~288. Perhaps 1.2–1.3× on top of the above. Self-contained in
`shamir()` and checkable against the same vectors.



### 3. Hardware: a Montgomery multiplier block, ~80×

The accelerator that fits the problem is not a faster multiply but
**one that keeps the operands out of SDRAM**. Load A, B and N into
the block once, compute inside it, read the result back.

**Sketch:**

- Operand registers: A, B, N and result R, each 12 × 32 bits, plus
  `n0inv`. About 1,600 flops. **No BRAM.**
- CIOS Montgomery multiply as a small FSM around one 32×32→64 DSP
  multiply (two if parallelising the `a·b` and `m·n` passes). 12×12
  limbs is 288 multiply-steps; pipelined at 48MHz that is **~300–400
  cycles**.
- Limb count as a register (8 or 12) so P-256 and P-384 share it.
- MMIO interface, in the existing peripheral space: write operands,
  write GO, poll DONE, read R. ~40 MMIO accesses per multiply at ~13
  cycles each is ~500 cycles of transfer. **~1,000 cycles per
  Montgomery multiply, against ~78,000 today.**
- Advertised through a SOC feature CSR like every other optional
  block (`rtl/csrs.v`, `zsoc.h`), so `ecdsa.c` keeps its C path for
  boards without it and picks the block when present.

**Expected:** P-384 verification from 15.5s to **~0.2s**; the
three-signature chain from 47s to under a second; the handshake from
56s to ~2s. At that point verification moves back *before* Finished,
where `docs/tls.md` says it belongs.

**Timing:** a pipelined DSP multiply at 48MHz is comfortable on ECP5.
The FSM is a counter and a few muxes. This should not touch Fmax.

**Not RSA.** Registers for 4096-bit operands would be ~6KB of flops,
which is the wrong trade. RSA verification is 6.9s and, since
early anchoring, is skipped on ECDSA chains. It can use the block
limb-block-wise later if it ever matters.

### 4. Session resumption: avoid the work

Orthogonal to both. TLS 1.3 PSK resumption skips certificates
entirely on reconnect, and a page is 2–3 connections to the same
host. Even with the block, this is what makes *browsing* fast rather
than a single page load.

## RSA is now the bottleneck, and the accelerator cannot reach it

With ECDSA in hardware, an RSA chain is the slow case. Measured on
Lakritz against www.google.com:

```
verify: rsa-pkcs1-sha256 under rsa key: ok, 1762 ms   (2048-bit intermediate)
verify: rsa-pkcs1-sha256 under rsa key: ok, 6866 ms   (4096-bit root)
```

The server **closed the connection during the second one**, and the
request went out on a dead socket. Verification already happens after
Finished, so the handshake completed — Google's edge simply does not
wait 8 seconds for a client to say something.

### Why the Montgomery block does not help

It is 12 limbs, 384 bits. RSA-2048 needs 64 and RSA-4096 needs 128.
Widening it is possible — the arrays are distributed RAM and there
are RAMW slots spare — but **BRAM is at 55/56**, so a wide version
would have to stay in LUT RAM, and depth-cascading 128-word arrays
brings back the multiplexers that cost 5,000 LUTs the first time.

More importantly it would not fix the problem. An RSA verification is
17 Montgomery multiplies **plus** the setup that computes R^2 mod m,
and that setup is inherently about 32n modular doublings — 4,096 of
them at 128 limbs. The block multiplies; it does not double. Remove
the multiplies entirely and the doublings still take about a second.

There is no shortcut: every route to R^2 that avoids division is a
fixed point of Montgomery multiplication. R^2 is what gets you INTO
the domain, so it cannot be computed with an operation that assumes
you are already there.

### What was done: R^2 from R mod m

`bignum.c` built R^2 by doubling 64n times from 1. For a **normalised**
modulus — top bit set, which every RSA modulus has — R mod m is
simply R - m, one subtraction, which halves the loop to 32n.

That is 20% off a verification (host: 0.408 -> 0.326 ms for
RSA-2048), so roughly 6.9s -> 5.5s on hardware. The general path is
kept and still taken for any modulus that is not normalised, because
this file has been bitten before by assuming they all are.

### What would actually fix it

**TLS session resumption.** A page load is two or three connections to
the same host, and resumption skips certificates entirely on all but
the first. That is worth more than any accelerator: it turns the
cost from per-connection into per-host-per-session.

The chain cache already does the equivalent within a host, but only
after the first connection has paid in full.

## The hashes

**SHA-256** is used for the TLS transcript, HKDF and HMAC — a few
kilobytes per handshake. It is not on the critical path and a
hardware block for it would buy nothing measurable.

**ChaCha20-Poly1305** is 3.9s for 258KB, ~730 cycles per byte, and
it is the one place a rotate instruction helps: each quarter-round
has four rotates, each three instructions on rv32i. Zbb `ror` makes
them one. Perhaps 1.3–1.5× on decryption — worth having, not worth
prioritising over the multiplier.

The same no-D-cache cost applies: ChaCha's 16-word state lives in
memory during the round function unless the compiler keeps it in
registers, and at `-Os` it may not. `-O2` for `monocypher.c` is a
free experiment.

## What not to do

- **A data cache.** It would help everything, including the HTML
  parser — but it is a large RTL change with coherency questions for
  VRAM and DMA, and the multiplier block gets the crypto win at a
  fraction of the risk.
- **Reducing security to save time.** Skipping chain verification,
  or caching `CertificateVerify`, were both considered and rejected
  in `docs/tls.md`. The chain cache that *was* added caches only
  "this chain is trustworthy", which does not vary per connection.
