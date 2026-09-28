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
multiply was estimated to land near 600 cycles. **Measured, it is
about 3,450** ([cryptobench.md](cryptobench.md#results), a 12-limb
multiply driven from C the way `ecdsa.c` drives it): 24 words in 1,655
cycles, the block's own work 699, 8 words out 439, and ~650 of call
and loop overhead around them. The transfers, and the software
moving them, cost four times what the multiply does. See
[fed.md](fed.md#hardware) for what that suggests.

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

## The register file

Measured, a multiply driven through the registers above costs ~3,450
cycles, of which the block's own work is ~700 (see "Cost and speed").
So with `` `define MONTMUL_REGS `` in `rtl/boards.vh` the block also
keeps **sixteen registers** of its width in **one block RAM**, and runs
commands on them. Software loads its operands once, issues one-word
commands, and reads back only what it needs:

| word | | |
|---|---|---|
| 2 | CONFIG | bits 15:8 now give the register count -- 0 without `MONTMUL_REGS`, and on older bitstreams. Bits 7:0 are the width, as before |
| 4 | OWNER | the claim: see "Sharing" |
| 5 | RSEL | `{ reg (4 bits), word (4 bits) }`: where RDATA starts |
| 6 | RDATA | a register's word; reading or writing moves RSEL on by one, so `width` accesses load or fetch a register |
| 7 | CMD | `{ op [15:12], rd [11:8], ra [7:4], rb [3:0] }` starts a command; bit 0 reads BUSY |

| op | | simulation, bus cycles | **on the board**, issued and polled from C |
|---|---|---|---|
| 1 MUL | `rd = ra * rb * R^-1 mod N` | ~561 | **776** (classic registers: 3,457) |
| 2 ADD | `rd = ra + rb mod N` | ~81 | **190** |
| 3 SUB | `rd = ra - rb mod N` | ~69 | ~190 (as ADD) |

A software field operation costs ~8,600 cycles here (worked out from
X25519's 24.1M), so a MUL is ~11x one and an ADD ~45x.

N and N0INV are the classic registers', loaded as before. Operands
must be below N; `rd` may be `ra` or `rb`. A MUL copies its registers
into A and B, runs **exactly the engine above**, and copies the result
back; ADD and SUB share one 33-bit adder with the engine's final
subtraction. Commands overwrite A, B and R, so within one computation
use one interface or the other -- which is what the claim is for.

**Everything that was there is unchanged**: every register at its
address, doing what it did, the multiply still 517 cycles. Without
`MONTMUL_REGS` the register file and its states are not built at all.

| yosys, ECP5 | LUT4 | CCU2C | FF | DP16KD |
|---|---|---|---|---|
| before | 676 | 141 | 524 | 0 |
| now, without `MONTMUL_REGS` (the claim register) | 752 | 141 | 555 | 0 |
| now, with `MONTMUL_REGS` | 1,186 | 145 | 703 | 1 |

(montmul.md's earlier 983 LUT4 was from an older yosys.)

**Every command takes the same number of cycles whatever its
operands**: SUB always runs its add-back pass, adding N or zero, since
X25519 on secret scalars runs here. `make -C rtl/tests rf` checks it.

What uses it:

- **`web`'s ECDSA** (`sw/apps/web/ecdsa.c`): the double-scalar
  multiplication of a P-256 or P-384 check runs on the registers, the
  running point staying in the block and each table point (made affine
  first) loaded for its addition; the final affine x comes out with an
  inversion by exponentiation, in the block too. An addition whose two
  points share an x coordinate -- where the formula would divide by
  zero, and which an attacker can arrange -- makes it give up and the
  classic path answer. Tested on the host with a model of the block
  against 384 signatures made by Python's `cryptography` (each valid,
  with a hash bit flipped, and with s replaced by n - s; the keys 1
  and n - 1 among them): all right, 374 through the register file and
  10 handed back. Leaving out the equal-x check makes the key-1 cases
  go wrong -- which only those cases show.

  **Measured on the board**, through google.com: a P-256 check
  **410 ms** (1.6 s through the classic registers), a P-384 check
  **1,082 ms** (2.7 s). Most of what was left was not the curve at all:
  the inverse of s modulo n, by Fermat's exponentiation in software --
  ~670 multiplies, ~840 ms of the P-384 check. It is now binary extended
  Euclid (`inv_mod()`), shifts and subtractions, for the field's
  inverse too on the paths that still need one. Public values only: its
  time depends on the value. Tested against the Fermat inverse it
  replaced on 10,592 values -- both curves' p and n, the edges (1, 2,
  m-1, m-2, (m+1)/2, every power of two, all-ones and alternating
  patterns) and random ones -- and all 384 signatures on every path.
  **Measured after it: P-256 245 ms, P-384 417 ms** on the register
  file (from 410 and 1,082), and 1.25 s / 2.28 s through the classic
  registers (from ~1.6 / ~2.7 s).
- **X25519 and Ed25519 checks** (`sw/common/z25519.c`,
  [z25519.md](z25519.md)): SSH, netserve, TLS, and zfed next.

## Sharing

The block holds **one modulus for the whole machine**, and more than
one program now uses it. `OWNER` (word 4, on every bitstream from this
version): write your pid to claim the block if nobody has it; read it
back and use the block only if it is yours; write `pid | 2^31` to
release it. A pid of 0 claims as `0x7FFFFFFE`. The claim is advisory
-- the block computes for anyone -- and costs a few dozen LUTs. The
kernel releases a dead process's claim (`k_hw_release_pid()`,
sw/os/kernel.c). On an older bitstream `OWNER` reads back 0: there is
nothing to share with, and the block is used as before.

`ecdsa.c` claims the block around each `ec_verify()`, and on each
claim forgets which modulus it last loaded, so it loads its own again
before its first multiply -- a few dozen writes a verification.
Without that, a second user loading 2^255-19 would leave `web`
computing with the wrong modulus: silently, with plausible results. If
the block stays busy for ~2 s, that process switches to software for
good (its curve constants rebuilt in software's form: the two forms
cannot be mixed within a verification).

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

    cd rtl/tests && make classic  # the classic interface: 51 products
    cd rtl/tests && make rf       # the register file
    cd rtl/tests && make cycles   # engine cycles per multiply

`rtl/tests/tb_montmul_rf.v` runs random programs -- sixteen registers
holding 0, 1, N-1 and random values, eighty random MUL, ADD and SUB
commands -- over P-384, P-256, P-384's scalar field and 2^255-19, and
checks every register against a Python model
(`gen_montmul_rf_vectors.py`); also the claim, CONFIG, the window's
auto-increment, and the classic interface between programs. Removing
the cycle that lets the carry's write land before the final subtract,
skipping the add-back after a borrow, skipping the carry altogether,
or reading the block RAM without its one-cycle offset each fails it.

Its first version found a testbench bug rather than a design one: the
bus tasks held the request a cycle past the ack, which the block
rightly takes as a second access -- harmless to every register before
this one, and every other word skipped by RDATA, which advances on
each access. The CPU's bus drops the request on the ack; the tasks now
do too.

Both testbenches, and montmul.v itself, used a signal before
declaring it; iverilog 14 refuses that, older versions did not. The
declarations moved; nothing else did.

Three bugs that inspection did not catch: a two-cycle multiplier
latency treated as one, operand arrays read one word past their end,
and a stale product consumed for `m`. Each would have produced a block
that synthesised, ran, and returned wrong answers.
