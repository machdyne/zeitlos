# TRNG

A hardware entropy source (`rtl/trng.v`) and the system CSPRNG built on
it (`sw/common/zrng.c`), available to every app, to the kernel, and to
Scheme as `(random)`.

Built because Zeitlos had no source of unpredictability at all. Nothing
in `rtl/` produced one and nothing in `sw/` collected one, which was
fine while the only consumers would have been games — and became a hard
blocker for the SSH client, whose security rests almost entirely on an
ephemeral key nobody can guess. A weak key there is not a degraded
session, it is an open one, and nothing about it looks wrong from
either end.

## The split, and why it is the whole design

    rtl/trng.v          harvest physical jitter, debias it, prove it is
                        still alive, deliver ~1,400 words/sec
    sw/common/zrng.c    hash that into a ChaCha20 key and generate every
                        actual random byte at memory speed

Raw ring-oscillator output is biased, autocorrelated, and its quality
depends on how a particular place-and-route happened to lay out the
loops. **It is entropy, not random numbers, and nothing should use it
directly as a key.** Reading more of it does not fix this.

So the hardware is slow on purpose and the software does the real work.
This is the same division Linux uses with `RDSEED`, and it is why
`z_rng_bytes()` can be called in a loop without thinking about cost.

It is also why there is no SHA-256 conditioner in fabric: it would cost
more LUTs than everything else in `trng.v` put together and buy nothing
that `zrng.c` does not already do for free on a CPU that is idle
waiting for the network anyway.

## Hardware

Eight ring oscillators — odd-length inverter chains, no stable state,
so they oscillate at a frequency set by propagation delay, which
jitters with temperature, voltage and silicon. Lengths are 13, 15, 17
… 27 stages, deliberately all different: identical rings tend to
injection-lock into a single source, which would quietly reduce eight
oscillators to one.

Their outputs are XORed, resolved through a three-stage synchroniser,
and sampled once every 256 system clocks. Sampling far slower than the
oscillators run is the point — jitter accumulates over time, so a fast
sample gets many bits carrying very little entropy each.

Von Neumann debiasing follows: sample pairs, `01`→0, `10`→1, `00` and
`11` discarded. This makes the output exactly unbiased for **any** fixed
input bias, which is the defect ring oscillators reliably have (a duty
cycle that is never quite 50%). It does not remove correlation between
samples — nothing in hardware does, and that is what the hashing in
`zrng.c` is for.

Bits are packed into 32-bit words and queued in an 8-deep FIFO.

Net rate: about 1,400 words/second. That sounds useless and is entirely
beside the point — seeding needs 256 bits **once**, which takes about
5.5 ms.

Lowering `SAMPLE_DIV` makes this faster and worse. Don't, without
measuring what comes out.

### Address map

`0x7000_04xx`, the fifth tenant of nibble `0x7` alongside `csrs.v`
(`00xx`), `cache.v` (`01xx`), `socctl.v` (`02xx`) and `rtc.v` (`03xx`).
Adding it widened `sysctl.v`'s tenant mask from `0x300` to `0x700`;
every existing tenant address has bit 10 clear, so nothing moved. The
only visible change is that `0x7000_05xx`–`07xx` now fall through to
`csrs.v` instead of aliasing onto an existing tenant, which is strictly
better and is what a sixth tenant will want.

Register details are in `rtl/trng.v`'s header. One sharp edge worth
repeating here because it has already caught us once: **a `CTRL` write
always loads the `ENABLE` bit**, so acknowledging a health failure with
`0x2` alone switches the oscillators off and nothing ever arrives
again. Write `0x3`. `rtl/tb/tb_trng.v` made this mistake first;
`z_rng_hw_clear()` gets it right.

## The risk that matters

**A combinational loop is exactly what every synthesis tool exists to
eliminate.** This is not theoretical. It happened on the first real
ECP5 build of this block, and it is worth walking through because the
failure is completely silent.

Written portably, as `assign ro_net[n] = ~ro_net[n-1]`, the chain does
not survive. Thirteen inversions is algebraically one inversion, so
yosys folds the whole ring into a single LUT:

    ro_net[0] = enable & ~ro_net[12]
              = enable & ~(~ro_net[0])      // 13 inversions
              = enable &   ro_net[0]

which is a latch that settles to zero — not an oscillator. `keep` on
the wire vector kept the *net names*; it did not stop abc from
restructuring the logic driving them.

