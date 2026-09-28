/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Montgomery modular multiplier -- R = A * B * 2^-(32*n) mod N
 *
 * -- What this is for --
 *
 * TLS certificate verification. Measured on Lakritz, one ECDSA P-384
 * signature takes 12.1 seconds, and a real chain needs three of them
 * -- 36 of the 85 seconds a page load costs. That is ~9,650 field
 * multiplies at ~60,000 cycles each.
 *
 * Not because multiplying is hard: picorv32 has a DSP multiplier and
 * uses ~2 cycles for one. It is because the software inner loop
 *
 *     pr = (uint64_t)a[i] * b[j] + t[j] + carry;
 *
 * is two loads and a store, and this CPU has NO DATA CACHE. Every one
 * of those is an SDRAM round trip of ~13 cycles, so the multiplier
 * sits idle while the memory system does all the work.
 *
 * This block holds the operands in its own registers and does 2n^2
 * multiply-accumulate steps without touching main memory at all:
 * about 340 cycles for 384 bits, against ~60,000. Software still
 * pays to load the operands in and read the result out -- ~150 cycles
 * of MMIO -- so the honest figure is roughly 120x on the multiply and
 * 40-60x on a whole verification.
 *
 * -- What it does NOT do --
 *
 * It does not replace the CPU's own multiplier. `CPU_MUL_FAST stays
 * exactly as it is; this is a separate peripheral that software calls
 * for one specific operation, and every other multiply in the system
 * is unaffected.
 *
 * It is not curve-specific, and barely even ECC-specific: it is a
 * Montgomery multiplier for any odd modulus up to LIMBS words. The
 * same block serves P-256, P-384, and the scalar fields of both,
 * which have no fast-reduction form of their own.
 *
 * -- Size --
 *
 * Four register files of LIMBS words (A, B, N) and LIMBS+2 (T), one
 * 32x32 multiplier, one 64-bit adder and a small FSM. NO BRAM: the
 * register files are 12-16 words deep and map to distributed LUT RAM,
 * which is what makes this affordable. At LIMBS=12 that is ~50 words
 * of storage.
 *
 * Set LIMBS to 8 for a P-256-only build at two thirds the storage;
 * the block reports its own limit in CONFIG so software can check
 * rather than assume.
 *
 * -- Measured --
 *
 * Synthesis (yosys, ECP5) and simulation (rtl/tests):
 *
 *   983 LUT4, 524 FF, 4 MULT18X18D, 32 TRELLIS_DPR16X4
 *   517 cycles per multiply at LIMBS=12
 *
 * The first version was 5,874 LUT4 -- six times the estimate, and
 * 95% device utilisation once the rest of the SOC was in. The whole
 * difference was HOW THE ARRAYS WERE WRITTEN, not what the block
 * computes: indexing them directly from half a dozen places, and
 * writing several words in one cycle, gives 50 words of flops behind
 * a pile of multiplexers and decoders rather than a RAM.
 *
 * Funnelling every access through one read and one write port per
 * array, and making the limb count a synthesis parameter instead of a
 * runtime register, took it to 983 LUT4 with the arrays inferred as
 * distributed RAM. That is why the FSM has more states than the
 * algorithm needs: S_E1/S_E2 and S_F1..S_F3 exist purely so that no
 * cycle writes two words.
 *
 * On hardware, ECDSA P-384 verification went from 12.1s to 2.7s and
 * a TLS handshake from 46s to 13s.
 *
 * -- Timing --
 *
 * The multiply is REGISTERED IN AND OUT and the accumulate happens
 * the following cycle, so the longest combinational path is one
 * 64-bit add -- not a multiply feeding an add. That is the whole
 * reason for the two-stage inner loop below, which otherwise just
 * makes the FSM longer. At 48MHz there is ample margin.
 *
 * -- Verified --
 *
 * rtl/tests/tb_montmul.v checks 51 products against Python, over
 * P-384, P-256 and P-384's scalar field, corner cases included. That
 * suite caught three bugs that inspection did not: a two-cycle
 * multiplier latency treated as one, operand arrays read one word
 * past their end, and a stale product consumed for m. Any of the
 * three would have produced a block that synthesised, ran, and
 * returned wrong answers.
 *
 * -- Register map (word-addressed, base 0x7000_0600) --
 *
 *    0  MAGIC    read: 32'h5A4D_4F4E ("ZMON"). An unmapped read does
 *                not fault on this bus, so a known constant is the
 *                only way software can tell this block is present --
 *                same reasoning as rtl/csrs.v and rtl/gpio.v.
 *    1  CTRL     write bit 0: START. read bit 0: BUSY.
 *    2  CONFIG   read: { 16'h4D4F, 8'b0, 8'd LIMBS } -- the widest
 *                modulus this bitstream was built for, in words.
 *    3  N0INV    write: -N^-1 mod 2^32. Computed by software once.
 *   16..27  A    write: multiplicand, little-endian words
 *   28..39  B    write: multiplier
 *   40..51  N    write: modulus (odd)
 *   52..63  R    read:  result
 *
 * N and N0INV are written ONCE per verification, not per multiply --
 * that is most of why the transfer cost is bearable.
 *
 * -- The register file (REGFILE=1) --
 *
 * Measured on Lakritz, a multiply driven as above costs ~3,450 cycles,
 * of which the engine's own work is ~700: the rest is moving 32 words
 * in and out, and the software doing it (docs/cryptobench.md). So the
 * block also keeps NREGS registers of LIMBS words each in one block
 * RAM, and runs commands on them: software loads its operands once,
 * issues a stream of one-word commands, and reads back only the
 * answer. A MUL copies its two registers into A and B, runs exactly
 * the engine above, and copies T back -- the multiply itself is the
 * same logic as ever. ADD and SUB are modulo N and take ~3 x LIMBS
 * cycles. Operands must be below N, as for the engine.
 *
 *    2  CONFIG   read: bits 15:8 are NREGS (0: no register file).
 *                Bits 7:0 are LIMBS as before, and software that
 *                masks them off is unaffected.
 *    4  OWNER    advisory claim, for sharing the block between
 *                processes (docs/montmul.md, "Sharing"). Read: the
 *                owner, 0 if none. Write a pid (1..2^31-1): claims the
 *                block if it has no owner. Write pid | 2^31: releases
 *                it if that pid owns it. Nothing is enforced -- the
 *                block computes for anyone -- so a caller claims, then
 *                reads OWNER back to see whether it won.
 *    5  RSEL     write: { reg (4 bits), word (4 bits) }, where the
 *                window below starts.
 *    6  RDATA    write: the word at RSEL, then RSEL + 1. Read: the
 *                same, auto-incrementing too. LIMBS writes load a
 *                register; LIMBS reads fetch one.
 *    7  CMD      write: { op [15:12], rd [11:8], ra [7:4], rb [3:0] }
 *                and the command runs; poll BUSY (CTRL or CMD bit 0).
 *                op 1 MUL: rd = ra * rb * R^-1 mod N
 *                op 2 ADD: rd = ra + rb mod N
 *                op 3 SUB: rd = ra - rb mod N
 *                Every command takes the same number of cycles whatever
 *                its operands: the register file runs X25519 on secret
 *                scalars (sw/common/z25519.c).
 *                rd may be ra or rb. A, B and R are overwritten, so
 *                the classic interface above and the commands must not
 *                be mixed within one computation -- which the claim
 *                is for.
 */

module montmul #(
	parameter LIMBS = 12,				// 12 -> 384 bits, 8 -> 256 bits
	parameter REGFILE = 1				// the register file and its commands
) (
	input clk,
	input resetn,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i
);

	localparam TW = LIMBS + 2;			// T needs two words of headroom
	localparam AW = 4;					// address width for the arrays

	// -- storage -----------------------------------------------------
	//
	// ONE write port and ONE asynchronous read port each, driven
	// through explicit address/data/enable wires. That shape is what
	// yosys infers as ECP5 distributed RAM (TRELLIS_DPR16X4).
	//
	// The first version indexed these arrays directly from half a
	// dozen places -- t[mul_j], t[mul_j-1], t[nlimbs], t[nlimbs+1],
	// t[j] -- and wrote several of them in the same cycle. That is
	// not a RAM, it is 50 words of flops behind a pile of
	// multiplexers and decoders, and it synthesised to 5,874 LUT4:
	// six times the estimate and 95% of the device once the rest of
	// the SOC was in.
	//
	// Everything below therefore funnels through these ports, one
	// read and one write per array per cycle, which is why the FSM
	// has more states than the algorithm strictly needs.

	// The engine's state, declared ahead of everything that reads it.
	// (It used to be declared further down, after its first use in
	// t_ra below. Older simulators let that through; iverilog 14 and
	// the Verilog standard do not.)
	reg [4:0] st;
	localparam S_IDLE = 5'd0;
	wire busy = (st != S_IDLE);

	reg [31:0] ra [0:(1<<AW)-1];
	reg [31:0] rb [0:(1<<AW)-1];
	reg [31:0] rn [0:(1<<AW)-1];
	reg [31:0] t  [0:(1<<AW)-1];

	reg [AW-1:0] a_ra, b_ra, n_ra, t_ra_eng;
	reg [AW-1:0] a_wa, b_wa, n_wa, t_wa;
	reg [31:0]   a_wd, b_wd, n_wd, t_wd;
	reg          a_we, b_we, n_we, t_we;

	wire [31:0] a_rd = ra[a_ra];
	wire [31:0] b_rd = rb[b_ra];
	wire [31:0] n_rd = rn[n_ra];
	// The result read address is COMBINATIONAL on the bus address
	// while the engine is idle.
	//
	// Registering it meant dat_r captured t_rd in the same cycle
	// t_ra was being set, so every result read returned the previous
	// transaction's word -- and since the engine leaves the pointer
	// on a zeroed headroom word, every result read back as zero.
	wire [AW-1:0] t_ra = busy ? t_ra_eng : (wb_adr_i[3:0] - 4'd4);
	wire [31:0] t_rd = t[t_ra];

	always @(posedge clk) begin
		if (a_we) ra[a_wa] <= a_wd;
		if (b_we) rb[b_wa] <= b_wd;
		if (n_we) rn[n_wa] <= n_wd;
		if (t_we) t[t_wa]  <= t_wd;
	end

	reg [31:0] n0inv;

	// -- the register file --------------------------------------------
	//
	// NREGS x 16 words, a register's words at { reg, word }: 256 x 32,
	// one block RAM. One write port and one SYNCHRONOUS read port --
	// the shape yosys maps to an ECP5 DP16KD (or a GateMate RAM) --
	// so a word is read the cycle after its address is presented, and
	// the copying states below are pipelined by one.
	localparam NREGS = REGFILE ? 16 : 0;

	reg [31:0] rf [0:255];
	reg [7:0]  rf_ra_eng, rf_wa, rsel;
	reg [31:0] rf_wd, rf_q;
	reg        rf_we;
	wire [7:0] rf_ra = busy ? rf_ra_eng : rsel;

	always @(posedge clk) begin
		if (REGFILE && rf_we) rf[rf_wa] <= rf_wd;
		rf_q <= rf[rf_ra];
	end

	reg [30:0] owner;
	reg        cmd_on;			// a command is running: finish into S_WB
	reg [3:0]  cmd_op, cmd_rd, cmd_ra, cmd_rb;

	// -- bus ---------------------------------------------------------

	reg [31:0] dat_r;
	reg ack_r;

	assign wb_dat_o = dat_r;
	assign wb_ack_o = ack_r;

	wire sel = wb_cyc_i && wb_stb_i && !ack_r;
	wire wr  = sel && wb_we_i;

	wire [5:0] w = wb_adr_i[5:0];

	wire is_a = (w >= 6'd16) && (w < 6'd28);
	wire is_b = (w >= 6'd28) && (w < 6'd40);
	wire is_n = (w >= 6'd40) && (w < 6'd52);
	wire is_r = (w >= 6'd52);

	// -- engine ------------------------------------------------------

	// Five bits: the conditional subtract needs two states past the
	// fifteen the multiply itself uses.
	localparam S_CLR  = 5'd1;
	localparam S_LOAD = 5'd2;
	localparam S_L1   = 5'd3;
	localparam S_E1   = 5'd4;
	localparam S_E2   = 5'd5;
	localparam S_M0   = 5'd6;
	localparam S_M1   = 5'd7;
	localparam S_M2   = 5'd8;
	localparam S_L2   = 5'd9;
	localparam S_F1   = 5'd10;
	localparam S_F2   = 5'd11;
	localparam S_F3   = 5'd12;
	localparam S_NEXT = 5'd13;
	localparam S_SUB  = 5'd14;
	localparam S_CPY  = 5'd15;
	localparam S_SUB0 = 5'd16;
	// the register file's commands
	localparam S_XA   = 5'd17;		// register ra -> A
	localparam S_XB   = 5'd18;		// register rb -> B
	localparam S_ADD  = 5'd19;		// T = A + B
	localparam S_ADDC = 5'd20;		// T[LIMBS] = the carry
	localparam S_ADDW = 5'd21;		// let that write land, then S_SUB0
	localparam S_SBAB = 5'd22;		// T = A - B
	localparam S_SBN0 = 5'd23;		// borrowed? then T += N
	localparam S_ADDN = 5'd24;
	localparam S_WB0  = 5'd25;		// T -> register rd
	localparam S_WB   = 5'd26;

	reg [4:0] i, j;
	reg [31:0] bi, m, carry;
	reg [31:0] hold;

	reg [31:0] mul_x, mul_y;
	reg [63:0] mul_p;
	reg mul_v1, mul_v;
	reg [4:0] mul_j1, mul_j;

	// Two cycles of latency: registered operands AND a registered
	// product. Both stages are deliberate -- they keep a 32x32
	// multiply out of the combinational path, so the longest path in
	// this block is one 64-bit add. The valid/index flags are delayed
	// by exactly the same amount.
	always @(posedge clk) begin
		mul_p <= mul_x * mul_y;
		mul_v1 <= (st == S_L1 || st == S_L2) && (j < LIMBS);
		mul_j1 <= j;
		mul_v <= mul_v1;
		mul_j <= mul_j1;
	end

	reg [63:0] acc;
	reg [32:0] acc2;

	// ONE 33-bit adder/subtractor for every word-serial add and
	// subtract: the final T - N (S_SUB), and the register file's A + B,
	// A - B and T + N. Four separate ones synthesised to ~120 more LUTs
	// than this and its two operand multiplexers. `carry` is the carry
	// for an add and the borrow for a subtract, as before.
	wire as_ab  = (st == S_ADD) || (st == S_SBAB);
	wire as_sub = (st == S_SUB) || (st == S_SBAB);
	wire [31:0] as_x = as_ab ? a_rd : t_rd;
	// SUB's add-back adds N after a borrow and ZERO otherwise, but
	// always runs: a command's time must not depend on its operands,
	// or X25519 on secret scalars would leak them through its timing.
	reg sb_borrow;
	wire [31:0] as_y = as_ab ? b_rd : ((st == S_ADDN && !sb_borrow) ? 32'd0 : n_rd);
	wire [32:0] as_sum = { 1'b0, as_x } + { 1'b0, as_sub ? ~as_y : as_y }
		+ { 32'd0, as_sub ? ~carry[0] : carry[0] };
	wire [31:0] as_res = as_sum[31:0];
	wire        as_out = as_sub ? ~as_sum[32] : as_sum[32];

	always @(posedge clk) begin

		ack_r <= 1'b0;
		a_we <= 1'b0;
		b_we <= 1'b0;
		n_we <= 1'b0;
		t_we <= 1'b0;
		rf_we <= 1'b0;

		if (!resetn) begin

			st <= S_IDLE;
			ack_r <= 1'b0;
			n0inv <= 32'd0;
			owner <= 31'd0;
			cmd_on <= 1'b0;
			rsel <= 8'd0;

		end else begin

			// -- bus --
			//
			// Array writes go through the same single write port the
			// engine uses; the engine is idle whenever software is
			// loading operands, so they never contend.

			if (sel) begin

				ack_r <= 1'b1;
				dat_r <= 32'd0;

				if (wr) begin
					if (w == 6'd1) begin
						if (wb_dat_i[0] && !busy) begin
							st <= S_CLR;
							j <= 5'd0;
							cmd_on <= 1'b0;
						end
					end else if (w == 6'd3) begin
						n0inv <= wb_dat_i;
					end else if (w == 6'd4) begin
						// OWNER: claim if free; release if yours
						if (wb_dat_i[31]) begin
							if (owner == wb_dat_i[30:0]) owner <= 31'd0;
						end else if (owner == 31'd0) begin
							owner <= wb_dat_i[30:0];
						end
					end else if (REGFILE && w == 6'd5) begin
						rsel <= wb_dat_i[7:0];
					end else if (REGFILE && w == 6'd6) begin
						if (!busy) begin
							rf_wa <= rsel; rf_wd <= wb_dat_i; rf_we <= 1'b1;
							rsel <= rsel + 8'd1;
						end
					end else if (REGFILE && w == 6'd7) begin
						if (!busy && (wb_dat_i[15:12] >= 4'd1) && (wb_dat_i[15:12] <= 4'd3)) begin
							cmd_op <= wb_dat_i[15:12];
							cmd_rd <= wb_dat_i[11:8];
							cmd_ra <= wb_dat_i[7:4];
							cmd_rb <= wb_dat_i[3:0];
							cmd_on <= 1'b1;
							rf_ra_eng <= { wb_dat_i[7:4], 4'd0 };
							j <= 5'd0;
							st <= S_XA;
						end
					end else if (is_a) begin
						a_wa <= w[3:0]; a_wd <= wb_dat_i; a_we <= 1'b1;
					end else if (is_b) begin
						b_wa <= w - 6'd28; b_wd <= wb_dat_i; b_we <= 1'b1;
					end else if (is_n) begin
						n_wa <= w - 6'd40; n_wd <= wb_dat_i; n_we <= 1'b1;
					end
				end else begin
					if (w == 6'd0) dat_r <= 32'h5A4D_4F4E;
					else if (w == 6'd1) dat_r <= { 31'd0, busy };
					else if (w == 6'd2) dat_r <= { 16'h4D4F, NREGS[7:0], LIMBS[7:0] };
					else if (w == 6'd4) dat_r <= { 1'b0, owner };
					else if (REGFILE && w == 6'd5) dat_r <= { 24'd0, rsel };
					else if (REGFILE && w == 6'd6) begin
						// prefetched: rf_ra has been RSEL since the
						// last access, so rf_q holds its word
						dat_r <= rf_q;
						if (!busy) rsel <= rsel + 8'd1;
					end
					else if (REGFILE && w == 6'd7) dat_r <= { 31'd0, busy };
					else if (is_r) dat_r <= t_rd;
				end

			end

			// -- CIOS --

			case (st)

			S_CLR: begin
				// One word per cycle, because there is one write port.
				t_wa <= j[AW-1:0]; t_wd <= 32'd0; t_we <= 1'b1;
				if (j == TW - 1) begin
					b_ra <= 4'd0;
					i <= 5'd0;
					st <= S_LOAD;
				end else j <= j + 5'd1;
			end

			S_LOAD: begin
				bi <= b_rd;
				j <= 5'd0;
				carry <= 32'd0;
				a_ra <= 4'd0;
				t_ra_eng <= 4'd0;
				st <= S_L1;
			end

			S_L1: begin
				a_ra <= (j + 5'd1);				// next operand
				t_ra_eng <= mul_j1[AW-1:0];			// word the next product lands in
				mul_x <= (j < LIMBS) ? a_rd : 32'd0;
				mul_y <= bi;

				if (mul_v) begin
					acc = mul_p + { 32'd0, t_rd } + { 32'd0, carry };
					t_wa <= mul_j[AW-1:0]; t_wd <= acc[31:0]; t_we <= 1'b1;
					carry <= acc[63:32];
				end

				if (j == LIMBS + 2) begin
					t_ra_eng <= LIMBS[AW-1:0];
					st <= S_E1;
				end else j <= j + 5'd1;
			end

			S_E1: begin
				acc2 = { 1'b0, t_rd } + { 1'b0, carry };
				t_wa <= LIMBS[AW-1:0]; t_wd <= acc2[31:0]; t_we <= 1'b1;
				hold <= { 31'd0, acc2[32] };
				st <= S_E2;
			end

			S_E2: begin
				t_wa <= LIMBS[AW-1:0] + 4'd1; t_wd <= hold; t_we <= 1'b1;
				t_ra_eng <= 4'd0;
				st <= S_M0;
			end

			S_M0: begin
				mul_x <= t_rd;
				mul_y <= n0inv;
				st <= S_M1;
			end

			S_M1: st <= S_M2;

			S_M2: begin
				m <= mul_p[31:0];
				j <= 5'd0;
				carry <= 32'd0;
				n_ra <= 4'd0;
				t_ra_eng <= 4'd0;
				st <= S_L2;
			end

			S_L2: begin
				n_ra <= (j + 5'd1);
				t_ra_eng <= mul_j1[AW-1:0];
				mul_x <= (j < LIMBS) ? n_rd : 32'd0;
				mul_y <= m;

				if (mul_v) begin
					acc = mul_p + { 32'd0, t_rd } + { 32'd0, carry };
					// j = 0 cancels and is discarded; the rest shift
					// down one word, which is the Montgomery divide.
					if (mul_j != 0) begin
						t_wa <= mul_j[AW-1:0] - 4'd1;
						t_wd <= acc[31:0];
						t_we <= 1'b1;
					end
					carry <= acc[63:32];
				end

				if (j == LIMBS + 2) begin
					t_ra_eng <= LIMBS[AW-1:0];
					st <= S_F1;
				end else j <= j + 5'd1;
			end

			S_F1: begin
				acc2 = { 1'b0, t_rd } + { 1'b0, carry };
				t_wa <= LIMBS[AW-1:0] - 4'd1; t_wd <= acc2[31:0]; t_we <= 1'b1;
				hold <= { 31'd0, acc2[32] };
				t_ra_eng <= LIMBS[AW-1:0] + 4'd1;
				st <= S_F2;
			end

			S_F2: begin
				t_wa <= LIMBS[AW-1:0]; t_wd <= t_rd + hold; t_we <= 1'b1;
				st <= S_F3;
			end

			S_F3: begin
				t_wa <= LIMBS[AW-1:0] + 4'd1; t_wd <= 32'd0; t_we <= 1'b1;
				st <= S_NEXT;
			end

			S_NEXT: begin
				if (i == LIMBS - 1) begin
					// The final conditional subtract, T -= N if
					// T >= N.
					//
					// It has to be here, not in software: CIOS leaves
					// T below 2N, which can need LIMBS+1 words, and
					// the extra word is not in the register map. A
					// caller reading only LIMBS words would silently
					// lose it -- which showed up as nine of
					// fifty-one vectors failing, all of them the ones
					// that happened to land above 2^384.
					//
					// rb is reused as the scratch it needs: B has
					// been fully consumed by this point, and one
					// borrowed array is cheaper than a fifth one.
					t_ra_eng <= LIMBS[AW-1:0];
					st <= S_SUB0;
				end else begin
					i <= i + 5'd1;
					b_ra <= (i + 5'd1);
					st <= S_LOAD;
				end
			end

			S_SUB0: begin
				// The word above the result, which decides whether a
				// borrow out of the subtraction actually means T < N.
				hold <= t_rd;
				j <= 5'd0;
				carry <= 32'd0;
				t_ra_eng <= 4'd0;
				n_ra <= 4'd0;
				st <= S_SUB;
			end

			S_SUB: begin
				b_wa <= j[AW-1:0]; b_wd <= as_res; b_we <= 1'b1;
				carry <= { 31'd0, as_out };
				t_ra_eng <= (j + 5'd1);
				n_ra <= (j + 5'd1);
				if (j == LIMBS - 1) begin
					j <= 5'd0;
					b_ra <= 4'd0;
					st <= S_CPY;
				end else j <= j + 5'd1;
			end

			S_CPY: begin
				// Keep the subtracted value unless it borrowed and
				// there was nothing above the result to cover it.
				if (!(carry[0] && (hold == 32'd0))) begin
					t_wa <= j[AW-1:0]; t_wd <= b_rd; t_we <= 1'b1;
				end
				b_ra <= (j + 5'd1);
				if (j == LIMBS - 1) begin
					if (REGFILE && cmd_on) begin
						// The last word's write lands next cycle;
						// S_WB reads word 0 first, so there is no
						// hazard for LIMBS >= 2.
						t_ra_eng <= 4'd0;
						j <= 5'd0;
						st <= S_WB;
					end else st <= S_IDLE;
				end else j <= j + 5'd1;
			end

			// -- the register file's commands -------------------------
			//
			// Copying in: the RAM answers a cycle after the address,
			// so at count j the data is word j-1 and the address sent
			// is word j+1. j runs to LIMBS to write the last word.

			S_XA: if (REGFILE) begin
				if (j != 5'd0) begin
					a_wa <= j[AW-1:0] - 4'd1; a_wd <= rf_q; a_we <= 1'b1;
				end
				rf_ra_eng <= { cmd_ra, j[3:0] + 4'd1 };
				if (j == LIMBS) begin
					rf_ra_eng <= { cmd_rb, 4'd0 };
					j <= 5'd0;
					st <= S_XB;
				end else j <= j + 5'd1;
			end

			S_XB: if (REGFILE) begin
				if (j != 5'd0) begin
					b_wa <= j[AW-1:0] - 4'd1; b_wd <= rf_q; b_we <= 1'b1;
				end
				rf_ra_eng <= { cmd_rb, j[3:0] + 4'd1 };
				if (j == LIMBS) begin
					j <= 5'd0;
					a_ra <= 4'd0;
					b_ra <= 4'd0;
					carry <= 32'd0;
					if (cmd_op == 4'd1) st <= S_CLR;		// MUL: the engine
					else if (cmd_op == 4'd2) st <= S_ADD;
					else st <= S_SBAB;
				end else j <= j + 5'd1;
			end

			// ADD: T = A + B, word by word; the carry becomes
			// T[LIMBS]; then the engine's own final subtract (S_SUB0 ..
			// S_CPY) takes N off if the sum reached it.
			S_ADD: if (REGFILE) begin
				t_wa <= j[AW-1:0]; t_wd <= as_res; t_we <= 1'b1;
				carry <= { 31'd0, as_out };
				a_ra <= (j + 5'd1);
				b_ra <= (j + 5'd1);
				if (j == LIMBS - 1) st <= S_ADDC;
				else j <= j + 5'd1;
			end

			S_ADDC: if (REGFILE) begin
				t_wa <= LIMBS[AW-1:0]; t_wd <= carry; t_we <= 1'b1;
				t_ra_eng <= LIMBS[AW-1:0];
				st <= S_ADDW;
			end

			S_ADDW: if (REGFILE) st <= S_SUB0;		// T[LIMBS]'s write lands this cycle

			// SUB: T = A - B; if that borrowed, T += N.
			S_SBAB: if (REGFILE) begin
				t_wa <= j[AW-1:0]; t_wd <= as_res; t_we <= 1'b1;
				carry <= { 31'd0, as_out };
				a_ra <= (j + 5'd1);
				b_ra <= (j + 5'd1);
				if (j == LIMBS - 1) st <= S_SBN0;
				else j <= j + 5'd1;
			end

			// Always through S_ADDN, adding N or zero: constant time.
			S_SBN0: if (REGFILE) begin
				j <= 5'd0;
				t_ra_eng <= 4'd0;
				n_ra <= 4'd0;
				sb_borrow <= carry[0];
				carry <= 32'd0;
				st <= S_ADDN;
			end

			S_ADDN: if (REGFILE) begin
				t_wa <= j[AW-1:0]; t_wd <= as_res; t_we <= 1'b1;
				carry <= { 31'd0, as_out };
				t_ra_eng <= (j + 5'd1);
				n_ra <= (j + 5'd1);
				if (j == LIMBS - 1) st <= S_WB0;
				else j <= j + 5'd1;
			end

			S_WB0: if (REGFILE) begin
				// one cycle for the last T write to land
				t_ra_eng <= 4'd0;
				j <= 5'd0;
				st <= S_WB;
			end

			// T -> register rd. T reads asynchronously: t_ra_eng set
			// last cycle is this cycle's word.
			S_WB: if (REGFILE) begin
				rf_wa <= { cmd_rd, j[3:0] }; rf_wd <= t_rd; rf_we <= 1'b1;
				t_ra_eng <= (j + 5'd1);
				if (j == LIMBS - 1) begin
					cmd_on <= 1'b0;
					st <= S_IDLE;
				end else j <= j + 5'd1;
			end

			default: ;

			endcase

		end

	end

endmodule
