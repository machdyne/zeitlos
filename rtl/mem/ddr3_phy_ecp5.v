/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 PHY, Lattice ECP5
 *
 * See docs/ddr3.md; how it got here is in docs/ddr3-bringup.md.
 *
 * The STROBE PATH follows LiteDRAM's ECP5 PHY (litedram/phy/
 * ecp5ddrphy.py), the one datapath known to read correctly on this
 * board: its DQSBUFM delay settings, its two-cycle read gate, capture at
 * a fixed latency, its write strobe pattern, and DELAYGs on the clock
 * and command outputs. Everything else is this project's, and follows:
 *
 *   1. ONE PATH for everything leaving the chip. DQ and DM are chosen
 *      by the same beat mux in the same cycle and go through the same
 *      output registers, so they cannot be misaligned against each
 *      other. (In the first attempt they had separate paths, and the
 *      hardware wanted them eight bytes apart.)
 *
 *   2. EVERY read control per lane: gate, READCLKSEL, capture offset.
 *      In the first attempt four controls were wired to lane 0's value
 *      or to nothing, and each produced a clean, uniform sweep map.
 *
 *   3. The burst assembly is rtl/mem/ddr3_rdasm.v, which has a
 *      testbench; nothing that can be simulated lives here, next to
 *      primitives that cannot. Capture is at a FIXED latency after the
 *      read -- not on DATAVALID, which on hardware fired without any
 *      strobe having been caught.
 *
 *   4. Every tuning field is used at its full width, and every index
 *      computed from one is wide enough not to wrap.
 *
 * DM is carried but not used: the controller does read-modify-write
 * and drives ctl_wmask_i to zero. It still goes through the data path,
 * so if masking is ever wanted it cannot be misaligned.
 *
 * -- clocks --
 *
 *   eclk_i  96MHz edge clock = the DRAM clock. Two beats per cycle.
 *   clk_i   48MHz system clock, CLKDIVF of eclk_i, so the two are
 *           phase-locked by construction. Four beats per cycle.
 *
 * A BL8 burst is therefore two system cycles, beats 0-3 then 4-7.
 *
 * -- commands --
 *
 * One per system cycle, on the first of its two DRAM clocks; the
 * second carries a deselect. Hence CL and CWL must both be even (see
 * ddr3_ctrl.v): an odd one would put data half a system cycle off the
 * boundary everything else is aligned to.
 */

