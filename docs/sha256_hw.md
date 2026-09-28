# SHA-256 block

`rtl/sha256.v` -- SHA-256 compression in hardware. Optional, per board:
`` `define SHA256 `` in `rtl/boards.vh`, advertised to software as
`CSR_FEATURES2` bit 12. **Software needs no change to use it**:
`sw/common/zsha256.c` finds it and uses it, and falls back to software
where it is absent or busy.

## What it is for

Measured on Lakritz ([cryptobench.md](cryptobench.md#results)),
software SHA-256 costs **~44,000 cycles a 64-byte block**. Everything
hashes with it: the kernel's password check, SSH (a MAC on every
packet), TLS (the transcript, HKDF), netserve's key checks, the BBS's
password hash (4.26 s at 2000 rounds -- a login that froze every
caller), and zfed, which names every object by its SHA-256
([fed.md](fed.md)). The block compresses a block in **~144 cycles**.
**Measured on the board, through `zsha256.c`: 1,390 cycles a block, 32x
software**, and the BBS's 2000-round password hash **0.63 s instead of
4.23 s** ([cryptobench.md](cryptobench.md#with-the-sha-256-block-and-montmuls-register-file),
which also says why these are not 144 and 32x).

Not SHA-384/512: zfed signs 32-byte object ids, so the SHA-512 inside
Ed25519 hashes ~96 bytes a check, a few milliseconds; TLS's SHA-384 is
too small to show in a page load; and a 64-bit design would cost about
twice this one's logic.

## Size

Yosys, ECP5: **760 LUT4, 131 CCU2C, 980 flip-flops, one DP16KD**
(the round constants), eight TRELLIS_DPR16X4 (the state).

Built for a board short of LUTs and not of flip-flops or block RAM:

- the message schedule is a 16-word **shift register** -- flip-flops,
  no multiplexers;
- the 64 round constants are a ROM **forced into block RAM**. Left to
  itself yosys builds 64 words from LUTs (~200 of them);
- the state is eight words of **distributed RAM**, loaded into and
  added back from the working variables one word a cycle through the
  same shift chain the rounds use;
- a round takes **two cycles**, so no path has more than two 32-bit
  adders in series. 72 cycles a block would save nothing visible:
  software spends longer pushing the block's sixteen words.

### Fitting

Place and route, with montmul's register file as well:

| board | logic | block RAM | system clock |
|---|---|---|---|
| Lakritz (ECP5-25F) | **97%** (23,777 of 24,288; 91% before) | 40 of 56 | **52.10 MHz**, passing at 48 |
| Sergei ML2 (ECP5-45F) | | | **52.25 MHz**, passing at 48 |

Lakritz has ~500 LUTs left: the next thing added there will need
something else taken out, and `MONTMUL_REGS` (~510 LUTs) is the first
candidate -- this block is the one everything uses. Enabled in
`rtl/boards.vh` on Lakritz and Sergei ML2, where both were measured.

## Registers

Base `0x7e00_0000`, decoded on address bits 27:24 as the probe is
(0x7f), since the eight 256-byte tenants of nibble 7 are all taken.
Word offsets:

| | | |
|---|---|---|
| 0 | MAGIC | `0x5A534841` ("ZSHA") |
| 1 | CTRL | write bit 0: START (compress the sixteen pushed words into H); read bit 0: BUSY |
| 2 | CONFIG | `0x53480001`: version 1 |
| 3 | OWNER | the claim: see "Sharing" |
| 4 | W | push a message word **as loaded from memory** (little-endian); the block swaps it to SHA-256's big-endian order |
| 5 | WBE | push a word already big-endian |
| 8-15 | H | the state H0..H7, as numbers |

Pushes and H writes are ignored while BUSY. Sixteen pushes make a
block; START compresses it into H.

## Sharing

One block, many processes -- and the kernel. Each call of
`z_sha256_update()` that has a whole block to compress:

1. claims the block: writes its pid to OWNER, reads it back, and uses
   the block only if it now owns it (the kernel, which has no pid,
   uses `0x7FFFFFFF`). Otherwise the call is done in software -- the
   answer is the same, a little later.
2. writes H from the caller's context,
3. pushes each block and starts it,
4. reads H back into the context, **writes zeros over H**, and releases.

So the block holds nothing between calls that anyone needs, and a
process can lose it to preemption or to another process between two
calls at no cost. The zeroing matters: H after a block is derived from
what was hashed, and for HMAC that is the key; the next owner could
read it. (The message schedule is not readable.) A process killed while
holding the block has its claim released and H cleared by the kernel
(`k_hw_release_pid()`, sw/os/kernel.c).

The claim is advisory -- the block computes for anyone -- which keeps
it to a few dozen LUTs.

## Testing

- `make -C rtl/tests sha256`: the RTL, in Icarus, against Python's
  `hashlib` -- the empty message, "abc", the two-block NIST vector, and
  lengths around every block boundary, through both W and WBE; MAGIC,
  CONFIG and the claim. Breaking a rotation, the constants' prefetch,
  the round count, or the final addition each fails it.
- `make -C sw/common/tests -f Makefile.sha256hw`: **the real
  `zsha256.c` driver against the real RTL**, under Verilator: the
  published vectors, a million `a` (15,626 blocks through the RTL), 300
  random messages in random pieces from misaligned addresses, the block
  held by someone else (the right answer, in software, their claim
  untouched), the block taken and junk left in H between two calls, and
  the block left released and zeroed after every hash. Leaving H
  uncleared, ignoring someone else's claim, swapping bytes in the
  misaligned path, or not waiting for BUSY each fails it. (Verilator does
  not relink when the driver's objects change; the Makefile removes the
  binary first -- without that, every one of those four breaks "passed"
  against a stale build.)
- `cryptobench` on the board: `zsha256.c`'s known answers through the
  block, and its speed ([cryptobench.md](cryptobench.md)).
