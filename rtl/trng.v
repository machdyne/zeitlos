/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * TRNG -- a true random number generator: a bank of ring oscillators
 * sampled well below their own frequency, von Neumann debiased, health
 * checked, and handed to software 32 bits at a time.
 *
 * -- What this is for, and what it deliberately is NOT --
 *
 * This block produces ENTROPY, not "random numbers". The distinction
 * is the whole design and it is worth being blunt about it: raw ring
 * oscillator output is biased, autocorrelated, and its quality depends
 * on how a particular place-and-route happened to lay out the loops.
 * Nothing here is safe to use directly as a key.
 *
 * The split is:
 *
 *   this block   -- harvest physical jitter, debias it, prove it is
 *                   still alive, deliver it slowly (~1.4k words/sec)
 *   sw/common/zrng.c -- hash that entropy into a ChaCha20 CSPRNG and
 *                   generate every actual random byte the system uses,
 *                   at memory speed, with periodic reseeding
 *
 * That is the same division of labour every serious system uses (it is
 * what Linux's random driver does with RDSEED), and it is why this
 * block does not need to be fast, and does not contain a hash. A
 * SHA-256 conditioner in fabric would cost more LUTs than the entire
 * rest of this file and buy nothing that zrng.c does not already do
 * for free on a CPU that is idle while it waits for the network
 * anyway. See docs/trng.md.
 *
 * -- Why ring oscillators, and the one real risk --
 *
 * A ring oscillator is an odd-length chain of inverters with its
 * output fed back to its input. It has no stable state, so it
 * oscillates at a frequency set by propagation delay -- which is
 * temperature, voltage and silicon dependent, and jitters. Sampling
 * that oscillation with an unrelated clock harvests the jitter. It
 * needs no external part, no vendor primitive and no pins, which is
 * why it is the only option that works identically on ECP5, Artix-7
 * and GateMate.
 *
 * THE RISK: a combinational loop is exactly what every synthesis tool
 * is built to eliminate. If yosys (or a vendor tool) optimises the
 * chains away, or if place-and-route resolves the loop into something
 * that does not actually oscillate, this block still runs, still acks,
 * and still returns 32-bit words -- they are simply constants or a
 * short repeating pattern. THAT IS A SILENT, TOTAL SECURITY FAILURE
 * and it is not detectable by looking at one word.
 *
 * Two things are done about it, and both matter:
 *
 *   1. The chains carry `keep` attributes in all three of the dialects
 *      the relevant tools read (yosys/nextpnr `(* keep *)`, Synplify
 *      `syn_keep`, Vivado `dont_touch`). Belt and braces on purpose --
 *      an attribute the tool silently ignores is worse than none.
 *
 *   2. The health monitor below runs continuously and latches a sticky
 *      failure bit. A dead oscillator bank fails the repetition-count
 *      test within 32 samples, which at the default divisor is under
 *      200 microseconds from reset. Software MUST check HEALTH before
 *      trusting a word (z_rng_secure(), sw/common/zrng.h) and the SSH
 *      client refuses to connect if it is clear rather than falling
 *      back to something weaker.
 *
 * The health tests are the two from NIST SP 800-90B section 4.4,
 * chosen because they are the cheap ones and they catch the failure
 * mode that actually happens here (a stuck or non-oscillating bank),
 * not because this block claims any kind of certification.
 *
 * -- Simulation --
 *
 * A zero-delay combinational loop makes an event-driven simulator spin
 * forever: iverilog will simply hang. `TRNG_SIM replaces the
 * oscillator bank with a set of free-running LFSRs of coprime lengths,
 * which is obviously NOT random but exercises every other path in this
 * file -- sampler, debiaser, packer, FIFO, health monitor and bus.
 * rtl/tb/tb_trng.v defines it. Nothing in a real build ever does.
 *
 * -- Sampling rate, and why it is deliberately slow --
 *
 * The oscillators run at some hundreds of MHz; sys_clk is 48MHz. If we
 * sampled every cycle we would mostly be measuring the oscillator's
 * phase, which is highly correlated between adjacent samples -- lots of
 * bits, very little entropy per bit. Jitter accumulates over time, so
 * the standard fix is to sample far more slowly than the oscillator
 * runs, and SAMPLE_DIV (default 256, i.e. 187.5k samples/sec) is that
 * choice. Von Neumann debiasing then discards on average three
 * quarters of what is left.
 *
 * The result is ~1.4k words/sec, which sounds terrible and is entirely
 * beside the point: seeding a CSPRNG needs 256 bits ONCE, which takes
 * about 5.5ms. Everything after that comes from zrng.c at whatever
 * speed the CPU can manage.
 *
 * Lowering SAMPLE_DIV makes this faster and worse. Do not, without
 * measuring what comes out.
 *
 * -- Von Neumann debiasing --
 *
 * Takes raw samples in pairs: 01 emits 0, 10 emits 1, 00 and 11 emit
 * nothing. This makes the output exactly unbiased for ANY fixed bias
 * in the input, which is the specific defect ring oscillators reliably
 * have (a duty cycle that is never quite 50%). It does NOT remove
 * correlation between samples -- nothing here does, and that is what
 * zrng.c's hashing is for. Correctness of the whole chain rests on the
 * conditioner, not on this stage.
 *
 * -- Where it lives in the address map --
 *
 * 0x7000_04xx, the fifth tenant of nibble 0x7 alongside rtl/csrs.v
 * (00xx), rtl/cache.v (01xx), rtl/socctl.v (02xx) and rtl/rtc.v
 * (03xx). Adding it widened sysctl.v's tenant mask from 0x300 to
 * 0x700; every existing tenant address has bit 10 clear so none of
 * them moved. Same absorption rule as before applies: when `TRNG is
 * off, csrs.v takes this window, acks it, and reads back zero, so the
 * MAGIC check fails cleanly instead of hanging the bus. See
 * sysctl.v's cs_csrs comment.
 *
 * -- Register map --
 *
 * Word-addressed, register n at byte address 0x7000_0400 + 4n, same
 * convention as rtl/rtc.v and rtl/csrs.v.
 *
 *   0  MAGIC   ro  32'h5A52_4E47 ("ZRNG"). Check before trusting
 *                  anything else -- an unmapped read does not fault on
 *                  this bus, so a known constant is the only way to
 *                  tell "present" from "whatever the bus resolved to".
 *
 *   1  DATA    ro  32 bits of debiased entropy. READING POPS THE FIFO,
 *                  so every word is delivered exactly once and reading
 *                  twice does not get you the same value twice. Reads
 *                  0 when empty, which is indistinguishable from a
 *                  legitimate all-zero word -- check STATUS.READY
 *                  first, do not infer emptiness from the value.
 *
 *   2  STATUS  ro  bit 0     READY      -- at least one word queued
 *                  bit 1     HEALTH_OK  -- clear once a test has ever
 *                                          failed, until CTRL clears it
 *                  bit 2     ENABLED    -- mirrors CTRL bit 0
 *                  bits 11:4 LEVEL      -- words currently in the FIFO
 *
 *   3  CTRL    rw  bit 0  ENABLE, set at reset. Clearing it stops the
 *                         oscillators (they are gated, so this really
 *                         does stop the current draw and the toggling,
 *                         which matters if they ever turn out to be a
 *                         noise source for something else on the die).
 *                         NOTE that a CTRL write ALWAYS loads this
 *                         bit, so a write intending only to clear the
 *                         health flag below must still set bit 0 or it
 *                         silently switches the source off. Writing
 *                         0x2 to acknowledge a failure and wondering
 *                         why no entropy ever arrives again is the
 *                         mistake this note exists to prevent; it was
 *                         made once already in rtl/tb/tb_trng.v.
 *                  bit 1  write 1 to clear the sticky health failure
 *                         and flush the FIFO. Self-clearing; reads 0.
 *                         Flushing is deliberate: words collected
 *                         while the source was unhealthy must not
 *                         survive the acknowledgement of that fact.
 *
 *   4  RATE    ro  approximate words per second, worst case. Read
 *                  rather than assumed so software sizing a wait keeps
 *                  working if SAMPLE_DIV is ever retuned.
 *
 *   5  HEALTH  ro  live test detail, for diagnosis rather than
 *                  decisions:
 *                  bits 7:0    longest run seen in the current window
 *                  bits 26:16  ones counted in the current window
 *
 *   6,7         -  reserved, read 0.
 */

module trng_wb #(
	// System clock, Hz -- rtl/sysctl.v passes its own SYSCLK. Only
	// used to compute the advertised RATE.
	parameter CLK_HZ = 48_000_000,

	// How many ring oscillators to XOR together. More is better:
	// independent jitter sources add, and a single oscillator that
	// injection-locks to something on the die stops mattering when
	// seven others do not. Eight is cheap (a few dozen LUTs total).
	parameter NUM_RO = 8,

	// Stages in the shortest oscillator. MUST BE ODD -- an even
	// length has a stable state and does not oscillate at all.
	// Each subsequent oscillator gets two more stages than the last,
	// so they run at deliberately different frequencies; identical
	// lengths tend to injection-lock into a single source, which
	// would quietly reduce NUM_RO to 1.
	parameter RO_BASE = 13,

	// Sample one bit every this many sys_clk cycles. See the header
	// comment -- this is the knob that trades rate against quality
	// and the default is already toward the conservative end.
	parameter SAMPLE_DIV = 256,

	// Output FIFO depth in 32-bit words. MUST BE A POWER OF TWO.
	// Small on purpose: this is a trickle source feeding a CSPRNG
	// seed, not a buffer anything streams from.
	parameter FIFO_DEPTH = 8
)
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output reg wb_ack_o,
	input wb_cyc_i
);

	localparam MAGIC = 32'h5A52_4E47;	// "ZRNG"

	// Stages allocated per oscillator. The bank is declared as one
	// flat vector of NUM_RO * RO_MAX bits and each oscillator closes
	// its loop after its own length, leaving the unused tail tied off
	// -- Verilog-2001 has no ragged arrays and this is the least
	// clever way to get variable-length chains out of one generate.
	localparam RO_MAX = RO_BASE + 2 * (NUM_RO - 1);

	// Von Neumann keeps at most one bit per four raw samples (it
	// discards 00 and 11, and consumes two samples per candidate
	// bit), so this is the worst case rather than an estimate.
	localparam RATE_WORDS = CLK_HZ / SAMPLE_DIV / 4 / 32;

	// NIST SP 800-90B 4.4.1, repetition count. Cutoff 32 for a
	// binary source is far looser than the standard's own formula
	// demands, deliberately: this test is here to catch a DEAD
	// source, and a false positive on a live one would be a
	// self-inflicted denial of service on every consumer of
	// randomness in the system.
	localparam REP_CUTOFF = 32;

	// NIST SP 800-90B 4.4.2, adaptive proportion, over a 1024-sample
	// window. A healthy source lands at 512 ones; these bounds are
	// +/- 20%, again loose on purpose (see above). A stuck source
	// hits 0 or 1024 and fails immediately.
	localparam WIN_LEN = 1024;
	localparam WIN_LO  = 410;
	localparam WIN_HI  = 614;

	// -- the entropy source --

	// `keep` in three dialects. Redundant by design: an attribute a
	// given tool ignores is silent, and what it would be silent about
	// here is the optimiser deleting the entire entropy source while
	// leaving a block that still returns plausible-looking words. See
	// this file's header comment.
	/* verilator lint_off UNOPTFLAT */
	(* keep = "true" *)
	(* syn_keep = "true" *)
	(* dont_touch = "true" *)
	wire [NUM_RO*RO_MAX-1:0] ro_net /* synthesis syn_keep=1 keep=1 */;
	/* verilator lint_on UNOPTFLAT */

	wire [NUM_RO-1:0] ro_tap;
	wire ro_xor;

	reg enable;
	reg [2:0] sync;
	reg [31:0] div_ctr;
	reg raw_bit;
	reg raw_valid;

	// von Neumann pair state
	reg vn_have_first;
	reg vn_first;
	reg out_bit;
	reg out_valid;

	// 32-bit packer
	reg [31:0] acc;
	reg [5:0] acc_cnt;

	// health monitor
	reg health_ok;
	reg last_raw;
	reg [7:0] rep_run;
	reg [7:0] rep_max;
	reg [10:0] win_ctr;
	reg [10:0] ones_ctr;
	reg [10:0] ones_shown;
	reg [7:0] rep_shown;

	// output FIFO
	reg [31:0] fifo [0:FIFO_DEPTH-1];
	reg [3:0] fifo_head;
	reg [3:0] fifo_tail;
	reg [4:0] fifo_level;

	wire fifo_full  = (fifo_level == FIFO_DEPTH[4:0]);
	wire fifo_empty = (fifo_level == 5'd0);
	wire bus_cycle  = wb_cyc_i && wb_stb_i && !wb_ack_o;
	wire pop_now    = bus_cycle && !wb_we_i && (wb_adr_i[2:0] == 3'd1) && !fifo_empty;
	wire push_now   = out_valid && (acc_cnt == 6'd31) && health_ok && !fifo_full;

	genvar gi, gj;

`ifdef TRNG_SIM
	// -- simulation stand-in --
	//
	// Free-running LFSRs of coprime lengths, one per "oscillator".
	// This is NOT an entropy source and must never be synthesised;
	// it exists so that a simulator can exercise the sampler,
	// debiaser, packer, FIFO, health monitor and bus interface
	// without a zero-delay combinational loop spinning forever. See
	// rtl/tb/tb_trng.v.
	reg [30:0] sim_lfsr [0:NUM_RO-1];
	integer si;
	always @(posedge wb_clk_i) begin
		if (wb_rst_i) begin
			for (si = 0; si < NUM_RO; si = si + 1)
				sim_lfsr[si] <= 31'd1 + si[30:0] * 31'd7919;
		end else if (enable) begin
			for (si = 0; si < NUM_RO; si = si + 1)
				sim_lfsr[si] <= { sim_lfsr[si][29:0],
					sim_lfsr[si][30] ^ sim_lfsr[si][27] };
		end
	end
	generate
		for (gi = 0; gi < NUM_RO; gi = gi + 1) begin : ro_bank_sim
			assign ro_tap[gi] = sim_lfsr[gi][30];
		end
	endgenerate
	assign ro_net = {NUM_RO*RO_MAX{1'b0}};
`else
	// -- the real oscillator bank --
	//
	// Oscillator gi has RO_BASE + 2*gi stages; stage 0 is gated by
	// `enable` so the whole bank can be stopped. Stages past that
	// oscillator's own length are tied low and optimise away (they are
	// outside the loop, so removing them is correct).
	//
	// -- WHY THESE ARE VENDOR PRIMITIVES AND NOT `assign x = ~y` --
	//
	// Because the portable version does not survive synthesis, and it
	// fails in the worst possible way. Written as plain inverters, an
	// optimiser sees that thirteen inversions is algebraically one
	// inversion, folds the entire chain, and emits a SINGLE LUT whose
	// output feeds its own input:
	//
	//     ro_net[0] = enable & ~ro_net[12]
	//               = enable & ~(~ro_net[0])        // 13 inversions
	//               = enable &   ro_net[0]
	//
	// which is not an oscillator at all -- it is a latch that settles
	// to zero. Confirmed on a real ECP5 build: yosys reported eight
	// loops (so the `keep` attributes looked like they had worked) and
	// nextpnr then reported eight loops of ONE CELL EACH, on ro_net[0],
	// [27], [54] ... -- exactly the chain heads. `keep` on the wire
	// vector kept the NET NAMES; it did not stop abc from restructuring
	// the logic driving them.
	//
	// An instantiated LUT primitive has no algebra for the optimiser to
	// do. It is an opaque cell with a fixed INIT, and the tool places
	// it and routes it and that is all. This is the only construction
	// that reliably survives, which is why every serious FPGA ring
	// oscillator is written this way.
	//
	// The cost is that this is now per-vendor code. That is a real loss
	// of portability and it is worth it: a portable entropy source that
	// silently produces zeros is worth strictly less than a
	// vendor-specific one that works. The generic fallback below is
	// kept for a toolchain not covered here, but it is NOT SAFE without
	// checking what came out -- see tools/check_trng.py.
	//
	// INIT values, for anyone adding a vendor:
	//   inverter  Z = ~A               -> set every index with A=0
	//   head      Z = A & ~B           -> set every index with A=1,B=0
	//             (A = enable, B = the feedback tap)
	generate
		for (gi = 0; gi < NUM_RO; gi = gi + 1) begin : ro_bank
			for (gj = 0; gj < RO_MAX; gj = gj + 1) begin : ro_stage

				if (gj == 0) begin : ro_head
`ifdef TRNG_RO_GENERIC
					assign ro_net[gi*RO_MAX] =
						enable & ~ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1];
`elsif ECP5
					(* keep *) LUT4 #(.INIT(16'h2222)) ro_head_lut (
						.A(enable),
						.B(ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1]),
						.C(1'b0), .D(1'b0),
						.Z(ro_net[gi*RO_MAX]));
`elsif ICE40
					(* keep *) SB_LUT4 #(.LUT_INIT(16'h2222)) ro_head_lut (
						.I0(enable),
						.I1(ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1]),
						.I2(1'b0), .I3(1'b0),
						.O(ro_net[gi*RO_MAX]));
`elsif GATEMATE
					(* keep *) CC_LUT2 #(.INIT(4'b0010)) ro_head_lut (
						.I0(enable),
						.I1(ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1]),
						.O(ro_net[gi*RO_MAX]));
