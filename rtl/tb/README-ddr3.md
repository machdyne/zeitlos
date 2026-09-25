# DDR3 testbenches

Run from `rtl/tb`:

```
# controller, against a checking DRAM model -- 34 checks
iverilog -g2012 -o /tmp/t tb_ddr3.v ddr3_model.v ddr3_phy_sim.v ../mem/ddr3_ctrl.v
vvp /tmp/t

# read burst assembly
iverilog -g2012 -o /tmp/a tb_ddr3_rdasm.v ../mem/ddr3_rdasm.v
vvp /tmp/a

# register block: TUNE applied under PAUSE
iverilog -g2012 -o /tmp/r tb_ddr3_reg.v ddr3_ecp5_stubs.v ../mem/ddr3.v \
    ../mem/ddr3_ctrl.v ../mem/ddr3_phy_ecp5.v ../mem/ddr3_rdasm.v
vvp /tmp/r

# project rules: plain Verilog, no local declarations
./check_ddr3_style.sh
```

Each ends with `== PASS` or `== FAIL`.

## tb_ddr3.v -- the controller

`rtl/mem/ddr3_ctrl.v` through `ddr3_phy_sim.v`, a behavioural PHY,
against `ddr3_model.v`, a DRAM model that **fails the run on any
protocol violation**: initialisation order and timings, tRCD, tRP, tRC,
tRAS, tRFC, tMRD, tMOD, refresh interval, commands to idle or active
banks.

It checks initialisation; refresh; address decode across banks, rows
and columns; bursts; word, half-word and byte stores through
read-modify-write; a block written in training mode, verified by
reading the model's memory directly; the read-buffer bypass, verified
by making the DRAM and the controller's block register DISAGREE and
seeing which answers; a block in which every beat carries a different
value; and access latency.

**Every beat distinct.** A pattern whose 16-bit halves match hides a
beat shifted by one. The doubles once had exactly such a shift on both
the read and the write path, in opposite directions, and they cancelled
for every symmetric pattern. That test is the one guaranteed to catch a
shift in either direction.

**The model derives write recovery from MR0**, in clocks, as the part
does for auto-precharge -- not from a fixed tWR in nanoseconds -- and
checks an ACTIVATE against the end of precharge without subtracting
unsigned times, so an ACTIVATE before recovery has even finished is
caught. **tRFC is checked against every command**, not only the next
refresh. Each of these was once wrong in the model, and each was shown
to fail a deliberately broken controller once fixed.

**The doubles capture on DQS**, as hardware does: the model latches
write data on each strobe edge, the PHY double read data a quarter beat
after each. Only real 0/1 transitions count -- the preamble takes the
strobe from high impedance to 0, which Verilog calls a negedge. The
model drives a read preamble and postamble, and each concurrent block
has its own loop index.

## tb_ddr3_rdasm.v -- read assembly

Bursts with every beat distinct, starting 0 to 3 beats into a cycle,
lane 1 up to two cycles after lane 0, captured at a fixed point. For
each case: exactly one offset assembles each lane, lane 1's is four
smaller per cycle of lag, and every one of the sixteen offsets selects
a different window on both lanes -- the knob reaches.

## tb_ddr3_reg.v -- register block

A TUNE write is applied seven cycles into a pause of the DQS buffers and
acknowledged only after the pause ends; other registers acknowledge at
once. A register write that never acknowledged would hang the BIOS on
its first TUNE write, with nothing on the console. The ECP5 primitives
are empty, port-accurate stand-ins (`ddr3_ecp5_stubs.v`, from yosys's
`cells_bb.v`); the DDR primitives have no simulation models, so nothing
here says anything about them.

## Not covered

The PHY's primitives -- DQSBUFM, IDDRX2DQA, ODDRX2DQA and the rest --
have no simulation models. Their behaviour was established on hardware,
by the BIOS's gate scan and training maps, and by matching LiteDRAM's
ECP5 PHY. See [docs/ddr3.md](../../docs/ddr3.md).
