# Montgomery multiplier

`rtl/montmul.v` — a modular multiplier for public-key cryptography.

Optional, per board: `` `define MONTMUL `` in `rtl/boards.vh`, advertised
to software as `CSR_FEATURES2` bit 3.

## What it is for

TLS certificate verification. On a 48MHz RV32IM, verifying a
certificate chain in software takes long enough that servers hang up
mid-handshake — three ECDSA P-384 signatures were **36 of the 85
seconds** a page load took in `sw/apps/web`.

The block does one operation, the one those signatures are made of:

    R = A * B * 2^-(32n) mod N

Montgomery multiplication, for any odd modulus up to its built width.
Not curve-specific and not ECDSA-specific — it is the inner loop of
RSA, DH and every prime-field curve.

## Cost and speed

| | |
|---|---|
| 983 LUT4, 524 FF, 4 MULT18X18D, 32 DPR16X4 | measured with yosys, ECP5 |
| 517 cycles per multiply at 12 limbs (384-bit) | measured in simulation |
| **no BRAM** | four register files in distributed LUT RAM |

Against roughly 60,000 cycles for the same operation in software.
With the MMIO transfer included — 24 words in, 12 out, the modulus
loaded once per verification rather than per multiply — a field
multiply lands near 600 cycles.

Measured on Lakritz, end to end:

| | software | with the block |
|---|---|---|
| ECDSA P-384 verify | 12.1 s | **2.7 s** |
| ECDSA P-256 verify | ~2.5 s | **1.6 s** |
| TLS handshake | 46 s | **13 s** |

RSA is **not** accelerated: the block is 384 bits and RSA-2048 needs
64 limbs. See [crypto_hw_options.md](crypto_hw_options.md) for why
widening it would not help as much as it appears to.

## Design

A word-serial CIOS loop (Coarsely Integrated Operand Scanning), which
interleaves the multiply and the reduction so the intermediate never
exceeds n+2 words.

The multiplier has **two cycles of latency** — registered operands and
a registered product — which keeps a 32x32 multiply out of the
combinational path, so the longest path in the block is one 64-bit
add. The FSM absorbs the latency rather than the timing budget doing
it.

Every array access goes through **one read port and one write port**,
which is what makes yosys infer distributed RAM. The first version
indexed the arrays directly from half a dozen places and wrote several
words per cycle; that is not a RAM but 50 words of flops behind a pile
of multiplexers, and it synthesised to **5,874 LUT4** — six times the
estimate, and 95% device utilisation with the rest of the SOC in.
Several FSM states exist purely so that no cycle writes two words.

The limb count is a **synthesis parameter**, not a runtime register.
Removing that one register removed every dynamic comparison against
it. A narrower modulus is zero-padded, which is the same integer with
a larger R — Montgomery only requires R > N and gcd(R, N) = 1.

## Software

`sw/apps/web/ecdsa.c`, behind `-DEC_HW`. It checks the feature bit,
the block's `MAGIC`, and its `CONFIG` for the built width, then runs a
**self-test** before trusting it — `to_field(1)` and back, and the same
for `p-1` to exercise carries. On failure it prints and falls back to
software field arithmetic, which computes the same answers about
twenty times slower.

That check is not decoration. A half-wired accelerator otherwise
appears as invalid signatures on every site, which looks like a
certificate problem rather than a hardware one.

## Verification

`rtl/tests/tb_montmul.v` — 51 products against values computed in
Python, over P-384, P-256 and P-384's scalar field, corner cases
first: `(N-1)^2`, zero, one, `R mod N`.

    cd rtl/tests && make          # the vector suite
    cd rtl/tests && make cycles   # engine cycles per multiply

Three bugs that inspection did not catch: a two-cycle multiplier
latency treated as one, operand arrays read one word past their end,
and a stale product consumed for `m`. Each would have produced a block
that synthesised, ran, and returned wrong answers.