`default_nettype none

module ddr3_phy_ecp5 (
	input wire clk_i,
	input wire eclk_i,
	input wire rst_i,
	input wire ddrdel_i,
	input wire pause_i,

	// The init sequence's reset of the edge-clock domain (ddr3_phy_init).
	input wire phy_rst_i,

	// -- controller ----------------------------------------------
	input wire ctl_reset_n_i,
	input wire ctl_cke_i,
	input wire ctl_odt_i,
	input wire ctl_cs_n_i,
	input wire ctl_ras_n_i,
	input wire ctl_cas_n_i,
	input wire ctl_we_n_i,
	input wire [15:0] ctl_a_i,
	input wire [2:0] ctl_ba_i,

	input wire ctl_wren_i,
	input wire [127:0] ctl_wdata_i,
	input wire [15:0] ctl_wmask_i,

	input wire ctl_rden_i,
	output wire ctl_rvalid_o,
	output wire [127:0] ctl_rdata_o,

	// -- tuning ----------------------------------------------------
	//
	// Every field is used at its full width, and every read field is
	// per lane.
	input wire [2:0] cfg_wr_delay_i,   // cycles, command to write data
	input wire [2:0] cfg_rd_gate0_i,   // cycles, command to read gate
	input wire [2:0] cfg_rd_gate1_i,
	input wire [2:0] cfg_rdclksel0_i,  // DQSBUFM read clock phase
	input wire [2:0] cfg_rdclksel1_i,
	input wire [3:0] cfg_rd_off0_i,    // beat offset into the history
	input wire [3:0] cfg_rd_off1_i,

	// -- status ----------------------------------------------------
	output wire [1:0] sts_datavalid_o,
	output reg [1:0] sts_burstdet_o,   // sticky until the next read
	input wire sts_clear_i,

	// -- pins ------------------------------------------------------
	output wire [15:0] ddr3_a,
	output wire [2:0] ddr3_ba,
	output wire ddr3_ras_n,
	output wire ddr3_cas_n,
	output wire ddr3_we_n,
	output wire ddr3_cs_n,
	output wire ddr3_cke,
	output wire ddr3_odt,
	output wire ddr3_reset_n,
	output wire [1:0] ddr3_dm,
	inout wire [15:0] ddr3_dq,
	inout wire [1:0] ddr3_dqs_p,
	output wire ddr3_clk_p
);

	genvar gi;

	// -- reset of every DDR primitive -----------------------------------
	//
	// The x2 input and output registers are gearboxes between the 96MHz
	// edge clock and the 48MHz system clock, and which beat lands in
	// which slot depends on when they leave reset relative to the edge
	// clock. They must be reset WHILE THE EDGE CLOCK IS STOPPED, by the
	// init sequence. LiteX's ECP5 DDR3 boards (lattice_versa_ecp5 in
	// litex-boards) do exactly this: the init sequence's reset is ORed
	// into the system reset -- asserted at once, released through a
	// synchroniser on the system clock -- and the system reset is every
	// DDR primitive's RST.
	//
	// Here they were reset by the SoC reset alone, released whenever the
	// reset counter finished -- at an arbitrary point relative to the
	// edge clock. The same bitstream then trained differently on every
	// boot: nothing at any setting on one, working cells on the next,
	// while the strobe-detection map stayed identical throughout.
	//
	// Asserted at once, released on the system clock, as that
	// synchroniser does. The system clock is stopped along with the
	// edge clock, so the release lands just after both restart.
	reg [1:0] prst;
	always @(posedge clk_i or posedge phy_rst_i) begin
		if (phy_rst_i) prst <= 2'b11;
		else prst <= {prst[0], 1'b0};
	end
	wire prim_rst = rst_i | prst[1];

	// =============================================================
	// Command and address
	//
	// ODDRX2F puts out four half-DRAM-clock values per system cycle.
	// Commands are single data rate, so both halves of the first DRAM
	// clock carry the command and the second clock carries deselect.
	// Everything except CS# may simply hold its value: with CS# high
	// the DRAM ignores the rest.
	// =============================================================
	wire [26:0] cmd_bus = {ctl_reset_n_i, ctl_cke_i, ctl_odt_i,
	                       ctl_ras_n_i, ctl_cas_n_i, ctl_we_n_i,
	                       ctl_ba_i, ctl_a_i, 2'b00};
	wire [26:0] cmd_pin;

	// Every clock and command output passes through a DELAYG, as in
	// LiteDRAM's ECP5 PHY. At DEL_VALUE 0 it adds only the delay
	// element's own fixed propagation -- which is the point: it sets
	// CK and the commands relative to DQS and DQ, whose outputs come
	// from DQS-aligned registers with delay elements of their own. The
	// write strobe's position relative to CK (tDQSS) depends on that
	// relationship, and the known-good datapath has these elements.
	wire [26:0] cmd_oddr;
	wire cs_q, ck_q;

	generate
		for (gi = 2; gi < 27; gi = gi + 1) begin : cmd
			ODDRX2F cmd_oddr (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.D0(cmd_bus[gi]), .D1(cmd_bus[gi]),
				.D2(cmd_bus[gi]), .D3(cmd_bus[gi]),
				.Q(cmd_oddr[gi])
			);
			DELAYG #(.DEL_VALUE(0)) cmd_dly (
				.A(cmd_oddr[gi]), .Z(cmd_pin[gi])
			);
		end
	endgenerate

	// CS# alone differs between the two DRAM clocks.
	ODDRX2F cs_oddr (
		.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
		.D0(ctl_cs_n_i), .D1(ctl_cs_n_i), .D2(1'b1), .D3(1'b1),
		.Q(cs_q)
	);
	DELAYG #(.DEL_VALUE(0)) cs_dly (.A(cs_q), .Z(ddr3_cs_n));

	assign {ddr3_reset_n, ddr3_cke, ddr3_odt,
	        ddr3_ras_n, ddr3_cas_n, ddr3_we_n,
	        ddr3_ba, ddr3_a} = cmd_pin[26:2];

	// CK: low then high in each DRAM clock, so it rises in the middle
	// of the command window -- half a DRAM clock of setup and of hold.
	ODDRX2F ck_oddr (
		.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
		.D0(1'b0), .D1(1'b1), .D2(1'b0), .D3(1'b1),
		.Q(ck_q)
	);
	DELAYG #(.DEL_VALUE(0)) ck_dly (.A(ck_q), .Z(ddr3_clk_p));

	// =============================================================
	// DQS buffers, one per lane
	//
	// Each produces the lane's read and write clocks, its FIFO
	// pointers, and DATAVALID. Every input that could differ between
	// lanes is driven per lane.
	// =============================================================
	wire [1:0] dqs_in;
	wire [1:0] dqsr90, dqsw, dqsw270;
	wire [1:0] rdpntr0, rdpntr1, rdpntr2, wrpntr0, wrpntr1, wrpntr2;
	wire [1:0] datavalid, burstdet;

	reg [1:0] rd_gate;
	wire [2:0] rdclksel [0:1];
	assign rdclksel[0] = cfg_rdclksel0_i;
	assign rdclksel[1] = cfg_rdclksel1_i;

	generate
		for (gi = 0; gi < 2; gi = gi + 1) begin : dqsbuf
			// Read and write DQS delay adjustments, as LiteDRAM's ECP5
			// PHY sets them -- the datapath that has read correctly
			// on this board. Lattice's table: LI adjusts the DQS
			// delay for READ, LO for WRITE. These were left at their
			// defaults (FACTORYONLY, 0), and the first scans showed
			// BURSTDET catching the strobe only sporadically, never
			// on both lanes in the same cell.
			DQSBUFM #(
				.DQS_LI_DEL_ADJ("MINUS"),
				.DQS_LI_DEL_VAL(1),
				.DQS_LO_DEL_ADJ("MINUS"),
				.DQS_LO_DEL_VAL(4)
			) dqsbufm (
				.DQSI(dqs_in[gi]),
				.ECLK(eclk_i), .SCLK(clk_i), .RST(prim_rst),
				.DDRDEL(ddrdel_i), .PAUSE(pause_i),

				// Delays track the DLL code continuously: LOADN low
				// loads it, MOVE low leaves it alone. Per-bit dynamic
				// calibration is for links much faster than this one.
				.RDLOADN(1'b0), .RDMOVE(1'b0), .RDDIRECTION(1'b1),
				.WRLOADN(1'b0), .WRMOVE(1'b0), .WRDIRECTION(1'b1),
				.DYNDELAY0(1'b0), .DYNDELAY1(1'b0), .DYNDELAY2(1'b0),
				.DYNDELAY3(1'b0), .DYNDELAY4(1'b0), .DYNDELAY5(1'b0),
				.DYNDELAY6(1'b0), .DYNDELAY7(1'b0),

				// Per lane.
				.READ0(rd_gate[gi]), .READ1(rd_gate[gi]),
				.READCLKSEL0(rdclksel[gi][0]),
				.READCLKSEL1(rdclksel[gi][1]),
				.READCLKSEL2(rdclksel[gi][2]),

				.DQSR90(dqsr90[gi]),
				.DQSW(dqsw[gi]), .DQSW270(dqsw270[gi]),
				.RDPNTR0(rdpntr0[gi]), .RDPNTR1(rdpntr1[gi]),
				.RDPNTR2(rdpntr2[gi]),
				.WRPNTR0(wrpntr0[gi]), .WRPNTR1(wrpntr1[gi]),
				.WRPNTR2(wrpntr2[gi]),
				.DATAVALID(datavalid[gi]),
				.BURSTDET(burstdet[gi]),
				.RDCFLAG(), .WRCFLAG()
			);
		end
	endgenerate

	assign sts_datavalid_o = datavalid;

	always @(posedge clk_i) begin
		if (rst_i || sts_clear_i || ctl_rden_i)
			sts_burstdet_o <= 2'b00;
		else
			sts_burstdet_o <= sts_burstdet_o | burstdet;
	end

	// =============================================================
	// Write path
	//
	// wr_pipe[k] is "a write was issued k cycles ago". The two data
	// cycles sit at cfg_wr_delay_i and the one after; the DQS preamble
	// half a system cycle before the first.
	// =============================================================
	reg [9:0] wr_pipe;
	reg [127:0] wdata_hold;
	reg [15:0] wmask_hold;

	always @(posedge clk_i) begin
		if (rst_i)
			wr_pipe <= 10'd0;
		else
			wr_pipe <= {wr_pipe[8:0], ctl_wren_i};
		if (ctl_wren_i) begin
			wdata_hold <= ctl_wdata_i;
			wmask_hold <= ctl_wmask_i;
		end
	end

	// wr_pipe[0] is one cycle after the request, so a delay of d means
	// the data leaves d cycles after the WRITE command did.
	wire wr_first  = (cfg_wr_delay_i == 3'd0) ? 1'b0
	               : wr_pipe[cfg_wr_delay_i - 3'd1];
	wire wr_second = wr_pipe[cfg_wr_delay_i];
	wire wr_pre    = (cfg_wr_delay_i < 3'd2) ? 1'b0
	               : wr_pipe[cfg_wr_delay_i - 3'd2];
	// Four-bit index: at a delay of 7, 3'd7 + 3'd1 wraps to 0.
	wire wr_post   = wr_pipe[{1'b0, cfg_wr_delay_i} + 4'd1];
	wire wr_data   = wr_first | wr_second;

	// ONE beat mux for all eighteen output bits: sixteen DQ and two
	// DM. out_bits[b*18 + i] is bit i of beat b.
	wire [143:0] out_bits;
	generate
		for (gi = 0; gi < 8; gi = gi + 1) begin : beat
			assign out_bits[gi*18 +: 16] = wdata_hold[gi*16 +: 16];
			assign out_bits[gi*18 + 16]  = wmask_hold[gi*2];
			assign out_bits[gi*18 + 17]  = wmask_hold[gi*2 + 1];
		end
	endgenerate

	// Beats 0-3 in the first data cycle, 4-7 in the second.
	wire [71:0] cycle_bits = wr_second ? out_bits[143:72]
	                                   : out_bits[71:0];

	wire [17:0] out_q;
	wire [15:0] dq_t;

	generate
		for (gi = 0; gi < 18; gi = gi + 1) begin : outbit
			// DQ bits 0-7 and DM 0 belong to lane 0; 8-15 and DM 1
			// to lane 1. Each is clocked by its own lane's DQSW270.
			ODDRX2DQA oddr (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.DQSW270(dqsw270[(gi < 16) ? gi / 8 : gi - 16]),
				.D0(cycle_bits[0*18 + gi]),
				.D1(cycle_bits[1*18 + gi]),
				.D2(cycle_bits[2*18 + gi]),
				.D3(cycle_bits[3*18 + gi]),
				.Q(out_q[gi])
			);
		end

		for (gi = 0; gi < 16; gi = gi + 1) begin : dqt
			TSHX2DQA tsh (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.DQSW270(dqsw270[gi / 8]),
				.T0(~wr_data), .T1(~wr_data),
				.Q(dq_t[gi])
			);
		end
	endgenerate

	assign ddr3_dm = out_q[17:16];

	// DQS, exactly as LiteDRAM drives it: the output register toggles
	// CONSTANTLY, D0..D3 = 0,1,0,1, and the preamble and postamble are
	// shaped by the tristate alone -- enabled for the second half of
	// the cycle before the data (T1) and the first half of the cycle
	// after it (T0).
	//
	// This was written as 4'b0101, which in Verilog is D0=1, D1=0,
	// D2=1, D3=0: the strobe INVERTED, half a beat out from the
	// configuration that works on this board. Every write would have
	// landed its data in the wrong place, so no read could ever match.
	// Worth saying why it happened: a bit pattern read left to right
	// and a port list read D0 first run in opposite directions, and
	// LiteDRAM writes the same constant as 0b1010, i.e. D0 = bit 0.
	wire [3:0] dqs_d = 4'b1010;          // D3..D0 = 1,0,1,0; D0 = 0
	wire dqs_t0 = ~(wr_data | wr_post);
	wire dqs_t1 = ~(wr_data | wr_pre);
	wire [1:0] dqs_q, dqs_t;

	generate
		for (gi = 0; gi < 2; gi = gi + 1) begin : dqsout
			ODDRX2DQSB oddr (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.DQSW(dqsw[gi]),
				.D0(dqs_d[0]), .D1(dqs_d[1]),
				.D2(dqs_d[2]), .D3(dqs_d[3]),
				.Q(dqs_q[gi])
			);
			TSHX2DQSA tsh (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.DQSW(dqsw[gi]),
				.T0(dqs_t0), .T1(dqs_t1),
				.Q(dqs_t[gi])
			);
			TRELLIS_IO #(.DIR("BIDIR")) dqs_io (
				.B(ddr3_dqs_p[gi]), .I(dqs_q[gi]),
				.T(dqs_t[gi]), .O(dqs_in[gi])
			);
		end
	endgenerate

	// =============================================================
	// Read path
	//
	// The gate opens a programmable number of cycles after the READ
	// command, per lane, for the two cycles of the burst plus one
	// either side. DQSBUFM uses it to decide which strobe edges are
	// data; DATAVALID then frames the burst for the assembly.
	// =============================================================
	// rd_pipe[k]: the read enable delayed k+1 cycles -- the same
	// convention as LiteDRAM's TappedDelayLine, so its numbers carry
	// over unchanged. Its command path has the same structure as this
	// one (DFI phases straight into ODDRX2F), so they carry over in
	// meaning too.
	reg [15:0] rd_pipe;
	always @(posedge clk_i) begin
		if (rst_i)
			rd_pipe <= 16'd0;
		else
			rd_pipe <= {rd_pipe[14:0], ctl_rden_i};
	end

	// Data is captured a FIXED time after the read, as LiteDRAM does
	// (13 cycles there). Here the history ends one cycle later so that
	// LiteDRAM's position falls mid-way through the offset range
	// rather than at an end of it.
	wire rd_capture = rd_pipe[13];

	// The READ pulse to each DQSBUFM: TWO cycles, combinational, as in
	// LiteDRAM's ECP5 PHY -- the read enable delayed by the CAS latency
	// in system cycles, ORed with the same delayed one cycle more.
	// Lattice FPGA-TN-02035 6.2.4: the READ pulse must be asserted for
	// two system cycles before the data returns. The reset value of the
	// gate field (3, rtl/mem/ddr3.v) is exactly LiteDRAM's position for
	// CL=6; the field is per lane and swept by the BIOS.
	//
	// This was three cycles wide and registered -- one cycle later and
	// one wider than the known-good datapath -- and the scans showed
	// reads "completing" at gates where no strobe had been seen.
	//
	// Four-bit indices: a field narrower than the arithmetic done on
	// it wraps.
	wire [3:0] g0 = {1'b0, cfg_rd_gate0_i};
	wire [3:0] g1 = {1'b0, cfg_rd_gate1_i};

	always @(*) begin
		rd_gate[0] = rd_pipe[g0] | rd_pipe[g0 + 4'd1];
		rd_gate[1] = rd_pipe[g1] | rd_pipe[g1 + 4'd1];
	end

	wire [15:0] q0, q1, q2, q3;
	wire [15:0] dq_pad_in, dq_dly;

	generate
		for (gi = 0; gi < 16; gi = gi + 1) begin : inbit
			TRELLIS_IO #(.DIR("BIDIR")) dq_io (
				.B(ddr3_dq[gi]), .I(out_q[gi]),
				.T(dq_t[gi]), .O(dq_pad_in[gi])
			);

			// Aligns DQ with the lane's DQS delay line, as the input
			// register requires in X2 mode.
			DELAYG #(.DEL_MODE("DQS_ALIGNED_X2")) dly_i (
				.A(dq_pad_in[gi]), .Z(dq_dly[gi])
			);

			IDDRX2DQA iddr (
				.SCLK(clk_i), .ECLK(eclk_i), .RST(prim_rst),
				.D(dq_dly[gi]),
				.DQSR90(dqsr90[gi / 8]),
				.RDPNTR0(rdpntr0[gi / 8]), .RDPNTR1(rdpntr1[gi / 8]),
				.RDPNTR2(rdpntr2[gi / 8]),
				.WRPNTR0(wrpntr0[gi / 8]), .WRPNTR1(wrpntr1[gi / 8]),
				.WRPNTR2(wrpntr2[gi / 8]),
				.Q0(q0[gi]), .Q1(q1[gi]), .Q2(q2[gi]), .Q3(q3[gi]),
				.QWL()
			);
		end
	endgenerate

	ddr3_rdasm rdasm (
		.clk_i(clk_i), .rst_i(rst_i),
		.cap_i(rd_capture),
		.q0_i(q0), .q1_i(q1), .q2_i(q2), .q3_i(q3),
		.off0_i(cfg_rd_off0_i), .off1_i(cfg_rd_off1_i),
		.rdata_o(ctl_rdata_o), .rvalid_o(ctl_rvalid_o)
	);

endmodule

`default_nettype wire
