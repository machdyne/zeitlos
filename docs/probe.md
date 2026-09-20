# The built-in logic probe

`rtl/probe.v` is a two-channel logic analyser inside the SoC. Software
arms it, hardware triggers it, and the capture is read back over
Wishbone and printed by the shell's `probe` command.

It exists because USB bring-up reached a point where every question
answerable from software had been answered and the device still would
not respond. What was left was the waveform, and nothing inside the
FPGA could see it.

**It is off by default.** Define `` `PROBE `` on a board to include it.

```
`define PROBE
`define PROBE_WORDS 512
`define PROBE_AW 9
```

## Cost

| | LUT4 | FF | DP16KD |
|---|---|---|---|
| mozart_ml1, `USB_HOST` | 20943 | 9482 | 56 |
| mozart_ml1, `USB_HOST` + `PROBE` | 21042 | 9525 | **57** |
| delta | **+99** | +43 | **+1** |

One block RAM and about a hundred LUT4. Cheap enough to switch on
during bring-up on any board with a spare EBR, and worth switching
back off afterwards.

## Registers

At `0x7f00_0000`, a tenant of nibble 7 decoded on bits [27:24] rather
than the [10:8] the 256-byte tenants use. `csrs_wb` is the
fall-through for that nibble, so `cs_probe` has to be excluded there
as well -- the same treatment `cs_montmul` gets.

| Offset | Access | Meaning |
|---|---|---|
| 0x0 | W | bit 0: arm. Clears the buffer and waits for the trigger |
| 0x0 | R | bit 0 armed, bit 1 running, bit 2 full, [31:16] capture depth in words |
| 0x4 | W | word index to read back |
| 0x8 | R | the word at that index |

Reading the depth from the status register is also how software knows
the block is present at all: a build without `` `PROBE `` decodes this
window to `csrs` and reads back something that is not a plausible word
count.

## Capture format

Sixteen samples per 32-bit word, two bits each, **oldest in the low
bits**. Each sample is `{sig[1], sig[0]}`. For USB that is
`{D+, D-}`, so `01` is D- high, `10` is D+ high, `00` is SE0 and `11`
is the illegal SE1.

For USB the samples are the controller's own, taken in the pads' I/O
cells (`usb_host.v`'s `line0_o`), one clock after the pins -- exactly
what the receiver sees, with D+ and D- captured through matched paths.
Captures before that change sampled the pins through fabric routing,
so a one-sample SE1 at an edge in them may be partly the probe's own
skew rather than the line's.

Depth of 512 words is 8192 samples. At 48 MHz with `DIV = 0` that is
170 us -- longer than a whole low-speed USB control transaction, which
is about 125 us. At low speed one bit is 32 samples, so bit centres
are easy to find by eye and trivial in a decoder.

## Using it elsewhere

Nothing in `probe.v` is USB-specific. It takes two wires and a
trigger; `rtl/sysctl.v` decides what they are:

```verilog
wire [1:0] probe_sig = wbs_usbh_line0;   // port 0 {D+, D-}, from usb_host
wire probe_trig = wbs_usbh_tx_active && !wbs_usbh_tx_port;
```

Change those two lines to look at something else.

**Not the USB host pins directly.** They go to input registers in the
pads' I/O cells (IDDRX1F), and nextpnr requires such a register to be
the pin's only load: `probe_sig = {usb_host_dp[0], usb_host_dm[0]}`,
which this file used to show, now fails to pack ("IDDRX1F ... D input
must be connected only to a top level input"). Use `line0_o`, as
above. The same applies to any pin read through I/O-cell logic. Candidates that
would have been useful already:

- **SPI / SD card** -- `probe_sig = {sck, mosi}` triggered on chip
  select falling, to see whether a command frame is well formed.
- **RMII** -- `{crs_dv, rxd[0]}` triggered on carrier sense, for
  "is anything arriving at all" questions.
- **I2S / audio** -- `{bck, din}` triggered on word select, to check
  frame alignment against the DAC's expectation.
- **Any internal state machine** -- the inputs are ordinary wires, so
  two bits of a state register work as well as two pads.

Two rules when repointing it:

**Synchronise anything that comes from a pad.** `usb_port.v` does this
for its own sampling; the probe is fed the already-registered version.
A raw pad into the capture shift register is a metastability source.

**Do not use a hierarchical reference for the trigger.** Reaching into
a module instance (`wbs_usbh_i.sie_tx_active`) is not supported here:
yosys implicitly declares the name as a fresh undriven wire and only
*warns*. The probe then arms and never fires, which looks exactly like
the signal never asserting. Add a real output port instead --
`usb_host.v` has `tx_active_o` for this reason.

## Reading a capture

`probe` prints 512 lines of hex. The sequence is: arm, restart
enumeration so there is traffic to trigger on, wait for `full`, then
dump.

Decoding is done off-board. For USB the steps are: unpack two bits per
sample oldest-first, map to J/K/SE0, find the first non-idle sample,
take one sample per 32 at bit centres, undo NRZI (no transition is a
1), remove a stuffed 0 after every six 1s, then read sync, PID, fields
and CRC.

That decode found, in order: that our token and CRC5 were correct;
that our DATA0 and CRC16 were correct; that the device ACKed the SETUP
4.2 bit times later; and that real devices emit a one-sample SE1 at
every transition, which a testbench driving both lines from one
register never produces.

## Limitations

- Two channels. Widening it means widening the shift register and the
  memory word; the structure does not otherwise change.
- One capture buffer, so one trigger point per run.
- No pre-trigger history. The trigger is the start of the window, not
  the middle of it.
- `DIV` is a parameter, not a register, so the sample rate is fixed at
  build time.

None of these were worth fixing for the job it was built for, and all
of them are worth reconsidering before reaching for it again.