`else
					assign ro_net[gi*RO_MAX] =
						enable & ~ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1];
`endif
				end else if (gj < RO_BASE + 2*gi) begin : ro_body
`ifdef TRNG_RO_GENERIC
					assign ro_net[gi*RO_MAX + gj] = ~ro_net[gi*RO_MAX + gj - 1];
`elsif ECP5
					(* keep *) LUT4 #(.INIT(16'h5555)) ro_inv_lut (
						.A(ro_net[gi*RO_MAX + gj - 1]),
						.B(1'b0), .C(1'b0), .D(1'b0),
						.Z(ro_net[gi*RO_MAX + gj]));
`elsif ICE40
					(* keep *) SB_LUT4 #(.LUT_INIT(16'h5555)) ro_inv_lut (
						.I0(ro_net[gi*RO_MAX + gj - 1]),
						.I1(1'b0), .I2(1'b0), .I3(1'b0),
						.O(ro_net[gi*RO_MAX + gj]));
`elsif GATEMATE
					(* keep *) CC_LUT1 #(.INIT(2'b01)) ro_inv_lut (
						.I0(ro_net[gi*RO_MAX + gj - 1]),
						.O(ro_net[gi*RO_MAX + gj]));
`else
					assign ro_net[gi*RO_MAX + gj] = ~ro_net[gi*RO_MAX + gj - 1];
`endif
				end else begin : ro_unused
					assign ro_net[gi*RO_MAX + gj] = 1'b0;
				end

			end
			assign ro_tap[gi] = ro_net[gi*RO_MAX + RO_BASE + 2*gi - 1];
		end
	endgenerate