And the giveaway was almost invisible. yosys reported eight loops, on
plausible intermediate wires, which looks exactly like success.
nextpnr then reported eight loops **one cell long each**, on
`ro_net[0]`, `[27]`, `[54]` … — the chain heads. The count was right.
Only the length was wrong.

Three things are done about it:

1. **Instantiated LUT primitives, not inverters.** `LUT4` on ECP5,
   `SB_LUT4` on ice40, `CC_LUT1`/`CC_LUT2` on GateMate. An opaque cell
   with a fixed `INIT` gives the optimiser no algebra to do. This costs
   portability — there is a `-DTRNG_RO_GENERIC` fallback, but it is
   **not safe without checking what came out**, because it is exactly
   the construction that failed above.

2. **`tools/check_trng.py`**, run on the build log, which parses
   nextpnr's loop report and verifies both the count *and the length of
   every loop* against `RO_BASE + 2i`. With the defaults it demands
   loops of 13, 15, 17, 19, 21, 23, 25 and 27 cells. A count-only check
   passes the broken build; this one does not.

3. **A continuous health monitor**, the two cheap tests from NIST SP
   800-90B §4.4: repetition count (32 identical raw samples in a row)
   and adaptive proportion (ones outside 410–614 in a 1024-sample
   window). Both cutoffs are far looser than the standard's formulas
   demand, deliberately — this is here to catch a *dead* source, and a
   false positive would be a self-inflicted denial of service on every
   consumer of randomness in the system. A dead bank fails within 32
   samples, under 200 µs from reset.

The failure flag is sticky. A source that was dead for a millisecond
produced words somebody may already be holding, and "it's fine now" is
not an answer to that. Acknowledging it also flushes the FIFO.

**Check `HEALTH` on real hardware after any toolchain change, not just
after a code change.** This is the one part of the system whose
correctness depends on the synthesiser *not* doing its job well.

## Software

`sw/common/zrng.h` / `zrng.c`. ChaCha20 in fast key erasure form — the
same construction as OpenBSD's `arc4random`: every call generates 32
bytes of keystream for the *next* key alongside the caller's output,
then overwrites the key. An attacker who reads process memory at time T
learns nothing about bytes handed out before T. Cost is one extra
ChaCha block per call.

ChaCha20 is implemented in `zrng.c` rather than pulled from
`sw/ext/monocypher` on purpose: this file is linked by ordinary apps
that want `(random)` and nothing else, and making every one of them
depend on a 23 KB crypto library for 1.5 KB of stream cipher is a bad
trade. Measured at `-Os` for rv32im: **2,400 bytes of text, 112 bytes of
bss.**

### Two questions that are not the same question

    z_rng_present()   is there a TRNG in this bitstream?
    z_rng_secure()    is it present, healthy, and did we seed from it?

Almost nothing should call the first. The second can be false on a
bitstream that *does* have the hardware — an optimised-away oscillator
bank reports itself unhealthy and gets refused here rather than quietly
producing predictable output.

**The generator always works; its quality is what varies.** With no
usable TRNG it falls back to cycle-counter jitter and sets
`z_rng_secure()` false permanently. That fallback is genuinely useful —
a game wants unpredictable-to-a-human, and forcing every caller to
handle a failure return means most of them handling it badly.

    shuffling, games, jitter, backoff, ids  →  just call z_rng_bytes()
    keys, nonces, anything an attacker sees →  check z_rng_secure() and refuse

The SSH client is the second case and refuses to connect. It does not
warn and continue.

### Presence check ordering is not optional

Reading `reg_trng_magic` on a bitstream built before `rtl/trng.v`
existed touches an address nothing decodes, and an undecoded address on
this bus **gets no ack at all** — the CPU waits forever. A dead hang,
not a garbage read. The CSR feature bit at `0x7000_0008` is decoded by
every bitstream ever built, so it is the only safe first probe.
`z_rng_present()` does this in the right order. Same hazard
`z_rtc_available()` and `z_icache_flush()` already document.

## Scheme

    (random)            a real in [0, 1)
    (random n)          an integer in [0, n)
    (random-hex n)      n random bytes as a 2n-character hex string
    (random-secure?)    #t if hardware-seeded and healthy
    (random-stir s)     mix a string into the pool

`(random n)` is rejection sampled inside `z_rng_below()`, so it is
uniform rather than `mod n` with its bias toward small values.

`(random)` divides a 32-bit draw by 2³². ms numbers are doubles, whose
53-bit mantissa holds that exactly — but only 2³² distinct values can
come out, worth knowing before sampling a continuous distribution
finely.