`endif

	assign ro_xor = ^ro_tap;

	// Everything below lives in ONE always block on purpose. The FIFO
	// is written by the packer and read by the bus in the same cycle
	// often enough that splitting them would mean two drivers on
	// fifo_level and fifo_head/tail, which is exactly the class of bug
	// that is invisible in simulation on some tools and fatal in
	// synthesis on all of them.
	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

			enable <= 1'b1;
			sync <= 3'd0;
			div_ctr <= 32'd0;
			raw_bit <= 1'b0;
			raw_valid <= 1'b0;

			vn_have_first <= 1'b0;
			vn_first <= 1'b0;
			out_bit <= 1'b0;
			out_valid <= 1'b0;

			acc <= 32'd0;
			acc_cnt <= 6'd0;

			health_ok <= 1'b1;
			last_raw <= 1'b0;
			rep_run <= 8'd0;
			rep_max <= 8'd0;
			win_ctr <= 11'd0;
			ones_ctr <= 11'd0;
			ones_shown <= 11'd0;
			rep_shown <= 8'd0;

			fifo_head <= 4'd0;
			fifo_tail <= 4'd0;
			fifo_level <= 5'd0;

			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;

		end else begin

			// -- synchroniser --
			//
			// Three stages, not the usual two. The whole point of
			// this input is that it is sampled while changing, so
			// metastability here is not a rare event to be made
			// improbable, it is the mechanism -- and the extra stage
			// costs one flop and keeps a half-resolved level from
			// reaching the arithmetic below.
			sync <= { sync[1:0], ro_xor };

			// -- downsampler --
			raw_valid <= 1'b0;
			if (enable) begin
				if (div_ctr == (SAMPLE_DIV - 1)) begin
					div_ctr <= 32'd0;
					raw_bit <= sync[2];
					raw_valid <= 1'b1;
				end else begin
					div_ctr <= div_ctr + 1;
				end
			end

			// -- health monitor, on RAW samples --
			//
			// Deliberately before the debiaser: von Neumann output is
			// unbiased by construction, so testing it would report a
			// healthy source no matter how broken the input was. The
			// tests only mean anything upstream of it.
			if (raw_valid) begin

				if (raw_bit == last_raw) begin
					rep_run <= rep_run + 1;
					if ((rep_run + 1) >= REP_CUTOFF[7:0]) health_ok <= 1'b0;
					if ((rep_run + 1) > rep_max) rep_max <= rep_run + 1;
				end else begin
					rep_run <= 8'd1;
				end
				last_raw <= raw_bit;

				if (raw_bit) ones_ctr <= ones_ctr + 1;

				if (win_ctr == (WIN_LEN - 1)) begin
					// End of window: judge it, publish it for the
					// HEALTH register, and start the next one.
					if ((ones_ctr + (raw_bit ? 11'd1 : 11'd0)) < WIN_LO[10:0] ||
						(ones_ctr + (raw_bit ? 11'd1 : 11'd0)) > WIN_HI[10:0])
						health_ok <= 1'b0;
					ones_shown <= ones_ctr + (raw_bit ? 11'd1 : 11'd0);
					rep_shown <= rep_max;
					ones_ctr <= 11'd0;
					rep_max <= 8'd0;
					win_ctr <= 11'd0;
				end else begin
					win_ctr <= win_ctr + 1;
				end

			end

			// -- von Neumann debiaser --
			out_valid <= 1'b0;
			if (raw_valid) begin
				if (!vn_have_first) begin
					vn_first <= raw_bit;
					vn_have_first <= 1'b1;
				end else begin
					vn_have_first <= 1'b0;
					// 01 -> 0, 10 -> 1, 00/11 -> nothing
					if (vn_first != raw_bit) begin
						out_bit <= vn_first;
						out_valid <= 1'b1;
					end
				end
			end

			// -- 32-bit packer --
			if (out_valid) begin
				acc <= { acc[30:0], out_bit };
				if (acc_cnt == 6'd31) acc_cnt <= 6'd0;
				else acc_cnt <= acc_cnt + 1;
			end

			// -- FIFO --
			//
			// A push and a pop in the same cycle cancel out in
			// fifo_level, which is why the two are resolved together
			// rather than as independent increments.
			if (push_now && !pop_now) begin
				fifo[fifo_tail[2:0]] <= { acc[30:0], out_bit };
				fifo_tail <= fifo_tail + 1;
				fifo_level <= fifo_level + 1;
			end else if (pop_now && !push_now) begin
				fifo_head <= fifo_head + 1;
				fifo_level <= fifo_level - 1;
			end else if (push_now && pop_now) begin
				fifo[fifo_tail[2:0]] <= { acc[30:0], out_bit };
				fifo_tail <= fifo_tail + 1;
				fifo_head <= fifo_head + 1;
			end

			// -- bus --

			wb_ack_o <= 1'b0;

			if (bus_cycle) begin

				wb_ack_o <= 1'b1;

				if (wb_we_i) begin

					case (wb_adr_i[2:0])

						3'd3: begin
							if (wb_sel_i[0]) begin
								enable <= wb_dat_i[0];
								// Acknowledging a health failure also
								// discards everything collected while
								// the source was suspect -- see the
								// register map comment.
								if (wb_dat_i[1]) begin
									health_ok <= 1'b1;
									rep_run <= 8'd0;
									rep_max <= 8'd0;
									win_ctr <= 11'd0;
									ones_ctr <= 11'd0;
									acc_cnt <= 6'd0;
									fifo_head <= 4'd0;
									fifo_tail <= 4'd0;
									fifo_level <= 5'd0;
								end
							end
						end

						default: begin
							// Reserved or read-only -- silently
							// ignored, matching this bus's usual
							// behaviour for an unhandled access.
						end

					endcase

				end else begin

					case (wb_adr_i[2:0])
						3'd0: wb_dat_o <= MAGIC;
						3'd1: wb_dat_o <= fifo_empty ? 32'd0 : fifo[fifo_head[2:0]];
						3'd2: wb_dat_o <= { 20'd0, fifo_level[3:0],
							1'b0, enable, health_ok, ~fifo_empty };
						3'd3: wb_dat_o <= { 31'd0, enable };
						3'd4: wb_dat_o <= RATE_WORDS;
						3'd5: wb_dat_o <= { 5'd0, ones_shown, 8'd0, rep_shown };
						default: wb_dat_o <= 32'd0;
					endcase

				end

			end

		end

	end

endmodule