`(random-hex n)` returns bytes because its callers want keys, tokens
and nonces, which are measured in bytes; hex because ms strings are
NUL-terminated and cannot hold a zero byte. Capped at 256 bytes per
call — not a security limit, just a bound on one allocation from a heap
`repl` shares with everything else it is running.

`(random-stir s)` absorbs one-way, so it cannot make the generator
worse whatever is passed in — and it never makes `(random-secure?)`
true. Nothing here can measure how much entropy a caller's bytes
carried, and guessing generously is how a system ends up believing it
is seeded when it isn't.

## nextpnr and combinational loops

nextpnr **hard-errors** on this, and `--timing-allow-fail` does not
help — the failure is in loop detection, before timing results are
judged:

    ERROR: Timing analysis failed due to combinational loops.

`--ignore-loops` is the flag for it, and the ice40 and ECP5 rules in
the top-level `Makefile` now pass it. The GateMate flow goes through
`nextpnr-himbaechel`, which may spell this differently or not need it;
that path is untested here.

If your nextpnr build predates `--ignore-loops`, the options are to
upgrade it, or to build with `TRNG` commented out in `rtl/boards.vh` —
in which case everything still works and `(random-secure?)` returns
`#f`, which is the honest answer for such a bitstream.

## Verifying it

`rtl/tb/tb_trng.v` covers the sampler, debiaser, packer, FIFO, health
monitor and bus — 15 checks, all passing:

    cd rtl/tb
    iverilog -g2005 -DTRNG_SIM -o tb_trng.vvp tb_trng.v ../trng.v
    ./tb_trng.vvp

`TRNG_SIM` replaces the oscillator bank with LFSRs, because a
zero-delay combinational loop never converges in an event-driven
simulator — iverilog simply hangs. **That means the testbench does not
and cannot test the entropy source.** Whether the oscillators oscillate
is a question for a board and the `HEALTH` bit.

At build time:

    make BOARD=... 2>&1 | tee build.log
    python3 tools/check_trng.py build.log

Expect `8 ring oscillators intact, lengths [13, 15, 17, 19, 21, 23, 25,
27]`. Anything else means the bank did not survive synthesis; the
script says what to do about it.

On hardware, in order:

1. `info` (or the kernel's boot report) lists `trng` among the
   features. If not, `make flash` — this is a gateware change, not a
   software one.
2. `(random-secure?)` at a `repl` prompt returns `#t`. If it returns
   `#f` with the feature bit set, the oscillators are not oscillating.
3. `(random-hex 32)` twice returns different strings. Identical output
   means the FIFO is not popping or the source is stuck.
4. Dump a few hundred KB through `write-file` and run `ent` or
   `dieharder` on it from a real machine. This tests the *CSPRNG*, which
   would pass even with a dead entropy source — it is a check on the
   plumbing, not on the physics. Step 2 is the one that tests the
   physics.

The ChaCha20 core in `zrng.c` was validated byte-for-byte against
Monocypher's `crypto_chacha20_djb`, and the generator's output measured
at χ² = 257.0 over 320,000 bytes (df = 255) with `z_rng_below(7)`
uniform to within 0.7%.

## Cost

Fabric cost is small — eight short inverter chains, a handful of
counters, an 8×32 FIFO and the Wishbone interface, on the order of 150
LUT4 on ECP5. It needs no pins, no external part and no board support,
which is why `TRNG` is defined at the universal level in
`rtl/boards.vh` like `RTC`.

## Future work

- **Feed real entropy in.** `z_rng_stir()` exists and nothing calls it
  yet. The natural sources are keystroke timings in `term` and packet
  arrival jitter in `net` — both are cheap, both are unmeasurable (so
  neither counts toward `z_rng_secure()`), and both make two boards
  with identical hardware diverge.

- **Seed persistence.** A 32-byte seed file rewritten each boot would
  make the fallback path meaningfully better on a board without `TRNG`.
  Deliberately not done yet: it needs a decision about where OS state
  lives on the filesystem, which is a bigger question than this.

- **An `rori` instruction.** RV32IM has no rotate, so each of ChaCha20's
  four rotates per quarter-round costs three instructions instead of
  one. The Zbb subset would cut ChaCha20 by roughly 40% here — and by
  the same margin in the SSH bulk cipher, where it matters much more.
  Cheaper than a crypto accelerator and useful to SHA-256 as well.
